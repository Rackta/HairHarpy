// The Topology room's UV (Viewport::tset.uv — the UV mode): the UV Editor view (a window whose camera is 'uv' — no 3D
// there: the active mesh's UV and the other selected meshes', dim), picking and moving its elements by the mode —
// polygons, edges, UV points (the corners at one vertex with one UV: moved together) — LMB one, Shift add, Ctrl take
// off, a drag on empty space a box, double click the shell (polygons, points) / the edge loop; a drag on the selection
// moves it (the Rotate tool turns it, the Scale tool scales it, about its box's middle; Shift — 15° steps / one axis);
// Alt+LMB / MMB pan, Alt+RMB / the wheel zoom, F frames. The edits (seams, Unfold, Pack, turns, mirrors, align) are one
// undo step each, written as the mesh's new source like any edit of the room. Preserve UVs: uvKeep.
#include "scene/Looks.h"
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QDateTime>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSettings>
#include <QWheelEvent>
#include <array>
#include <map>
#include <memory>
#include <thread>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace hm {

namespace {
// "Let me think…": a small window over the editor while a long pack runs (many passes) — shown at once, before the work blocks the window
struct ThinkBox {
  std::unique_ptr<ui::ThinkScope> s;
  ThinkBox(bool on, const QPoint&) { if (on) { s = std::make_unique<ui::ThinkScope>(QObject::tr("Packing the shells"), 0); s->now(); } }   // (the program's own «Let me think…» window)
};
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
double segDist(double px, double py, double ax, double ay, double bx, double by) {
  double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy, t = l2 > 0 ? std::clamp(((px - ax) * vx + (py - ay) * vy) / l2, 0.0, 1.0) : 0;
  return std::hypot(ax + vx * t - px, ay + vy * t - py);
}
bool inPoly(const PolyMesh& B, int f, double u, double v) {
  bool in = false; int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
  for (int k = s, j = e - 1; k < e; j = k++) { const auto& a = B.cornerUV[size_t(k)]; const auto& b = B.cornerUV[size_t(j)];
    if ((a[1] > v) != (b[1] > v) && u < (double(b[0]) - a[0]) * (v - a[1]) / (double(b[1]) - a[1]) + a[0]) in = !in; }
  return in;
}
const QColor SEL(0xff, 0x9a, 0x2e), SEAM(0xff, 0x5a, 0x4f), BORDER(0x5f, 0xd3, 0xc4);
}  // namespace

// ---------- the mode, the preferences ----------
bool Viewport::uvShown(int id) const {
  if (!uvMode()) return false; const Item* it = scene_->item(id);
  return it && it->type == ItemType::IMesh && it->poly && scene_->isVisible(*it) && (id == topoMesh() || sel_.hasItem(id));
}
QColor Viewport::stretchColor(double q) {
  const double w = q <= 0 ? 1.0 : std::min(1.0, std::abs(std::log2(q)) / (4.0 / 3.0));
  return q < 1 ? QColor::fromRgbF(0.45 + 0.5 * w, 0.45 * (1 - w) + 0.2, 0.45 * (1 - w) + 0.2) : QColor::fromRgbF(0.45 * (1 - w) + 0.2, 0.45 * (1 - w) + 0.35, 0.45 + 0.5 * w);
}
bool Viewport::uvDispShown(int id) const {
  if (room() != "topo") return false; const Item* it = scene_->item(id);
  return it && it->type == ItemType::IMesh && it->poly && scene_->isVisible(*it) && (id == topoMesh() || sel_.hasItem(id));
}
bool Viewport::uvPaneShown() const { for (int i : shownPanes()) if (paneCamera(i).view == "uv") return true; return false; }

void Viewport::setSymmetry(bool on) {
  if (!on && tset.sym && uvMode() && tsel_.id >= 0) {              // (Symmetry off: the mirror shown as picked becomes really picked — on its own side)
    std::set<int> pts; if (tKey() == 'v') { pts = uvPoints(); for (int g : uvGhostPts()) pts.insert(g); }
    const TSel o = uvOpSel(); tsel_.f = o.f; tsel_.e = o.e; tsel_.v = o.v; tsel_.eSide = o.eSide; tsel_.ver++; if (!pts.empty()) { uvPts_ = pts; uvPtsVer_ = tsel_.ver; } tselNote(); updateGizmo(); }
  tset.sym = on; saveTopoPrefs(); uvChanged(); topoRev_++; requestUpdate();   // (the model's −X half tinted / not in the UV mode)
  ui::toast(on ? (uvMode() ? tr("Symmetry on — the UV too: mirror shells follow across U = %1").arg(uvo.symAxis) : tr("Symmetry on")) : tr("Symmetry off"));
  emit topoState();
}
void Viewport::setUvMode(bool on) {
  if (on == tset.uv) return;
  tset.uv = on; saveTopoPrefs();
  if (on) {
    if (!uvPaneShown()) { openUvEditor(); uvSplitByMode_ = true; }
    int m = topoMesh(); if (m < 0) ui::toast(tr("UV mode — select a mesh: its UV are shown in the UV Editor"));
    else if (!uv::has(*baseOf(*scene_->item(m)))) ui::toast(tr("UV mode — this mesh has no UV yet: mark seams (Edge mode) and Unfold"));
    else ui::toast(tr("UV mode"));
  } else {
    // (the window the mode opened goes back to what it was)
    if (uvSplitByMode_ && uvWasPane_ >= 0 && uvWasPane_ < 4 && paneCamera(uvWasPane_).view == "uv") {
      setPaneView(uvWasPane_, uvWasView_.isEmpty() ? QString("persp") : uvWasView_);
      if (!uvWasSplit_) backToSingle(uvWasPane_ == act_ ? (act_ == 0 ? 1 : 0) : act_);
    }
    uvSplitByMode_ = false; ui::toast(tr("UV mode off"));
  }
  uvChanged(); emit topoState();
}
void Viewport::setPreserveUv(bool on) { tset.keepUv = on; saveTopoPrefs(); ui::toast(on ? tr("Preserve UVs on: moved vertices keep their place in the texture") : tr("Preserve UVs off")); emit topoState(); }

void Viewport::openUvEditor() {
  if (uvPaneShown()) { requestUpdate(); return; }               // (one is shown: no other window turned into it)
  uvWasSplit_ = split_; uvWasLayout_ = layout_;
  int pane = -1;
  if (!split_) { const int keep = act_; setSplitLayout("2"); setSplit(true); pane = keep == 1 ? 0 : 1; if (act_ != keep) activatePane(keep); }
  else { auto s = shownPanes(); for (auto it = s.rbegin(); it != s.rend(); ++it) if (*it != act_) { pane = *it; break; } if (pane < 0) pane = s.back(); }
  uvWasPane_ = pane; uvWasView_ = paneCamera(pane).view;
  setPaneView(pane, "uv");
  if (pane == act_) cam_.uvPpu = 0; else panes_[size_t(pane)].uvPpu = 0;
  overlayRev_++; requestUpdate();
}

void Viewport::saveUvPrefs() const {
  QSettings().setValue("topo/uv", QJsonDocument(QJsonObject{{"edChecker", uvo.checker}, {"packFree", uvo.packFree}, {"c3d", uvo.c3d}, {"c3dv2", true}, {"s3d", uvo.s3d}, {"dispOp", uvo.dispOp}, {"tex", uvo.tex}, {"stretch", uvo.stretch}, {"rotate", uvo.rotate}, {"packPasses", uvo.packPasses}, {"packFinal", uvo.packFinal}, {"soloSet", uvSetIso},
                                                            {"packEqual", uvo.packEqual}, {"cells", uvo.density}, {"strAmt", uvo.strAmt}, {"strUp", uvo.strUp}, {"strSpread", uvo.strSpread}, {"rectMode", uvo.rectMode}, {"strSpreadv2", true}, {"anch", uvo.anch}, {"symUnfold", uvo.symUnfold}, {"symTint", uvo.symTint}, {"pivShells", uvo.pivShells}, {"diag", uvo.diag}, {"stretchAngle", uvo.stretchAngle}, {"symAxis", uvo.symAxis}, {"warpN", uvo.warpN}, {"pad", uvo.pad}, {"angle", uvo.angle}}).toJson(QJsonDocument::Compact));
}

// ---------- the active mesh's points and shells ----------
const Viewport::UvCache* Viewport::uvCache() const {
  int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return nullptr;
  auto B = baseSp(*it); if (!B) return nullptr;
  if (uvc_.base.lock() != B || uvc_.stamp != uvStamp_ || !uvc_.T) {
    uvc_ = {}; uvc_.base = B; uvc_.stamp = uvStamp_; uvc_.T = topo::ttopo(B); if (!uvc_.T) return nullptr;
    uvc_.P = uv::points(*B, *uvc_.T); uvc_.S = uv::shells(*B, *uvc_.T, uvc_.P);
    uvc_.seam = uv::seamFlags(*B, *uvc_.T); uvc_.cont.resize(size_t(uvc_.T->ne)); for (int e = 0; e < uvc_.T->ne; e++) uvc_.cont[size_t(e)] = uv::continuous(*B, *uvc_.T, uvc_.P, e);
  }
  return &uvc_;
}
// Symmetry on: the polygons on the −X side that have a mirror twin (+X) — burgundy, in the UV Editor and on the model
// alike (half of a symmetric mesh; a part with no mirror stays as it is)
std::vector<char> Viewport::uvMinusXFaces() const {
  const UvCache* c = uvCache(); if (!c) return {}; const auto B = uvc_.base.lock(); if (!B) return {};
  const topo::TTopo& T = *c->T; if (!uvc_.mcMade) { uvc_.mc = uv::mirrorCorners(*B, T); uvc_.mcMade = true; }
  const double eps = std::max(1e-9, B->bounds().size().length() * 1e-4); std::vector<char> o(size_t(T.nf), 0);
  for (int f = 0; f < T.nf; f++) { const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; if (s >= e || size_t(s) >= uvc_.mc.size() || uvc_.mc[size_t(s)] < 0) continue;
    double x = 0; for (int k = s; k < e; k++) x += B->pos[size_t(T.fv[size_t(k)])].x; if (x / (e - s) < -eps) o[size_t(f)] = 1; }
  return o;
}
std::set<int> Viewport::uvPoints() {
  const UvCache* c = uvCache(); if (!c) return {};
  if (uvPtsVer_ == tsel_.ver && tsel_.id == topoMesh() && (uvPts_.empty() || *uvPts_.rbegin() < c->P.count())) return uvPts_;
  std::set<int> o; if (tsel_.id != topoMesh()) return o;
  for (int p = 0; p < c->P.count(); p++) if (tsel_.v.count(c->P.vert[size_t(p)])) o.insert(p);
  return o;
}

// ---------- the editor's view ----------
QPointF Viewport::uvToPx(double u, double v) const {
  const double W = paneW(), H = paneH(), s = cam_.uvScale(W, H);
  return {W / 2 + (u - cam_.uvCx) * s, H / 2 - (v - cam_.uvCy) * s * cam_.uvAsp};   // (V the texture's height to width shorter: a texel square)
}
std::array<double, 2> Viewport::pxToUv(const QPointF& p) const {
  const double W = paneW(), H = paneH(), s = cam_.uvScale(W, H);
  return {cam_.uvCx + (p.x() - W / 2) / s, cam_.uvCy - (p.y() - H / 2) / (s * cam_.uvAsp)};
}
void Viewport::uvWheel(QWheelEvent* e) {
  const double notches = e->angleDelta().y() / 120.0; if (notches == 0) return;
  const double W = paneW(), H = paneH(); auto at = pxToUv(e->position());
  double s = cam_.uvScale(W, H) * std::pow(1.18, notches); s = std::clamp(s, 4.0, 400000.0); cam_.uvPpu = s;
  cam_.uvCx = at[0] - (e->position().x() - W / 2) / s; cam_.uvCy = at[1] + (e->position().y() - H / 2) / (s * cam_.uvAsp);
  requestUpdate();
}
void Viewport::uvFrame() {
  if (uvTiles) { uvTilesFrame(); return; }
  const double W = paneW(), H = paneH(); std::array<double, 4> b{0, 0, 1, 1};
  int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (it && uv::has(*baseOf(*it))) {
    auto mv = uvMoving(); std::vector<int> all;
    if (mv.empty()) { const PolyMesh& B = *baseOf(*it); const auto h = uvHidden(*it, B);   // (the picked set / mat: only it)
      for (int f = 0; f < B.faceCount(); f++) if (h.empty() || !h[size_t(f)]) for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) all.push_back(k);
      if (all.empty()) { all.resize(B.faceVerts.size()); std::iota(all.begin(), all.end(), 0); } }
    b = uv::box(*baseOf(*it), mv.empty() ? all : mv);
  }
  double w = std::max(b[2] - b[0], 1e-4), h = std::max(b[3] - b[1], 1e-4);
  cam_.uvPpu = std::clamp(std::min(W / (w * 1.15), H / (h * cam_.uvAsp * 1.15)), 4.0, 400000.0); cam_.uvCx = (b[0] + b[2]) / 2; cam_.uvCy = (b[1] + b[3]) / 2;
  requestUpdate();
}

// ---------- picking ----------
Viewport::UvPick Viewport::uvPickAt(const QPointF& mp) const {
  UvPick r; int m = topoMesh(); if (m < 0) return r; const Item* it = scene_->item(m); const PolyMesh& B = *baseOf(*it);
  if (!uv::has(B)) return r; const UvCache* c = uvCache(); if (!c) return r; const topo::TTopo& T = *c->T;
  const std::vector<char> hidF = uvHidden(*it, B); auto shown = [&](int f) { return hidF.empty() || size_t(f) >= hidF.size() || !hidF[size_t(f)]; };
  const char key = tKey(); auto at = pxToUv(mp);
  if (key == 'f') { for (int f = 0; f < T.nf && r.f < 0; f++) if (shown(f) && inPoly(B, f, at[0], at[1])) r.f = f; return r; }
  if (key == 'e') { double bd = 8;
    for (int f = 0; f < T.nf; f++) if (shown(f)) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      QPointF a = uvToPx(B.cornerUV[size_t(k)][0], B.cornerUV[size_t(k)][1]), b = uvToPx(B.cornerUV[size_t(nk)][0], B.cornerUV[size_t(nk)][1]);
      double d = segDist(mp.x(), mp.y(), a.x(), a.y(), b.x(), b.y()); if (d < bd) { bd = d; r.e = T.ce[size_t(k)]; r.ek = k; } }
    return r; }
  double bd = 9;
  for (int p = 0; p < c->P.count(); p++) { int k = c->P.list[size_t(c->P.off[size_t(p)])]; if (!shown(T.cf[size_t(k)])) continue;
    QPointF a = uvToPx(B.cornerUV[size_t(k)][0], B.cornerUV[size_t(k)][1]); double d = std::hypot(a.x() - mp.x(), a.y() - mp.y()); if (d < bd) { bd = d; r.p = p; } }
  return r;
}
std::vector<int> Viewport::uvBox(double x0, double y0, double x1, double y1, char key) const {
  std::vector<int> o; int m = topoMesh(); if (m < 0) return o; const Item* it = scene_->item(m); const PolyMesh& B = *baseOf(*it);
  const UvCache* c = uvCache(); if (!c || !uv::has(B)) return o; const topo::TTopo& T = *c->T;
  uvEdgeSideTmp_.clear(); auto a = pxToUv({x0, y1}), b = pxToUv({x1, y0}); auto in = [&](double u, double v) { return u >= a[0] && u <= b[0] && v >= a[1] && v <= b[1]; };
  const std::vector<char> hidF = uvHidden(*it, B); auto shown = [&](int f) { return hidF.empty() || size_t(f) >= hidF.size() || !hidF[size_t(f)]; };
  if (key == 'f') { for (int f = 0; f < T.nf; f++) if (shown(f)) { double u = 0, v = 0; int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; for (int k = s; k < e; k++) { u += B.cornerUV[size_t(k)][0]; v += B.cornerUV[size_t(k)][1]; } if (in(u / (e - s), v / (e - s))) o.push_back(f); } }
  else if (key == 'e') { std::set<int> es; for (int f = 0; f < T.nf; f++) if (shown(f)) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      if (in((B.cornerUV[size_t(k)][0] + B.cornerUV[size_t(nk)][0]) / 2.0, (B.cornerUV[size_t(k)][1] + B.cornerUV[size_t(nk)][1]) / 2.0)) { es.insert(T.ce[size_t(k)]); uvEdgeSideTmp_[T.ce[size_t(k)]].insert(f); } } o.assign(es.begin(), es.end()); }   // (f: the shell side it was found on)
  else for (int p = 0; p < c->P.count(); p++) { int k = c->P.list[size_t(c->P.off[size_t(p)])]; if (shown(T.cf[size_t(k)]) && in(B.cornerUV[size_t(k)][0], B.cornerUV[size_t(k)][1])) o.push_back(p); }
  return o;
}
std::vector<int> Viewport::uvMoving() const {
  int m = topoMesh(); const UvCache* c = uvCache(); if (m < 0 || !c || tsel_.id != m) return {};
  const topo::TTopo& T = *c->T; const char key = tKey(); std::vector<int> ks;
  if (key == 'f') ks = uv::faceCorners(T, {tsel_.f.begin(), tsel_.f.end()});
  else if (key == 'e') {                                       // (an edge picked on one shell: that side only)
    for (int e : tsel_.e) if (e >= 0 && e < T.ne) for (int f : {T.ef0[size_t(e)], T.ef1[size_t(e)]}) if (f >= 0 && (c->cont[size_t(e)] || uvSideOn(tsel_, e, f))) for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { int k = uv::cornerAt(T, f, v); if (k >= 0) ks.push_back(k); } }
  else { auto pts = const_cast<Viewport*>(this)->uvPoints(); for (int p : pts) for (int i = c->P.off[size_t(p)]; i < c->P.off[size_t(p) + 1]; i++) ks.push_back(c->P.list[size_t(i)]); }
  std::vector<int> out = uv::pointCorners(c->P, ks);
  // (a set worked in: only its polygons' corners move — what else is picked is left where it lies)
  if (const Item* it = scene_->item(m)) { const PolyMesh& B = *baseOf(*it); if (const int ws = uvWorkSet(m, B); ws >= 0 && topo::hasUvSets(B))
    out.erase(std::remove_if(out.begin(), out.end(), [&](int k) { const int f = size_t(k) < T.cf.size() ? T.cf[size_t(k)] : -1; return f < 0 || !(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> ws & 1u)); }), out.end()); }
  return out;
}

// ---------- the selection tools in the editor (the Topology room's Select tools: Lasso, Select Cursor, Fill, Shells, Loop, Ring) ----------
const Viewport::UvCache* Viewport::uvCacheFor(int m, UvCache& tmp) const {
  const Item* it = scene_->item(m); if (!it) return nullptr; auto B = baseSp(*it); if (!B) return nullptr;
  tmp = {}; tmp.base = B; tmp.T = topo::ttopo(B); if (!tmp.T) return nullptr; tmp.P = uv::points(*B, *tmp.T); tmp.S = uv::shells(*B, *tmp.T, tmp.P);
  tmp.seam = uv::seamFlags(*B, *tmp.T); tmp.cont.resize(size_t(tmp.T->ne)); for (int e = 0; e < tmp.T->ne; e++) tmp.cont[size_t(e)] = uv::continuous(*B, *tmp.T, tmp.P, e);
  return &tmp;
}
// the elements (polygons / edges / points, by key) whose middle the test says is inside — in screen pixels
std::vector<int> Viewport::uvArea(const std::function<bool(double, double)>& inPx, char key, int mesh) const {
  std::vector<int> o; uvEdgeSideTmp_.clear(); int m = mesh >= 0 ? mesh : topoMesh(); if (m < 0) return o; const Item* it = scene_->item(m); const PolyMesh& B = *baseOf(*it);
  UvCache loc; const UvCache* c = m == topoMesh() ? uvCache() : uvCacheFor(m, loc); if (!c || !uv::has(B)) return o; const topo::TTopo& T = *c->T;
  const std::vector<char> hidF = uvHidden(*it, B); auto shown = [&](int f) { return hidF.empty() || size_t(f) >= hidF.size() || !hidF[size_t(f)]; };
  auto in = [&](double u, double v) { const QPointF p = uvToPx(u, v); return inPx(p.x(), p.y()); };
  if (key == 'f') { for (int f = 0; f < T.nf; f++) if (shown(f)) { double u = 0, v = 0; int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; for (int k = s; k < e; k++) { u += B.cornerUV[size_t(k)][0]; v += B.cornerUV[size_t(k)][1]; } if (in(u / (e - s), v / (e - s))) o.push_back(f); } }
  else if (key == 'e') { std::set<int> es; for (int f = 0; f < T.nf; f++) if (shown(f)) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      if (in((B.cornerUV[size_t(k)][0] + B.cornerUV[size_t(nk)][0]) / 2.0, (B.cornerUV[size_t(k)][1] + B.cornerUV[size_t(nk)][1]) / 2.0)) { es.insert(T.ce[size_t(k)]); uvEdgeSideTmp_[T.ce[size_t(k)]].insert(f); } } o.assign(es.begin(), es.end()); }   // (the side it was found on: a lasso or the brush's dab picks that shell's edge only)
  else for (int p = 0; p < c->P.count(); p++) { int k = c->P.list[size_t(c->P.off[size_t(p)])]; if (shown(T.cf[size_t(k)]) && in(B.cornerUV[size_t(k)][0], B.cornerUV[size_t(k)][1])) o.push_back(p); }
  return o;
}
// an area pick on every selected mesh that has UV (Polygon / Edge mode): each mesh keeps its own picks; the active one stays the active one
void Viewport::uvAreaApply(const std::function<bool(double, double)>& inPx, const QString& mode) {
  const int act = topoMesh(); const char key = tKey(); size_t total = 0;
  for (int id : uvMeshes()) { auto list = uvArea(inPx, key, id); if (key == 'f') { const auto pl = uvProjArea(id, inPx); list.insert(list.end(), pl.begin(), pl.end()); } total += list.size();
    if (id == act) { uvApplyList(list, mode); continue; }
    tselApply(id, list, mode, key); if (key == 'e') uvNoteSides(list, mode); }
  tselActivate(act); ui::hud(tr("%1 %2").arg(total).arg(mode == "remove" ? tr("deselected") : tr("selected")), 900);
}
// a list of elements (by the mode's key) taken into the selection: set / add / remove
void Viewport::uvApplyList(const std::vector<int>& list, const QString& mode) {
  const int am = topoMesh(); if (am < 0) return; const char key = tKey();
  if (key == 'v') {
    const UvCache* c = uvCache(); if (!c) return;
    std::set<int> pts = mode == "set" ? std::set<int>{} : uvPoints();
    for (int p : list) { if (mode == "remove") pts.erase(p); else pts.insert(p); }
    std::set<int> vs; for (int p : pts) vs.insert(c->P.vert[size_t(p)]);
    tselApply(am, {vs.begin(), vs.end()}, "set", 'v'); uvPts_ = pts; uvPtsVer_ = tsel_.ver;
  } else { tselApply(am, list, mode); if (key == 'e') uvNoteSides(list, mode); }
}
// the UV side each picked edge was picked on (the shell the user works with): kept for the edge constraints, for this very selection only
// the edges picked as a run (a loop) on one shell: each is noted on that shell's side — else a loop over a seam lit both shells
void Viewport::uvSidesOnShell(const topo::TTopo& T, const uv::Shells& S, const std::vector<int>& edges, int shell, const QString& mode) {
  if (shell < 0) return; uvEdgeSideTmp_.clear();
  for (int e : edges) for (int f : {T.ef0[size_t(e)], T.ef1[size_t(e)]}) if (f >= 0 && S.of[size_t(f)] == shell) uvEdgeSideTmp_[e].insert(f);
  uvNoteSides(edges, mode);
}
void Viewport::uvNoteSides(const std::vector<int>& edges, const QString& mode) {
  if (mode == "set") tsel_.eSide.clear();
  for (int e : edges) { const auto g = uvEdgeSideTmp_.find(e);
    if (mode == "remove") {                                      // (one side taken off: the edge stays picked on its other side)
      const auto h = tsel_.eSide.find(e); if (h == tsel_.eSide.end()) continue; if (g != uvEdgeSideTmp_.end()) for (int f : g->second) h->second.erase(f); else h->second.clear();
      if (h->second.empty()) tsel_.eSide.erase(h); else tsel_.e.insert(e); continue; }
    if (g != uvEdgeSideTmp_.end()) tsel_.eSide[e].insert(g->second.begin(), g->second.end()); }
}
// the mirror of the picked sides: each side's polygon mirrored, the edge mirrored within it (an edge on the plane: the same edge, its other side)
std::map<int, std::set<int>> Viewport::uvMirrorSides(int m, const TSel& s) const {
  std::map<int, std::set<int>> out; const Item* it = scene_->item(m); if (!it) return out; const auto T = topo::ttopo(it->base ? it->base : it->poly); if (!T) return out;
  auto hasEdge = [&](int f, int e) { for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) if (T->ce[size_t(k)] == e) return true; return false; };
  for (int e : s.e) { if (e < 0 || e >= T->ne) continue; const auto mE = tSymList(m, {e}, 'e'); const auto g = s.eSide.find(e);
    if (g == s.eSide.end()) { if (!mE.empty()) out[mE[0]]; continue; }
    for (int f : g->second) { const auto mF = tSymList(m, {f}, 'f'); const int fm = mF.empty() ? f : mF[0]; const int x = mE.empty() ? e : mE[0]; if (hasEdge(fm, x)) out[x].insert(fm); } }
  return out;
}
Viewport::TSel Viewport::uvOpSel() const {
  TSel o = tsel_; if (!tset.sym || !uvMode() || tsel_.id < 0) return o; const int m = tsel_.id;
  for (const char k : {'f', 'e', 'v'}) { std::set<int>& s = k == 'f' ? o.f : k == 'e' ? o.e : o.v; if (s.empty()) continue; for (int x : tSymList(m, std::vector<int>(s.begin(), s.end()), k)) s.insert(x); }
  for (const auto& [e, fs] : uvMirrorSides(m, tsel_)) { if (fs.empty()) { o.eSide.erase(e); continue; } o.eSide[e].insert(fs.begin(), fs.end()); }
  return o;
}
std::set<int> Viewport::uvGhostPts() const {
  std::set<int> out; const UvCache* c = uvCache(); const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!c || !it || !tset.sym) return out;
  { auto tmp = baseOf(*it)->cornerUV; uvSymApply(tmp); }         // (the corners' mirror map made)
  for (int p : const_cast<Viewport*>(this)->uvPoints()) for (int i = c->P.off[size_t(p)]; i < c->P.off[size_t(p) + 1]; i++) { const int k = c->P.list[size_t(i)]; const int q = size_t(k) < uvSymMap_.part.size() ? uvSymMap_.part[size_t(k)] : -1; if (q >= 0) out.insert(c->P.of[size_t(q)]); }
  return out;
}
std::set<std::pair<int, int>> Viewport::uvSelPointSides(const TSel& s, const topo::TTopo& T, const uv::Shells& S) const {
  std::set<std::pair<int, int>> out, seen; const char key = tKey();
  auto put = [&](int v, int f) { if (f >= 0 && seen.insert({v, S.of[size_t(f)]}).second) out.insert({v, f}); };
  if (key == 'f') { for (int f : s.f) if (f < T.nf) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) put(T.fv[size_t(k)], f); }
  else if (key == 'e') { for (int e : s.e) if (e < T.ne) for (int f : {T.ef0[size_t(e)], T.ef1[size_t(e)]}) if (f >= 0 && uvSideOn(s, e, f)) { put(T.ea[size_t(e)], f); put(T.eb[size_t(e)], f); } }
  else if (const UvCache* c = uvCache()) { auto pts = const_cast<Viewport*>(this)->uvPoints(); for (int g : uvGhostPts()) pts.insert(g); for (int p : pts) { const int k = c->P.list[size_t(c->P.off[size_t(p)])]; put(c->P.vert[size_t(p)], T.cf[size_t(k)]); } }
  return out;
}
// polygons taken into the selection as the mode's elements (the polygons themselves; their edges; their points)
void Viewport::uvApplyFaces(const std::vector<int>& fs, const QString& mode) {
  const int am = topoMesh(); const UvCache* c = uvCache(); if (am < 0 || !c) return; const topo::TTopo& T = *c->T; const char key = tKey();
  if (key == 'f') { tselApply(am, fs, mode, 'f'); return; }
  uvEdgeSideTmp_.clear(); if (key == 'e') { std::set<int> es; for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { es.insert(T.ce[size_t(k)]); uvEdgeSideTmp_[T.ce[size_t(k)]].insert(f); } tselApply(am, {es.begin(), es.end()}, mode, 'e'); uvNoteSides({es.begin(), es.end()}, mode); return; }
  std::set<int> ps; for (int k : uv::faceCorners(T, fs)) ps.insert(c->P.of[size_t(k)]);
  uvApplyList({ps.begin(), ps.end()}, mode);
}
// the whole UV shell of a polygon
std::vector<int> Viewport::uvShellOf(int f0) const {
  std::vector<int> fs; const UvCache* c = uvCache(); if (!c || f0 < 0 || f0 >= c->T->nf) return fs;
  const int s = c->S.of[size_t(f0)]; for (int f = 0; f < c->T->nf; f++) if (c->S.of[size_t(f)] == s) fs.push_back(f);
  return fs;
}
int Viewport::uvFaceAt(const QPointF& mp) const {
  int m = topoMesh(); if (m < 0) return -1; const Item* it = scene_->item(m); const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) return -1;
  const UvCache* c = uvCache(); if (!c) return -1; const std::vector<char> hidF = uvHidden(*it, B); auto at = pxToUv(mp);
  for (int f = 0; f < c->T->nf; f++) if ((hidF.empty() || size_t(f) >= hidF.size() || !hidF[size_t(f)]) && inPoly(B, f, at[0], at[1])) return f;
  return -1;
}
// the Select Cursor's dab: what is under its circle taken in
void Viewport::uvPaintAt(const QPointF& mp) {
  const char key = tKey(); const double R = key == 'e' ? 6 : 14; const QString mode = uvd_.mode;   // (edges: a narrow dab — a wide one took the neighbouring small edges too)
  if (key != 'v') { const QString md = mode; uvAreaApply([mp, R](double x, double y) { return std::hypot(x - mp.x(), y - mp.y()) <= R; }, md == "set" ? "set" : md); if (md == "set") uvd_.mode = "add"; return; }   // (the dab on every selected mesh)
  auto list = uvArea([mp, R](double x, double y) { return std::hypot(x - mp.x(), y - mp.y()) <= R; }, key);
  const UvPick pk = uvPickAt(mp); const int el = key == 'f' ? pk.f : key == 'e' ? pk.e : pk.p; if (el >= 0 && std::find(list.begin(), list.end(), el) == list.end()) list.push_back(el);
  if (mode == "set") { uvApplyList(list, "set"); uvd_.mode = "add"; return; }   // (the first dab replaces, the rest add)
  if (!list.empty()) uvApplyList(list, mode);
}
// the edge loop through edge el, on the side of corner ek only — from edge to edge through the UV point they share there (a corner k's edge runs from
// point P.of[k] to the next corner's; a seam edge is two of them, one on each side): a sleeve's seam has both sides on one shell, the clicked one kept
// (2026-11-10). Fills uvEdgeSideTmp_ with the sides taken
std::vector<int> Viewport::uvLoopOnSide(int el, int ek) {
  const UvCache* c = uvCache(); if (!c || el < 0 || ek < 0) return {}; const topo::TTopo& T = *c->T;
  auto nextK = [&](int k) { const int f = T.cf[size_t(k)]; return k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; };
  std::set<int> loopE; { const auto L = topo::edgeLoop(T, el); loopE.insert(L.begin(), L.end()); } loopE.insert(el);
  std::map<int, std::vector<int>> atPt; for (int k = 0; k < int(T.cf.size()); k++) if (loopE.count(T.ce[size_t(k)])) { atPt[c->P.of[size_t(k)]].push_back(k); atPt[c->P.of[size_t(nextK(k))]].push_back(k); }
  std::set<int> seenE{el}; std::vector<int> st{ek}, outE{el}; uvEdgeSideTmp_.clear(); uvEdgeSideTmp_[el].insert(T.cf[size_t(ek)]);
  while (!st.empty()) { const int k = st.back(); st.pop_back();
    for (int p : {c->P.of[size_t(k)], c->P.of[size_t(nextK(k))]}) for (int k2 : atPt[p]) { const int e2 = T.ce[size_t(k2)]; if (seenE.count(e2)) continue; seenE.insert(e2); outE.push_back(e2); uvEdgeSideTmp_[e2].insert(T.cf[size_t(k2)]); st.push_back(k2); } }
  if (std::getenv("UVDBG")) std::fprintf(stderr, "uvLoopOnSide e%d k%d: loop %zu edges, on the side %zu\n", el, ek, loopE.size(), outE.size());
  return outE;
}
// a press with a Select tool (not the plain rectangle): true when it took the press
bool Viewport::uvToolPress(const QPointF& mp, bool ctrl, bool shift) {
  if (ctrl && shift) return false;                               // (Ctrl+Shift: the shortest path from the last picked — the editor's own handling, whichever Select tool)
  const QString st = tset.selTool; const QString mode = ctrl ? "remove" : shift ? "add" : "set";
  const int am = topoMesh(); const UvCache* c = uvCache(); if (am < 0 || !c) return false; const topo::TTopo& T = *c->T; const char key = tKey();
  const UvPick pk = uvPickAt(mp); const int el = key == 'f' ? pk.f : key == 'e' ? pk.e : pk.p;
  const bool onSel = el >= 0 && !ctrl && !shift && (key == 'f' ? tsel_.f.count(el) > 0 : key == 'e' ? tsel_.e.count(el) > 0 && (pk.ek < 0 || c->cont[size_t(el)] || uvSideOn(tsel_, el, T.cf[size_t(pk.ek)])) : uvPoints().count(el) > 0);
  if (st == "lasso") { uvd_.type = 11; uvd_.mode = mode; uvd_.pts = {{mp.x(), mp.y()}}; drag_ = Drag::Uv; return true; }
  if (st == "cursor") {
    if (onSel) return false;                                    // (a drag on the selection moves it)
    if (el >= 0) {                                              // (a click on an element picks it itself — the brush went by the polygons' middles, and a big polygon was missed; a second click soon after: the shell / the edge loop)
      const qint64 now = QDateTime::currentMSecsSinceEpoch(); const bool dbl = uvClick_.el == el && uvClick_.key == key && now - uvClick_.t < 400; uvClick_ = dbl ? UvClick{} : UvClick{el, key, now};
      if (dbl) {
        if (key == 'e' && pk.ek >= 0) { const auto L = uvLoopOnSide(el, pk.ek);   /* (the loop stays on the side it was picked on) */ tselApply(am, L, "add", 'e'); uvNoteSides(L, "add"); ui::hud(tr("Edge loop: %1").arg(L.size()), 1000); }
        else if (key == 'e') { const auto L = topo::edgeLoop(T, el); tselApply(am, L, "add", 'e'); ui::hud(tr("Edge loop: %1").arg(L.size()), 1000); }
        else { const int f0 = key == 'f' ? el : T.cf[size_t(c->P.list[size_t(c->P.off[size_t(el)])])]; const auto fs = uvShellOf(f0);
          if (key == 'f') uvApplyFaces(fs, "add"); else { std::set<int> ps = uvPoints(); for (int k : uv::faceCorners(T, fs)) ps.insert(c->P.of[size_t(k)]); uvApplyList({ps.begin(), ps.end()}, "set"); }
          ui::hud(tr("Shell: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"), 1000); }
        uvd_.type = 14; drag_ = Drag::Uv; return true; }
      if (key == 'f') uvApplyFaces({el}, mode);
      else if (key == 'e') { tselApply(am, {el}, mode, 'e'); if (pk.ek >= 0) { uvEdgeSideTmp_.clear(); uvEdgeSideTmp_[el].insert(T.cf[size_t(pk.ek)]); uvNoteSides({el}, mode); } }
      else { std::set<int> ps = ctrl || shift ? uvPoints() : std::set<int>{}; if (ctrl) ps.erase(el); else ps.insert(el); uvApplyList({ps.begin(), ps.end()}, "set"); }
      if (key == 'v') uvLastPt_ = el; else tsel_.last = el;     // (where Ctrl+Shift draws the shortest path from)
    }
    uvd_.type = 12; uvd_.mode = el >= 0 && mode == "set" ? QString("add") : mode; drag_ = Drag::Uv; if (el < 0) uvPaintAt(mp);   /* (a click picks the element under it alone; only a drag paints — the dab at the press took everything near and wiped the side of the pick) */ if (el >= 0) { if (key == 'v') uvLastPt_ = el; else tsel_.last = el; } return true; }
  if (st == "fill") {
    const int f = uvFaceAt(mp);
    if (f >= 0) { const auto fs = (!fillHas("hard") && !fillHas("crease") && !fillHas("group")) ? uvShellOf(f) : fillRegion(am, f); uvApplyFaces(fs, mode); ui::hud(tr("Shell: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"), 1000); uvd_.type = 14; drag_ = Drag::Uv; return true; }
    return false; }
  if (st == "loop" || st == "ring") {
    int e = -1, eK = -1; double bd = 10; const PolyMesh& B = *baseOf(*scene_->item(am));   // (the edge nearest the pointer, whatever the mode)
    for (int f = 0; f < T.nf; f++) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      QPointF a = uvToPx(B.cornerUV[size_t(k)][0], B.cornerUV[size_t(k)][1]), b = uvToPx(B.cornerUV[size_t(nk)][0], B.cornerUV[size_t(nk)][1]);
      double d = segDist(mp.x(), mp.y(), a.x(), a.y(), b.x(), b.y()); if (d < bd) { bd = d; e = T.ce[size_t(k)]; eK = k; } }
    if (e < 0) return false;
    if (st == "ring") { const int f0 = T.ef0[size_t(e)] >= 0 ? T.ef0[size_t(e)] : T.ef1[size_t(e)]; const auto L = f0 >= 0 ? topo::faceLoop(T, f0, e) : std::vector<int>{}; if (!L.empty()) uvApplyFaces(L, mode); }
    else { const auto L = topo::edgeLoop(T, e); std::set<int> es(L.begin(), L.end());
      if (key == 'e') { const auto Ls = uvLoopOnSide(e, eK); tselApply(am, Ls, mode, 'e'); uvNoteSides(Ls, mode); }   // (on the side under the pointer)
      else if (key == 'f') { std::vector<int> fs; for (int f = 0; f < T.nf; f++) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) if (es.count(T.ce[size_t(k)])) { fs.push_back(f); break; } tselApply(am, fs, mode, 'f'); }
      else { std::set<int> ps; for (int e2 : es) for (int v : {T.ea[size_t(e2)], T.eb[size_t(e2)]}) for (int f : {T.ef0[size_t(e2)], T.ef1[size_t(e2)]}) if (f >= 0) { int k = uv::cornerAt(T, f, v); if (k >= 0) ps.insert(c->P.of[size_t(k)]); }
        uvApplyList({ps.begin(), ps.end()}, mode); } }
    uvd_.type = 14; drag_ = Drag::Uv; return true; }
  return false;
}

// RMB in the editor (Ctrl+RMB — its pie — picks what it does): the selected shells (none: the one under the pointer) turned freely, turned by 90°,
// scaled, or flipped (a swipe across — U, up and down — V). With the Puppet on, RMB on a pin takes it off
bool Viewport::uvRmbPress(const QPointF& mp) {
  const int am = topoMesh(); if (am < 0 || topoMode == "object") return false;
  if (uvTf_.on && uvTf_.kind == 'p') return uvTfRemovePin(mp);
  const Item* it = scene_->item(am); const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) return false;
  auto mv = uvMoving();
  if (mv.empty()) { const int f = uvFaceAt(mp); if (f < 0) return false; if (tsel_.id != am) tselApply(am, {}, "set"); uvApplyFaces(uvShellOf(f), "set"); mv = uvMoving(); }
  if (mv.empty()) return false;
  const auto b = uv::box(B, mv); const double uc = (b[0] + b[2]) / 2, vc = (b[1] + b[3]) / 2;
  uvd_.type = 4; uvd_.m = am; uvd_.corners = mv; uvd_.uv0 = B.cornerUV; uvd_.cu = uc; uvd_.cv = vc; uvFillMore(); uvd_.axis = 0;
  uvd_.op = uvRmb == "scale" ? 's' : uvRmb == "rot90" ? 'R' : uvRmb == "flip" ? 'F' : 'r';
  if (uvd_.op != 's') if (const UvCache* uc2 = uvCache()) {   // (each shell about its own middle)
    std::map<int, std::array<double, 4>> bx; for (int k : mv) { const int s = uc2->S.of[size_t(uc2->T->cf[size_t(k)])]; auto& q4 = bx.emplace(s, std::array<double, 4>{1e300, 1e300, -1e300, -1e300}).first->second;
      const auto& q = B.cornerUV[size_t(k)]; q4[0] = std::min(q4[0], double(q[0])); q4[1] = std::min(q4[1], double(q[1])); q4[2] = std::max(q4[2], double(q[0])); q4[3] = std::max(q4[3], double(q[1])); }
    uvd_.piv.assign(B.cornerUV.size(), {uc, vc}); for (int k : mv) { const auto& q4 = bx[uc2->S.of[size_t(uc2->T->cf[size_t(k)])]]; uvd_.piv[size_t(k)] = {(q4[0] + q4[2]) / 2, (q4[1] + q4[3]) / 2}; } }
  drag_ = Drag::Uv; return true;
}

// ---------- several meshes in one editor (as the UV Editors of the 3D packages show them): the selected meshes' UV lie in the one tile; a pick on any of them
// makes it the active one, a drag takes the picks of all of them ----
std::vector<int> Viewport::uvMeshes() const {
  std::vector<int> o; const int a = topoMesh();
  for (int id : tselTargets()) { const Item* it = scene_->item(id); if (it && baseOf(*it) && uv::has(*baseOf(*it)) && (id == a || uvShown(id))) o.push_back(id); }
  return o;
}
int Viewport::uvMeshAt(const QPointF& mp) const {
  const auto at = pxToUv(mp);
  for (int id : uvMeshes()) { const Item* it = scene_->item(id); const PolyMesh& B = *baseOf(*it); const std::vector<char> hidF = uvHidden(*it, B);
    for (int f = 0; f < B.faceCount(); f++) { if (!hidF.empty() && size_t(f) < hidF.size() && hidF[size_t(f)]) continue; if (inPoly(B, f, at[0], at[1])) return id; } }
  return -1;
}
void Viewport::uvMakeActive(int m) {
  if (m < 0 || m == topoMesh() || !sel_.hasItem(m)) return;
  sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), m), sel_.items.end()); sel_.items.push_back(m);
  tselActivate(m); uvStamp_++; uvRev_++; uvHov_ = {}; topoRev_++; updateGizmo(); emit topoState(); requestUpdate();
}
std::vector<int> Viewport::uvMovingOf(int m, const TSel& s) const {
  const Item* it = scene_->item(m); if (!it) return {}; auto B = baseSp(*it); auto T = topo::ttopo(B); if (!T || !uv::has(*B)) return {};
  const uv::Points P = uv::points(*B, *T); const char key = tKey(); std::vector<int> ks;
  if (key == 'f') ks = uv::faceCorners(*T, {s.f.begin(), s.f.end()});
  else if (key == 'e') { for (int e : s.e) if (e >= 0 && e < T->ne) for (int f : {T->ef0[size_t(e)], T->ef1[size_t(e)]}) if (f >= 0 && (uv::continuous(*B, *T, P, e) || uvSideOn(s, e, f))) for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) { int k = uv::cornerAt(*T, f, v); if (k >= 0) ks.push_back(k); } }
  else return {};                                              // (points of other meshes: not taken along)
  std::vector<int> out = uv::pointCorners(P, ks);
  if (const int ws = uvWorkSet(m, *B); ws >= 0 && topo::hasUvSets(*B))   // (a set worked in: only its polygons' corners — the rest picked stays where it lies)
    out.erase(std::remove_if(out.begin(), out.end(), [&](int k) { const int f = size_t(k) < T->cf.size() ? T->cf[size_t(k)] : -1; return f < 0 || !(size_t(f) < B->faceSets.size() && (B->faceSets[size_t(f)] >> ws & 1u)); }), out.end());
  return out;
}

void Viewport::uvFillMore() {                                    // the other picked meshes' corners taken into the drag of the active one
  uvd_.more.clear();
  for (const auto& [id, s] : tselX_) { if (!sel_.hasItem(id) || id == uvd_.m) continue; auto cs = uvMovingOf(id, s); if (cs.empty()) continue;
    const Item* it = scene_->item(id); UvMore x; x.m = id; x.corners = std::move(cs); x.uv0 = baseOf(*it)->cornerUV; uvd_.more.push_back(std::move(x)); }
}

// the projection of the polygons not unfolded yet (above the tile): a polygon picked in it — the polygon of the model in front under the point
bool Viewport::uvProjPick(const QPointF& mp, int& mesh, int& face) const {
  if (!uvProj_.on) return false;
  const auto at = pxToUv(mp); const double wx = (at[0] - uvProj_.ox) / uvProj_.sc + uvProj_.x0, wy = (at[1] - 1.12) / uvProj_.sc + uvProj_.y0; double bestZ = -1e300; bool hit = false;
  for (const auto& pm : uvProj_.meshes) { const Item* it = scene_->item(pm.id); if (!it || !it->poly) continue; const PolyMesh& B = *baseOf(*it); const auto& W = it->poly->pos; if (W.size() != B.pos.size()) continue;
    for (int f : pm.fs) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; std::vector<std::pair<double, double>> poly; double z = 0; for (int k = s; k < e; k++) { const Vec3& v = W[size_t(B.faceVerts[size_t(k)])]; poly.push_back({v.x, v.y}); z += v.z; }
      if (insidePolygon(poly, wx, wy) && z / double(e - s) > bestZ) { bestZ = z / double(e - s); mesh = pm.id; face = f; hit = true; } } }
  return hit;
}
// the same for a screen test (a box, a lasso, a brush): the projected polygons of the mesh whose middle it takes
std::vector<int> Viewport::uvProjArea(int mesh, const std::function<bool(double, double)>& inPx) const {
  std::vector<int> o; if (!uvProj_.on) return o;
  for (const auto& pm : uvProj_.meshes) { if (pm.id != mesh) continue; const Item* it = scene_->item(pm.id); if (!it || !it->poly) continue; const PolyMesh& B = *baseOf(*it); const auto& W = it->poly->pos; if (W.size() != B.pos.size()) continue;
    for (int f : pm.fs) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; double cx = 0, cy = 0; for (int k = s; k < e; k++) { const Vec3& v = W[size_t(B.faceVerts[size_t(k)])]; cx += v.x; cy += v.y; } cx /= double(e - s); cy /= double(e - s);
      const QPointF p = uvToPx(uvProj_.ox + (cx - uvProj_.x0) * uvProj_.sc, 1.12 + (cy - uvProj_.y0) * uvProj_.sc); if (inPx(p.x(), p.y())) o.push_back(f); } }
  return o;
}

// ---------- the mouse in the editor ----------
void Viewport::uvPress(QMouseEvent* e) {
  struct PickScope { bool& f; bool was; PickScope(bool& x) : f(x), was(x) { f = true; } ~PickScope() { f = was; } } pickScope_(uvPicking_);
  const QPointF mp = e->position(); const bool alt = e->modifiers() & Qt::AltModifier, ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  uvSyncToolBrush();
  uvd_ = {}; uvd_.p0 = uvd_.last = mp; uvd_.leadF = uvHov_.f;   /* (the shell under the pointer: with Symmetry its mirror follows it) */
  if (alt || e->button() == Qt::MiddleButton) { uvd_.type = e->button() == Qt::RightButton ? 2 : 1; if (cam_.uvPpu <= 0) cam_.uvPpu = cam_.uvScale(paneW(), paneH()); drag_ = Drag::Uv; return; }
  if (uvTiles) { if (e->button() == Qt::LeftButton) uvTilePick(mp); return; }   // (the tiles: to look at — a click works in that tile's set)
  if (e->button() == Qt::LeftButton && uvTf_.on && uvTfPress(mp)) { uvd_.type = 9; drag_ = Drag::Uv; return; }   // (the frame / the warp: its handles)
  if (e->button() == Qt::LeftButton && uvBrush && room() == "topo" && uvMode() && uvBrushPress(mp, ctrl)) { uvd_.type = 10; drag_ = Drag::Uv; return; }   // (the Relax / Pin brush)
  if (e->button() == Qt::RightButton && shift) { emit pieRequested(toGlobal(mp)); return; }
  if (e->button() == Qt::RightButton && ctrl && uvBrush && room() == "topo" && uvMode()) { uvd_.type = 16; uvd_.cu = uvBrushR; uvd_.cv = uvBrushHard; drag_ = Drag::Uv; return; }   // (a brush on: Ctrl+RMB drag — size ↔ hardness)   // (the RMB's action: free turn / 90° / scale / flip)
  if (e->button() == Qt::RightButton && ctrl && room() == "topo" && uvMode()) { emit uvPieRequested(toGlobal(mp)); return; }   // (Ctrl+RMB: the pie of what RMB does to the shells)
  if (e->button() == Qt::RightButton && !ctrl && room() == "topo" && uvMode() && uvRmbPress(mp)) return;
  if (e->button() != Qt::LeftButton || room() != "topo") return;
  if (uvMode() && topoMode != "object") if (const int hit = uvMeshAt(mp); hit >= 0 && hit != topoMesh()) uvMakeActive(hit);   // (a pick on another selected mesh: that one is the active one now — the picks of the others kept)
  if (uvMode() && topoMode != "object") { int pm = -1, pf = -1; if (uvMeshAt(mp) < 0 && uvProjPick(mp, pm, pf)) {   // (a polygon not unfolded, picked in its projection: picked on the model, to see which part it is)
      if (pm != topoMesh()) uvMakeActive(pm); if (topoMode != "poly") setTopoMode("poly", true);
      tselApply(pm, {pf}, ctrl ? "remove" : shift ? "add" : "set", 'f'); tsel_.last = pf; ui::hud(tr("Not unfolded yet: the polygon is picked on the model"), 1500); return; } }
  const int am = topoMesh();
  if (!uvMode()) { ui::hud(tr("The UV Editor — turn the UV mode on (under the modes) to work in it"), 1500); return; }
  // Object mode (or no active mesh): a click on a shown mesh's polygon makes it the active one
  if (am < 0 || topoMode == "object") {
    auto at = pxToUv(mp);
    for (int id : topoMeshes()) { if (!uvShown(id) && id != am) continue; const PolyMesh& B = *baseOf(*scene_->item(id)); if (!uv::has(B)) continue;
      for (int f = 0; f < B.faceCount(); f++) if (inPoly(B, f, at[0], at[1])) { if (shift) { sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), id), sel_.items.end()); sel_.items.push_back(id); } else sel_.items = {id};
        sel_.anchors.clear(); onSelChange(); uvRev_++; return; } }
    return;
  }
  const PolyMesh& B = *baseOf(*scene_->item(am));
  if (!uv::has(B)) { ui::toast(tr("This mesh has no UV yet — mark seams (Edge mode) and Unfold")); return; }
  const char key = tKey();
  if (tsel_.id != am) tselApply(am, {}, "set");
  if (const int gid = uvGroupAt(mp); gid > 0) {                    // (a click on a group's plate: every shell of the group — Shift adds, Ctrl takes off)
    const auto attrs = uvShellAttrs(am); std::vector<int> fs; for (size_t f = 0; f < attrs.size(); f++) if (attrs[f].group == gid) fs.push_back(int(f));
    if (topoMode != "poly") setTopoMode("poly", true);
    tselApply(am, fs, ctrl ? "remove" : shift ? "add" : "set", 'f'); ui::hud(tr("Group %1: %2 polygon%3").arg(gid).arg(fs.size()).arg(fs.size() == 1 ? "" : "s"), 1200); uvRev_++; requestUpdate(); return; }
  if (uvGizmoOn() && key == 'f' && !ctrl) if (const int gf = uvFaceAt(mp); gf >= 0) {   // (a double click with the gizmo on: the whole shell)
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); const bool dbl = uvGzClick_.first == gf && now - uvGzClick_.second < 400; uvGzClick_ = dbl ? std::pair<int, qint64>{-1, 0} : std::pair<int, qint64>{gf, now};
    if (dbl) { const auto fs = uvShellOf(gf); uvApplyFaces(fs, shift ? "add" : "set"); ui::hud(tr("Shell: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"), 1000); requestUpdate(); return; } }
  // the gizmo (Move / Rotate / Scale): a handle pressed — that operation, along its axis, about the pivot
  if (uvGizmoOn()) if (const int h = uvGizmoHit(mp)) {
    bool take = true;
    if (h == 5) {                                                // (inside the frame on what isn't selected: that is picked, not moved)
      const UvPick pk = uvPickAt(mp); const char k = tKey(); const int el = k == 'f' ? pk.f : k == 'e' ? pk.e : pk.p;
      if (el >= 0 && !(k == 'f' ? tsel_.f.count(el) > 0 : k == 'e' ? tsel_.e.count(el) > 0 && (pk.ek < 0 || uvCache()->cont[size_t(el)] || uvSideOn(tsel_, el, uvCache()->T->cf[size_t(pk.ek)])) : uvPoints().count(el) > 0)) take = false;
    }
    const auto mv = take ? uvMoving() : std::vector<int>{};
    if (!mv.empty() || (take && uvSelBox())) {
      const auto b = uvSelBox().value_or(uv::box(B, mv)); const double uc = (b[0] + b[2]) / 2, vc = (b[1] + b[3]) / 2;   // (the frame of every picked mesh's pick: what the handles scale about)
      uvd_.type = 4; uvd_.m = am; uvd_.corners = mv; uvd_.uv0 = B.cornerUV; uvd_.cu = uc; uvd_.cv = vc; uvFillMore(); uvd_.axis = 0;
      if (uvd_.corners.empty() && !uvd_.more.empty()) { uvd_.m = uvd_.more[0].m; uvd_.corners = uvd_.more[0].corners; uvd_.uv0 = uvd_.more[0].uv0; uvd_.more.erase(uvd_.more.begin()); }   // (nothing picked on the active mesh: the first other picked one leads the drag)
      if (h == 4) uvd_.op = 'r';
      else if (h >= 10 && h <= 13) { uvd_.op = 's'; uvd_.axis = 3;   // (a corner: both ways — the opposite corner held)
        uvd_.cu = h == 10 || h == 13 ? b[2] : b[0]; uvd_.cv = h == 10 || h == 11 ? b[3] : b[1]; }
      else if (h >= 20 && h <= 23) { uvd_.op = 's'; uvd_.axis = h <= 21 ? 1 : 2;   // (a side: one way — the opposite side held)
        if (h == 20) uvd_.cu = b[2]; else if (h == 21) uvd_.cu = b[0]; else if (h == 22) uvd_.cv = b[3]; else uvd_.cv = b[1]; }
      else { uvd_.op = 'm'; uvd_.axis = h == 1 ? 1 : h == 2 ? 2 : 0; }
      if (uvo.pivShells && uvd_.op == 'r') if (const UvCache* uc2 = uvCache()) {   // (each shell about its own middle)
        std::map<int, std::array<double, 4>> bx; for (int k : mv) { const int s = uc2->S.of[size_t(uc2->T->cf[size_t(k)])]; auto& q4 = bx.emplace(s, std::array<double, 4>{1e300, 1e300, -1e300, -1e300}).first->second;
          const auto& q = B.cornerUV[size_t(k)]; q4[0] = std::min(q4[0], double(q[0])); q4[1] = std::min(q4[1], double(q[1])); q4[2] = std::max(q4[2], double(q[0])); q4[3] = std::max(q4[3], double(q[1])); }
        uvd_.piv.assign(B.cornerUV.size(), {uc, vc}); for (int k : mv) { const auto& q4 = bx[uc2->S.of[size_t(uc2->T->cf[size_t(k)])]]; uvd_.piv[size_t(k)] = {(q4[0] + q4[2]) / 2, (q4[1] + q4[3]) / 2}; } }
      drag_ = Drag::Uv; return;
    }
  }
  if (tool_ == "cursor" && tset.selTool != "rect" && uvToolPress(mp, ctrl, shift)) return;   // (the Select tools: Lasso, Select Cursor, Fill, Shells, Loop, Ring)
  const UvPick pk = uvPickAt(mp); const int el = key == 'f' ? pk.f : key == 'e' ? pk.e : pk.p;
  auto box = [&](const QString& mode) { uvd_.type = 3; uvd_.mode = mode; drag_ = Drag::Uv; };
  if (el < 0) { box(ctrl ? "remove" : shift ? "add" : "set"); return; }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool dbl = uvClick_.el == el && uvClick_.key == key && now - uvClick_.t < 400;
  uvClick_ = dbl ? UvClick{} : UvClick{el, key, now};
  const UvCache* c = uvCache(); const topo::TTopo& T = *c->T;
  auto pickPts = [&](const std::set<int>& pts) {             // (the UV points picked: their vertices picked in 3D)
    std::set<int> vs; for (int p : pts) vs.insert(c->P.vert[size_t(p)]);
    tselApply(am, {vs.begin(), vs.end()}, "set", 'v'); uvPts_ = pts; uvPtsVer_ = tsel_.ver;
  };
  // (the UV's own edges: a corner k's edge runs from point P.of[k] to the next corner's — a seam edge is two of them, one on each side; walked here,
  //  the clicked side stays the clicked side — a sleeve's seam has both its sides on one shell, 2026-11-10)
  auto nextK = [&](int k) { const int f = T.cf[size_t(k)]; return k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; };
  if (dbl && key == 'e' && pk.ek >= 0) {                       // the edge loop, on the clicked side
    const auto outE = uvLoopOnSide(el, pk.ek); tselApply(am, outE, "add"); uvNoteSides(outE, "add"); ui::hud(tr("Edge loop: %1").arg(outE.size()), 1000); return; }
  if (ctrl && shift && key == 'e' && pk.ek >= 0 && tsel_.last >= 0 && tsel_.last != el && !tsel_.e.empty()) {   // the shortest path from the last picked edge — along the UV's edges of this shell, on the side clicked
    const int last = tsel_.last, sh = c->S.of[size_t(T.cf[size_t(pk.ek)])]; const auto ls = tsel_.eSide.find(last);
    std::vector<int> from; for (int k = 0; k < int(T.cf.size()); k++) if (T.ce[size_t(k)] == last && c->S.of[size_t(T.cf[size_t(k)])] == sh && (ls == tsel_.eSide.end() || ls->second.empty() || ls->second.count(T.cf[size_t(k)]))) from.push_back(k);
    if (!from.empty()) {
      std::map<int, std::vector<std::pair<int, int>>> adj;   // (point → (point, corner))
      for (int k = 0; k < int(T.cf.size()); k++) if (c->S.of[size_t(T.cf[size_t(k)])] == sh) { const int a = c->P.of[size_t(k)], b = c->P.of[size_t(nextK(k))]; adj[a].push_back({b, k}); adj[b].push_back({a, k}); }
      auto at = [&](int p) { const auto& u = B.cornerUV[size_t(c->P.list[size_t(c->P.off[size_t(p)])])]; return QPointF(u[0], u[1]); };
      std::map<int, double> d; std::map<int, std::pair<int, int>> pr; using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
      for (int k : from) for (int p : {c->P.of[size_t(k)], c->P.of[size_t(nextK(k))]}) { d[p] = 0; q.push({0, p}); }
      const int t0 = c->P.of[size_t(pk.ek)], t1 = c->P.of[size_t(nextK(pk.ek))]; int hit = -1;
      while (!q.empty()) { auto [dd, u] = q.top(); q.pop(); if (dd > d[u]) continue; if (u == t0 || u == t1) { hit = u; break; }
        for (auto [v, k] : adj[u]) { const QPointF dv = at(v) - at(u); const double nd = dd + std::hypot(dv.x(), dv.y()); auto it = d.find(v); if (it == d.end() || nd < it->second) { d[v] = nd; pr[v] = {u, k}; q.push({nd, v}); } } }
      if (hit >= 0) { std::vector<int> outE{el}; uvEdgeSideTmp_.clear(); uvEdgeSideTmp_[el].insert(T.cf[size_t(pk.ek)]);
        for (int v = hit; pr.count(v); v = pr[v].first) { const int k = pr[v].second; outE.push_back(T.ce[size_t(k)]); uvEdgeSideTmp_[T.ce[size_t(k)]].insert(T.cf[size_t(k)]); }
        tselApply(am, outE, "add"); uvNoteSides(outE, "add"); tsel_.last = el; return; } } }
  if (dbl) {                                                   // the shell (polygons, points) / the edge loop
    if (key == 'e') { const int sh0 = pk.ek >= 0 ? c->S.of[size_t(T.cf[size_t(pk.ek)])] : -1; auto L = topo::edgeLoop(T, el); if (sh0 >= 0) L.erase(std::remove_if(L.begin(), L.end(), [&](int x) { const int a0 = T.ef0[size_t(x)], b0 = T.ef1[size_t(x)]; return !((a0 >= 0 && c->S.of[size_t(a0)] == sh0) || (b0 >= 0 && c->S.of[size_t(b0)] == sh0)); }), L.end()); tselApply(am, L, "add"); uvSidesOnShell(T, c->S, L, sh0, "add"); ui::hud(tr("Edge loop: %1").arg(L.size()), 1000); return; }
    int f0 = key == 'f' ? el : T.cf[size_t(c->P.list[size_t(c->P.off[size_t(el)])])]; int s = c->S.of[size_t(f0)];
    std::vector<int> fs; for (int f = 0; f < T.nf; f++) if (c->S.of[size_t(f)] == s) fs.push_back(f);
    if (key == 'f') tselApply(am, fs, "add");
    else { std::set<int> pts = uvPoints(); for (int k : uv::faceCorners(T, fs)) pts.insert(c->P.of[size_t(k)]); pickPts(pts); }
    ui::hud(tr("Shell: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"), 1000); return;
  }
  if (ctrl && shift) {                                         // the shortest path from the last picked (as in 3D)
    if (key == 'v') { auto pts = uvPoints(); const int last = uvLastPt_; pts.insert(el);
      if (last >= 0 && last != el && last < c->P.count()) if (auto p = topo::path(T, tWorld(am), 'v', c->P.vert[size_t(last)], c->P.vert[size_t(el)])) {
        const int sh = c->S.of[size_t(T.cf[size_t(c->P.list[size_t(c->P.off[size_t(el)])])])]; std::set<int> on(p->begin(), p->end());
        for (int q = 0; q < c->P.count(); q++) if (on.count(c->P.vert[size_t(q)]) && c->S.of[size_t(T.cf[size_t(c->P.list[size_t(c->P.off[size_t(q)])])])] == sh) pts.insert(q); }
      pickPts(pts); uvLastPt_ = el; return; }
    const int last = tsel_.last; const bool any = key == 'f' ? !tsel_.f.empty() : !tsel_.e.empty();
    const int shP = key == 'e' && pk.ek >= 0 ? c->S.of[size_t(T.cf[size_t(pk.ek)])] : -1;
    if (last >= 0 && last != el && any) { auto p = topo::path(T, tWorld(am), key, last, el); if (p) { tselApply(am, *p, "add"); if (key == 'e') uvSidesOnShell(T, c->S, *p, shP, "add"); } else { ui::toast(tr("No path between them (different parts of the mesh)")); tselApply(am, {el}, "add"); if (key == 'e') uvSidesOnShell(T, c->S, {el}, shP, "add"); } }
    else { tselApply(am, {el}, "add"); if (key == 'e') uvSidesOnShell(T, c->S, {el}, shP, "add"); }
    tsel_.last = el; return;
  }
  if (key == 'v') uvLastPt_ = el; else tsel_.last = el;       // (the last picked: where Ctrl+Shift draws the path from)
  if (ctrl) { if (key == 'v') { auto pts = uvPoints(); pts.erase(el); pickPts(pts); } else { tselApply(am, {el}, "remove"); if (key == 'e' && pk.ek >= 0) { uvEdgeSideTmp_.clear(); uvEdgeSideTmp_[el].insert(T.cf[size_t(pk.ek)]); uvNoteSides({el}, "remove"); } } return; }
  bool had = key == 'f' ? tsel_.f.count(el) > 0 : key == 'e' ? tsel_.e.count(el) > 0 && (pk.ek < 0 || c->cont[size_t(el)] || uvSideOn(tsel_, el, T.cf[size_t(pk.ek)])) : uvPoints().count(el) > 0;
  if (!had || shift) { if (key == 'v') { std::set<int> pts = shift ? uvPoints() : std::set<int>{}; pts.insert(el); pickPts(pts); } else { tselApply(am, {el}, shift ? "add" : "set"); if (key == 'e' && pk.ek >= 0) { uvEdgeSideTmp_.clear(); uvEdgeSideTmp_[el].insert(T.cf[size_t(pk.ek)]); uvNoteSides({el}, shift ? "add" : "set"); } } }
  // a drag on the selection moves it
  uvd_.type = 4; uvd_.op = 'm'; uvd_.m = am; uvd_.corners = uvMoving(); uvd_.uv0 = B.cornerUV; uvFillMore(); drag_ = Drag::Uv;
}

void Viewport::uvMove(QMouseEvent* e) {
  struct PickScope { bool& f; bool was; PickScope(bool& x) : f(x), was(x) { f = true; } ~PickScope() { f = was; } } pickScope_(uvPicking_);
  const QPointF mp = e->position(); uvSyncToolBrush();
  if (drag_ != Drag::Uv) {                                     // (the element under the pointer lit)
    if (uvBrush) { uvBrushAt_ = mp; overlayRev_++; requestUpdate(); }   // (the brush's circle follows)
    if (!uvMode() || topoMode == "object" || ui::Pie::current()) { if (uvHov_.f >= 0 || uvHov_.e >= 0 || uvHov_.p >= 0) { uvHov_ = {}; uvHovRev_++; requestUpdate(); } return; }
    UvPick p = uvPickAt(mp);
    if (uvo.stretch && uvHov_.f < 0 && p.f < 0 && uvStretchHud_) { ui::hud({}); uvStretchHud_ = false; }   // (over nothing: the stretch's line gone too — it stayed when the pointer left the shell)
    if (p.f != uvHov_.f || p.ek != uvHov_.ek || p.p != uvHov_.p) { uvHov_ = {p.f, p.e, p.ek, p.p}; uvHovRev_++; requestUpdate();
      if (uvo.stretch) {                                         // (the stretch on: the polygon under the pointer — by how much)
        const int am = topoMesh(); const Item* it = am >= 0 ? scene_->item(am) : nullptr; const PolyMesh* B = it ? baseOf(*it) : nullptr;
        if (B && uv::has(*B) && p.f >= 0 && p.f < B->faceCount()) { double au = 0, a3 = 0; for (int f = 0; f < B->faceCount(); f++) { au += std::abs(uv::faceArea(*B, f)); a3 += uv::faceArea3(*B, f); }
          const double y = uv::faceArea3(*B, p.f), q = y > 0 && au > 0 ? std::abs(uv::faceArea(*B, p.f)) / y * (a3 / au) : 0;
          uvStretchHud_ = true;
          ui::hud(q <= 0 ? tr("Stretch: the polygon has no UV area") : tr("Stretch: UV area ×%1 of the mesh's — the sides ×%2 · %3").arg(q, 0, 'f', 2).arg(std::sqrt(q), 0, 'f', 2)
                    .arg(std::abs(std::log2(q)) < 0.15 ? tr("as it should be") : q < 1 ? tr("squeezed (red)") : tr("stretched (blue)"))); }
        else ui::hud({}); } }
    return;
  }
  const double W = paneW(), H = paneH(), s = cam_.uvScale(W, H);
  QPointF d = mp - uvd_.last; uvd_.last = mp;
  if (uvd_.type == 1) { cam_.uvCx -= d.x() / s; cam_.uvCy += d.y() / (s * cam_.uvAsp); requestUpdate(); return; }
  if (uvd_.type == 2) {
    auto at = pxToUv(uvd_.p0); double ns = std::clamp(s * std::exp((d.x() - d.y()) * 0.01), 4.0, 400000.0); cam_.uvPpu = ns;
    cam_.uvCx = at[0] - (uvd_.p0.x() - W / 2) / ns; cam_.uvCy = at[1] + (uvd_.p0.y() - H / 2) / (ns * cam_.uvAsp); requestUpdate(); return; }
  if (uvd_.type == 16) { uvBrushR = std::clamp(uvd_.cu + (mp.x() - uvd_.p0.x()) * 0.5, 6.0, 600.0); uvBrushHard = std::clamp(uvd_.cv - (mp.y() - uvd_.p0.y()) / 200.0, 0.0, 1.0); if (uvBrushFromTool_) { brushR = uvBrushR; brushHard = uvBrushHard; panelsLater(); } uvBrushAt_ = uvd_.p0; overlayRev_++; requestUpdate(); ui::hud(tr("Brush: size %1 px · hardness %2%").arg(int(uvBrushR)).arg(int(std::lround(uvBrushHard * 100)))); return; }
  if (uvd_.type == 10) { uvBrushStroke(mp, e->modifiers()); requestUpdate(); return; }
  if (uvd_.type == 9) { uvTfMove(mp, e->modifiers()); return; }
  if (uvd_.type == 11) { auto [lx, ly] = uvd_.pts.back(); if (std::hypot(mp.x() - lx, mp.y() - ly) >= 3) uvd_.pts.push_back({mp.x(), mp.y()}); lasso_ = uvd_.pts; uvd_.moved = true; overlayRev_++; requestUpdate(); return; }
  if (uvd_.type == 12) { const double R = 14; const int n = std::max(1, int(std::ceil(std::hypot(mp.x() - uvd_.p0.x(), mp.y() - uvd_.p0.y()) / (R / 2)))); const QPointF a = uvd_.p0; uvd_.p0 = mp;
    for (int i = 1; i <= n; i++) uvPaintAt(a + (mp - a) * (double(i) / n)); uvd_.moved = true;
    lasso_.clear(); for (int i = 0; i < 24; i++) { double an = i * M_PI / 12; lasso_.push_back({mp.x() + std::cos(an) * R, mp.y() + std::sin(an) * R}); } overlayRev_++; requestUpdate(); return; }
  if (uvd_.type == 13) { lasso_ = {{uvd_.p0.x(), uvd_.p0.y()}, {mp.x(), uvd_.p0.y()}, {mp.x(), mp.y()}, {uvd_.p0.x(), mp.y()}}; uvd_.moved = true; overlayRev_++; requestUpdate(); return; }
  if (uvd_.type == 14) return;
  if (uvd_.type == 3) { lasso_ = {{uvd_.p0.x(), uvd_.p0.y()}, {mp.x(), uvd_.p0.y()}, {mp.x(), mp.y()}, {uvd_.p0.x(), mp.y()}}; uvd_.moved = true; overlayRev_++; requestUpdate(); return; }
  if (uvd_.type != 4 || uvd_.corners.empty()) return;
  if (!uvd_.moved && std::hypot(mp.x() - uvd_.p0.x(), mp.y() - uvd_.p0.y()) < 3) return;
  uvd_.moved = true; if (uvHov_.f >= 0 || uvHov_.ek >= 0 || uvHov_.p >= 0) { uvHov_ = {}; uvHovRev_++; }
  uvTransformDrag(uvd_.m, uvd_.corners, uvd_.uv0, uvd_.piv, mp, e->modifiers(), true);
  for (const auto& x : uvd_.more) uvTransformDrag(x.m, x.corners, x.uv0, {}, mp, e->modifiers(), false);   // (the other picked meshes: the same drag)
}

// the moving / turning / scaling / flipping of the picked UV (one mesh of them): the drag of the press, the pointer now at mp. Several meshes picked: each is run so
void Viewport::uvTransformDrag(int mesh, const std::vector<int>& corners, const std::vector<std::array<float, 2>>& uv0, const std::vector<std::array<double, 2>>& piv, const QPointF& mp, Qt::KeyboardModifiers mods, bool main) {
  const double W = paneW(), H = paneH(), s = cam_.uvScale(W, H); const bool shift = mods & Qt::ShiftModifier;
  std::vector<std::array<float, 2>> U = uv0;
  // (the pivot of each corner: the selection's middle — or, the gizmo's per shell setting, its shell's own)
  auto pv = [&](int k) { return piv.size() == uv0.size() ? piv[size_t(k)] : std::array<double, 2>{uvd_.cu, uvd_.cv}; };
  if (uvd_.op == 'm') {
    double du = (mp.x() - uvd_.p0.x()) / s, dv = -(mp.y() - uvd_.p0.y()) / (s * cam_.uvAsp);
    if (uvd_.axis == 1) dv = 0; else if (uvd_.axis == 2) du = 0;   // (the gizmo's U / V arrow)
    else if (shift) { if (std::abs(du) > std::abs(dv)) dv = 0; else du = 0; }   // (Shift: along one axis)
    for (int k : corners) U[size_t(k)] = {float(uv0[size_t(k)][0] + du), float(uv0[size_t(k)][1] + dv)};
    const UvTex tx = uvTex(mesh);
    ui::hud(tr("Move U %1 · V %2  (%3 · %4 px)").arg(du, 0, 'f', 4).arg(dv, 0, 'f', 4).arg(du * tx.w, 0, 'f', 1).arg(dv * tx.h, 0, 'f', 1));
  } else {
    QPointF c = uvToPx(uvd_.cu, uvd_.cv);
    if (uvd_.op == 'F') {                                         // (a swipe across: flip U; up or down: flip V — each shell about its own middle)
      const bool across = std::abs(mp.x() - uvd_.p0.x()) >= std::abs(mp.y() - uvd_.p0.y());
      for (int k : corners) { const auto p = pv(k); const auto& q = uv0[size_t(k)]; U[size_t(k)] = across ? std::array<float, 2>{float(2 * p[0] - q[0]), q[1]} : std::array<float, 2>{q[0], float(2 * p[1] - q[1])}; }
      ui::hud(across ? tr("Flip U (a swipe across)") : tr("Flip V (a swipe up / down)"));
    } else if (uvd_.op == 'R') {                                  // (turned by 90° steps, the way the pointer goes round)
      QPointF c0 = uvToPx(uvd_.cu, uvd_.cv);
      double a = std::atan2(-(mp.y() - c0.y()), mp.x() - c0.x()) - std::atan2(-(uvd_.p0.y() - c0.y()), uvd_.p0.x() - c0.x());
      a = std::round(a / (M_PI / 2)) * (M_PI / 2); const double cs = std::cos(a), sn = std::sin(a), ka = cam_.uvAsp;
      for (int k : corners) { const auto p = pv(k); double x = uv0[size_t(k)][0] - p[0], y = (uv0[size_t(k)][1] - p[1]) * ka; U[size_t(k)] = {float(p[0] + x * cs - y * sn), float(p[1] + (x * sn + y * cs) / ka)}; }
      ui::hud(tr("Rotate %1°").arg(a * 180 / M_PI, 0, 'f', 0));
    } else if (uvd_.op == 'r') {
      double a = std::atan2(-(mp.y() - c.y()), mp.x() - c.x()) - std::atan2(-(uvd_.p0.y() - c.y()), uvd_.p0.x() - c.x());
      if (shift) a = std::round(a / (M_PI / 12)) * (M_PI / 12);
      const double cs = std::cos(a), sn = std::sin(a);
      const double ka = cam_.uvAsp;   // (turned as texels are: V in the texture's proportion)
      for (int k : corners) { const auto p = pv(k); double x = uv0[size_t(k)][0] - p[0], y = (uv0[size_t(k)][1] - p[1]) * ka; U[size_t(k)] = {float(p[0] + x * cs - y * sn), float(p[1] + (x * sn + y * cs) / ka)}; }
      ui::hud(tr("Rotate %1°").arg(a * 180 / M_PI, 0, 'f', 1));
    } else {
      double r0 = std::max(2.0, std::hypot(uvd_.p0.x() - c.x(), uvd_.p0.y() - c.y())), r = std::hypot(mp.x() - c.x(), mp.y() - c.y()), k0 = r / r0;
      double ku = k0, kv = k0;
      if (uvd_.axis == 3) {                                       // (the frame's corner: both ways, each its own — Shift: evenly)
        ku = (mp.x() - c.x()) / (std::abs(uvd_.p0.x() - c.x()) < 2 ? 2.0 : uvd_.p0.x() - c.x()); kv = (mp.y() - c.y()) / (std::abs(uvd_.p0.y() - c.y()) < 2 ? 2.0 : uvd_.p0.y() - c.y());
        if (shift) { const double k = std::abs(ku) > std::abs(kv) ? ku : kv; ku = kv = k; } }
      else if (uvd_.axis == 1) { ku = (mp.x() - c.x()) / (std::abs(uvd_.p0.x() - c.x()) < 2 ? 2.0 : uvd_.p0.x() - c.x()); kv = shift ? ku : 1; }   // (the frame's side: one way)
      else if (uvd_.axis == 2) { kv = (mp.y() - c.y()) / (std::abs(uvd_.p0.y() - c.y()) < 2 ? -2.0 : uvd_.p0.y() - c.y()); ku = shift ? kv : 1; }   // (a side with Shift: evenly both ways, from the opposite side)
      else if (shift) { double dx = std::abs(mp.x() - uvd_.p0.x()), dy = std::abs(mp.y() - uvd_.p0.y());   // (Shift: along the axis the pointer goes)
        if (dx > dy) { ku = std::abs(mp.x() - c.x()) / std::max(2.0, std::abs(uvd_.p0.x() - c.x())); kv = 1; } else { kv = std::abs(mp.y() - c.y()) / std::max(2.0, std::abs(uvd_.p0.y() - c.y())); ku = 1; } }
      for (int k : corners) { const auto p = pv(k); U[size_t(k)] = {float(p[0] + (uv0[size_t(k)][0] - p[0]) * ku), float(p[1] + (uv0[size_t(k)][1] - p[1]) * kv)}; }
      ui::hud(tr("Scale U %1 · V %2").arg(ku, 0, 'f', 3).arg(kv, 0, 'f', 3));
    }
  }
  if (main) uvLive(mesh, U); else { overlayRev_++; liveApply(mesh, nullptr, &U); }   // (the others: no Symmetry — their own mirror shells are not the active mesh's)
}

void Viewport::uvRelease(QMouseEvent* e) {
  struct PickScope { bool& f; bool was; PickScope(bool& x) : f(x), was(x) { f = true; } ~PickScope() { f = was; } } pickScope_(uvPicking_);
  const QPointF mp = e->position(); UvDragS d = uvd_; uvd_ = {};
  if (drag_ == Drag::Uv) drag_ = Drag::None;
  if (d.type == 10) { uvBrushRelease(d); requestUpdate(); return; }
  if (d.type == 9) { uvTfRelease(); return; }
  if (d.type == 16) { ui::hud({}); emit topoState(); requestUpdate(); return; }
  if (d.type == 14 || d.type == 12) { lasso_.clear(); overlayRev_++; ui::hud({}); requestUpdate(); return; }
  if (d.type == 11) { lasso_.clear(); overlayRev_++;
    auto pts = d.pts; pts.push_back({mp.x(), mp.y()});
    if (pts.size() >= 3 && d.moved && tKey() != 'v') { uvAreaApply([pts](double x, double y) { return insidePolygon(pts, x, y); }, d.mode); }
    else if (pts.size() >= 3 && d.moved) { auto list = uvArea([pts](double x, double y) { return insidePolygon(pts, x, y); }, tKey()); uvApplyList(list, d.mode); ui::hud(tr("%1 %2").arg(list.size()).arg(d.mode == "remove" ? tr("deselected") : tr("selected")), 900); }
    else if (d.mode == "set" && tselAny()) tselClear();
    requestUpdate(); return; }
  if (d.type == 13) { lasso_.clear(); overlayRev_++;
    if (!d.moved) { if (d.mode == "set" && tselAny()) tselClear(); requestUpdate(); return; }
    const double x0 = std::min(d.p0.x(), mp.x()), x1 = std::max(d.p0.x(), mp.x()), y0 = std::min(d.p0.y(), mp.y()), y1 = std::max(d.p0.y(), mp.y());
    auto hit = uvArea([=](double x, double y) { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }, 'f');   // (every polygon in the box: its whole shell)
    std::set<int> sh; if (const UvCache* c = uvCache()) for (int f : hit) sh.insert(c->S.of[size_t(f)]);
    std::vector<int> fs; if (const UvCache* c = uvCache()) for (int f = 0; f < c->T->nf; f++) if (sh.count(c->S.of[size_t(f)])) fs.push_back(f);
    if (d.mode == "set") tselClear();
    uvApplyFaces(fs, d.mode == "set" ? "add" : d.mode); ui::hud(tr("%1 shell%2").arg(sh.size()).arg(sh.size() == 1 ? "" : "s"), 900);
    requestUpdate(); return; }
  if (d.type == 3) {
    lasso_.clear(); overlayRev_++;
    int am = topoMesh(); if (am < 0) { requestUpdate(); return; }
    const char key = tKey();
    if (!d.moved || std::hypot(mp.x() - d.p0.x(), mp.y() - d.p0.y()) < 4) {
      if (d.mode == "set" && tsel_.f.size() + tsel_.e.size() + tsel_.v.size()) tselClear();
      requestUpdate(); return;
    }
    if (key != 'v') { const double bx0 = std::min(d.p0.x(), mp.x()), bx1 = std::max(d.p0.x(), mp.x()), by0 = std::min(d.p0.y(), mp.y()), by1 = std::max(d.p0.y(), mp.y()); uvAreaApply([=](double x, double y) { return x >= bx0 && x <= bx1 && y >= by0 && y <= by1; }, d.mode); requestUpdate(); return; }   // (every selected mesh takes part)
    auto list = uvBox(std::min(d.p0.x(), mp.x()), std::min(d.p0.y(), mp.y()), std::max(d.p0.x(), mp.x()), std::max(d.p0.y(), mp.y()), key);
    if (key == 'v') {
      std::set<int> pts = d.mode == "set" ? std::set<int>{} : uvPoints();
      for (int p : list) { if (d.mode == "remove") pts.erase(p); else pts.insert(p); }
      const UvCache* c = uvCache(); std::set<int> vs; for (int p : pts) vs.insert(c->P.vert[size_t(p)]);
      tselApply(am, {vs.begin(), vs.end()}, "set", 'v'); uvPts_ = pts; uvPtsVer_ = tsel_.ver;
    } else { tselApply(am, list, d.mode); if (key == 'e') uvNoteSides(list, d.mode); }   // (the side each edge was found on, whatever the way it was picked — a box, a lasso)
    ui::hud(tr("%1 %2").arg(list.size()).arg(d.mode == "remove" ? tr("deselected") : tr("selected")), 900);
    requestUpdate(); return;
  }
  if (d.type == 4 && !d.moved && d.op == 'R') {                  // (RMB, no drag: a click — turned 90° (Shift — the other way))
    const double a = (e->modifiers() & Qt::ShiftModifier) ? -M_PI / 2 : M_PI / 2; const double ka = cam_.uvAsp; std::vector<std::array<float, 2>> U = d.uv0;
    for (int k : d.corners) { const std::array<double, 2> p = d.piv.size() == d.uv0.size() ? d.piv[size_t(k)] : std::array<double, 2>{d.cu, d.cv}; double x = d.uv0[size_t(k)][0] - p[0], y = (d.uv0[size_t(k)][1] - p[1]) * ka;
      U[size_t(k)] = {float(p[0] + x * std::cos(a) - y * std::sin(a)), float(p[1] + (x * std::sin(a) + y * std::cos(a)) / ka)}; }
    uvLive(d.m, U); d.moved = true;
    for (const auto& x : d.more) {                                 // (the other picked meshes: the same quarter turn about the selection's middle, as their drag)
      std::vector<std::array<float, 2>> V = x.uv0;
      for (int k : x.corners) { if (size_t(k) >= V.size()) continue; double u = x.uv0[size_t(k)][0] - d.cu, v = (x.uv0[size_t(k)][1] - d.cv) * ka;
        V[size_t(k)] = {float(d.cu + u * std::cos(a) - v * std::sin(a)), float(d.cv + (u * std::sin(a) + v * std::cos(a)) / ka)}; }
      overlayRev_++; liveApply(x.m, nullptr, &V); } }
  if (d.type == 4 && d.moved) {
    ui::hud({});
    if (Item* it = scene_->item(d.m)) { if (d.op == 'r' || d.op == 'R') if (auto Tn = topo::ttopo(baseSp(*it))) uvPinsNormalize(d.m, *baseSp(*it), *Tn); /* (the constraints follow a turn only — not a move or a scale) */ topoWrite(d.m, baseSp(*it)); for (const auto& x : d.more) if (Item* xi = scene_->item(x.m)) topoWrite(x.m, baseSp(*xi)); uvStamp_++; uvRev_++; commit(false); emit topoState(); }
    if (d.op == 'm' && tKey() == 'v') uvPtsVer_ = tsel_.ver;     // (the points stay picked as they were)
  }
  requestUpdate();
}

// the UV shown while dragging (a copy of the base — the undo step keeps the one before)
void Viewport::uvLive(int m, const std::vector<std::array<float, 2>>& U) {
  overlayRev_++;                                               // (what is drawn over the editor — the Puppet's pins — follows)
  if (!scene_->item(m)) return;                               // (checks: a puppet with no mesh)
  // (a brush stroke moves what is under it, not the pick: the mirroring, which follows the pick, is left out — it would
  // set the picked shells' partners on their mirror places while the brush never touched them)
  if (tset.sym && uvd_.type != 10) { auto S = U; uvSymApply(S, uvd_.type == 4 && uvd_.m == m ? &uvd_.uv0 : nullptr); liveApply(m, nullptr, &S); return; }   // (the UV symmetry: the mirrored shells follow)
  liveApply(m, nullptr, &U);                                   // (the points and shells stay the same: the cache goes along)
}

// Preserve UVs: the moved vertices' UV from where they now lie on the surface as it was
std::vector<std::array<float, 2>> Viewport::uvKeep(int m, const std::vector<Vec3>& P0, const std::vector<std::array<float, 2>>& uv0, const std::vector<Vec3>& P) const {
  const Item* it = scene_->item(m); if (!it) return uv0;
  auto B = baseSp(*it); auto T = topo::ttopo(B); if (!T || uv0.size() != B->faceVerts.size() || uv0.empty()) return uv0;
  PolyMesh O; O.pos = P0; O.faceStart = B->faceStart; O.faceVerts = B->faceVerts; O.cornerUV = uv0;
  std::vector<int> moved; for (size_t v = 0; v < P.size() && v < P0.size(); v++) if ((P[v] - P0[v]).lengthSq() > 0) moved.push_back(int(v));
  return uv::preserve(O, *T, P, moved);
}

// ---------- what the editor draws ----------
const Viewport::UvDraw& Viewport::uvDraw() {
  uvTfResync();                                                // (an undo under the Frame / Warp / Puppet: made again first — before the UV cache is read for drawing)
  const int am = topoMesh();
  const UvTex tex = am >= 0 ? uvTex(am) : UvTex{}; cam_.uvAsp = tex.asp(); for (auto& p : panes_) p.uvAsp = tex.asp();   // (the texture's proportion: the editor's V)
  QByteArray sig = QByteArray::number(uvRev_) + "|" + QByteArray::number(topoRev_) + "|" + QByteArray::number(staticRev_) + "|" + QByteArray::number(tsel_.ver) + "|" + QByteArray::number(uvPtsVer_)
                 + "|" + topoMode.toLatin1() + "|" + QByteArray::number(am) + "|" + QByteArray::number(int(tset.uv)) + room().toLatin1();
  sig += "|tf" + QByteArray::number(uvTfRev_);
  sig += "|" + QByteArray::number(uvPicked.id) + "," + QByteArray::number(uvPicked.set) + "," + QByteArray::number(uvPicked.mat) + "|" + QByteArray::number(looks::revision());
  sig += uvSetIso ? "|iso" : "|-";   // (the picked set / mat: what is shown, whose texture)
  sig += tset.sym ? (uvo.symTint ? "|sym+" : "|sym") : "|nosym";   // (the symmetry: its axis, the −X tint)
  sig += QByteArray(uvo.diag ? "|diag" : "") + (uvo.stretchAngle ? "|ang" : "");
  sig += "|tex" + QByteArray::number(tex.w) + "x" + QByteArray::number(tex.h) + "p" + QByteArray::number(tex.pad) + (uvShowPad ? "s" : "");
  for (int id : sel_.items) sig += "," + QByteArray::number(id);
  if (am >= 0) if (const Item* pi = scene_->item(am)) sig += "|p" + QByteArray::number(pi->raw.value("uvPins").toArray().size());   // (the pins drawn)
  sig += "|m" + QByteArray::number(uvo.pad, 'g', 6) + "|p" + QByteArray::number(uvTex(topoMesh()).pad, 'g', 6) + QByteArray::number(uvTex(topoMesh()).w) + QByteArray::number(uvTex(topoMesh()).h);
  if (am >= 0) if (const Item* pi = scene_->item(am)) sig += "|a" + QByteArray::number(qHash(QJsonDocument(pi->raw.value("uvShellAttr").toArray()).toJson(QJsonDocument::Compact)));   // (the locked, the groups)
  if (uvTiles) { sig += "|tiles"; for (const QString& n : uvTileNames()) sig += "," + n.toUtf8(); }
  if (sig == uvSig_) return uvDraw_;
  uvSig_ = sig; uvDraw_ = {}; UvDraw& D = uvDraw_;
  if (uvTiles && room() == "topo" && tset.uv) { uvTilesDraw(D); return D; }   // (All sets as tiles: every set in a tile of its own — nothing else)
  const QColor SEL = uvo.cSel, SEAM = uvo.cSeam, BORDER = uvo.cBorder;   // (the user's colours: here, over the file's defaults)
  if (tset.sym && uvMode()) { UvDraw::Lines a; a.c = QColor(0xc8, 0x6b, 0xff); a.w = 1.6f; a.p = {{float(uvo.symAxis), -3}, {float(uvo.symAxis), 4}}; D.lines.push_back(a); }   // (the UV symmetry's axis)
  // the grid: the 0…1 tile in tenths, the tiles round it, the tile's outline
  { UvDraw::Lines g; g.c = QColor(0x36, 0x36, 0x36); for (int i = 1; i < 10; i++) { float t = i / 10.f; g.p.push_back({t, 0}); g.p.push_back({t, 1}); g.p.push_back({0, t}); g.p.push_back({1, t}); } D.grid.push_back(g);
    UvDraw::Lines o; o.c = QColor(0x44, 0x44, 0x44); for (int i = -3; i <= 4; i++) { o.p.push_back({float(i), -3}); o.p.push_back({float(i), 4}); o.p.push_back({-3, float(i)}); o.p.push_back({4, float(i)}); } D.grid.push_back(o);
    UvDraw::Lines t; t.c = QColor(0x8a, 0x8a, 0x8a); t.w = 1.5f; for (auto [a, b] : {std::pair<std::array<float, 2>, std::array<float, 2>>{{0, 0}, {1, 0}}, {{1, 0}, {1, 1}}, {{1, 1}, {0, 1}}, {{0, 1}, {0, 0}}}) { t.p.push_back(a); t.p.push_back(b); } D.grid.push_back(t); }
  {                                                              // the tile's Margin (the snaps keep it off the edges) and Padding (Pack keeps half of it off the edges, a whole between shells): dashed frames inside the border
    const int tm = topoMesh(); const UvTex tx = uvTex(tm); const double mw = std::max(1, tx.w), mh = std::max(1, tx.h);
    auto dashed = [&](double a0, double b0, double a1, double b1, const QColor& col, float w) { UvDraw::Lines l; l.c = col; l.w = w; const double dl = 0.014;
      auto edge = [&](double x0, double y0, double x1, double y1) { const double len = std::hypot(x1 - x0, y1 - y0); const int n = std::max(1, int(len / dl)); for (int i = 0; i < n; i += 2) { const double t0 = double(i) / n, t1 = std::min(1.0, double(i + 1) / n); l.p.push_back({float(x0 + (x1 - x0) * t0), float(y0 + (y1 - y0) * t0)}); l.p.push_back({float(x0 + (x1 - x0) * t1), float(y0 + (y1 - y0) * t1)}); } };
      edge(a0, b0, a1, b0); edge(a1, b0, a1, b1); edge(a1, b1, a0, b1); edge(a0, b1, a0, b0); D.grid.push_back(l); };
    if (uvo.pad > 0.0005) dashed(uvo.pad, uvo.pad, 1 - uvo.pad, 1 - uvo.pad, QColor(0xe0, 0xa0, 0x30), 1.4f);                                    // (Margin: amber)
    if (tx.pad > 0) dashed(tx.pad / 2 / mw, tx.pad / 2 / mh, 1 - tx.pad / 2 / mw, 1 - tx.pad / 2 / mh, QColor(0x40, 0xc0, 0xd0), 1.2f);        // (Padding: teal)
  }
  if (room() != "topo" || !tset.uv) return D;
  // the other selected meshes: dim
  for (int id : topoMeshes()) {
    if (id == am || !uvShown(id)) continue; const Item* it = scene_->item(id); const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) continue;
    const std::vector<char> hidF = uvHidden(*it, B); UvDraw::Lines l; l.c = sel_.hasItem(id) ? QColor(0x9a, 0xa4, 0xc8) : QColor(0x5c, 0x5c, 0x66);   /* (a selected mesh lighter than the rest: all of them are edited together) */
    int onlySet = -2; if (uvPicked.id >= 0 && uvPicked.set >= 0 && uvPicked.mat == -2 && topo::hasUvSets(B)) { onlySet = -1; const auto S0 = uvSets(uvPicked.id); if (size_t(uvPicked.set) < S0.size()) { const auto Sx = uvSets(id); for (size_t q = 0; q < Sx.size(); q++) if (Sx[q].name == S0[size_t(uvPicked.set)].name) onlySet = int(q); } }   // (a set worked in: of the others, only what is in it — the rest of their polygons not drawn, or they read as part of it)
    for (int f = 0; f < B.faceCount(); f++) { if (!hidF.empty() && hidF[size_t(f)]) continue; if (onlySet != -2 && !(onlySet >= 0 && size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> onlySet & 1u))) continue;
      int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { l.p.push_back(B.cornerUV[size_t(k)]); l.p.push_back(B.cornerUV[size_t(k + 1 < e ? k + 1 : s)]); } }
    D.lines.push_back(std::move(l));
    if (sel_.hasItem(id)) {                                    // (a picked mesh is as active as the first: its stretch, its turned over and overlapping polygons too)
      const auto Tm = topo::ttopo(baseSp(*it));
      if (Tm && Tm->nf < 80000) {
        auto shownX = [&](int f) { return hidF.empty() || size_t(f) >= hidF.size() || !hidF[size_t(f)]; };
        auto ftris = [&](UvDraw::Tris& t, int f, const QColor& col) { const int s = Tm->fo[size_t(f)]; for (int k = s + 1; k + 1 < Tm->fo[size_t(f) + 1]; k++) { t.p.push_back(B.cornerUV[size_t(s)]); t.p.push_back(B.cornerUV[size_t(k)]); t.p.push_back(B.cornerUV[size_t(k + 1)]); t.c.push_back(col); } };
        if (uvo.stretch && uvo.stretchAngle) { const auto a = uv::angleStretch(B, *Tm, tex.asp()); UvDraw::Tris t; t.op = float(0.85 * uvo.dispOp);
          for (int f = 0; f < Tm->nf; f++) if (shownX(f)) { const double q = std::clamp((double(a[size_t(f)]) - 1) / 1.0, 0.0, 1.0);
            const QColor g(0x80, 0x80, 0x88), amb(0xf0, 0xa0, 0x30), rd(0xe0, 0x30, 0x30); const QColor& x0 = q < 0.5 ? g : amb; const QColor& x1 = q < 0.5 ? amb : rd; const double w = q < 0.5 ? q * 2 : (q - 0.5) * 2;
            ftris(t, f, QColor(int(x0.red() + (x1.red() - x0.red()) * w), int(x0.green() + (x1.green() - x0.green()) * w), int(x0.blue() + (x1.blue() - x0.blue()) * w))); }
          D.tris.push_back(std::move(t));
        } else if (uvo.stretch) { double au = 0, a3 = 0; std::vector<double> r(size_t(Tm->nf), 0);
          for (int f = 0; f < Tm->nf; f++) { const double x = std::abs(uv::faceArea(B, f)), y = uv::faceArea3(B, f); au += x; a3 += y; r[size_t(f)] = y > 0 ? x / y : 0; }
          const double k = au > 0 && a3 > 0 ? a3 / au : 1; UvDraw::Tris t; t.op = float(0.85 * uvo.dispOp);
          for (int f = 0; f < Tm->nf; f++) if (shownX(f)) ftris(t, f, stretchColor(r[size_t(f)] * k));
          D.tris.push_back(std::move(t)); }
        if (tset.sym && uvo.symTint) {                          // (the −X half of a picked mesh: burgundy as the first one's)
          UvDraw::Tris t; t.op = 0.85f; for (int f : uvHotOf(id).minusX) if (f < Tm->nf && shownX(f)) ftris(t, f, QColor(0x6e, 0x06, 0x1e));
          if (!t.p.empty()) D.tris.push_back(std::move(t)); }
        const UvHot& hx = uvHotOf(id);                          // (found once per state of its UV — not at every click and every step of a drag)
        if (!hx.flipped.empty()) { UvDraw::Tris t; t.op = 0.5f; for (int f : hx.flipped) if (f < Tm->nf && shownX(f)) ftris(t, f, QColor(0xe0, 0x30, 0x30)); D.tris.push_back(std::move(t)); }
        if (!hx.overlap.empty()) { UvDraw::Tris t; t.op = 0.75f; for (int f : hx.overlap) if (f < Tm->nf && shownX(f)) ftris(t, f, QColor(0xff, 0x1f, 0xe6)); if (!t.p.empty()) D.tris.push_back(std::move(t)); }
      } }
    if (auto sx = tselX_.find(id); sx != tselX_.end() && tKey() == 'f' && sel_.hasItem(id)) {          // (its picked polygons, as the active mesh's)
      UvDraw::Tris t; t.op = 0.55f; for (int f : sx->second.f) { if (f < 0 || f >= B.faceCount()) continue; const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; for (int k = s + 1; k + 1 < e; k++) { t.p.push_back(B.cornerUV[size_t(s)]); t.p.push_back(B.cornerUV[size_t(k)]); t.p.push_back(B.cornerUV[size_t(k + 1)]); t.c.push_back(SEL); } }
      if (!t.p.empty()) D.tris.push_back(std::move(t)); }
  }
  // the meshes of the sets with no UV yet: their projection (the front) above the tile — to see there is something to unfold
  { QString want; if (uvPicked.id >= 0 && uvPicked.set >= 0 && uvPicked.mat == -2) { const auto S0 = uvSets(uvPicked.id); if (size_t(uvPicked.set) < S0.size()) want = S0[size_t(uvPicked.set)].name; } else want = uvFreePicked;
    uvProj_ = {}; struct PJ { const PolyMesh* B; const PolyMesh* W; std::vector<int> fs; int id = -1; }; std::vector<PJ> pj; double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (int id : topoMeshes()) {
      const Item* pi = scene_->item(id); if (!pi || !pi->poly) continue; const PolyMesh& PB = *baseOf(*pi); const bool hasUv = uv::has(PB); if (!uvShown(id) || (!hasUv && !topo::hasUvSets(PB))) continue;   // (a mesh with UV: only its polygons not unfolded yet — their UV has no area)
      int si = -1; if (!want.isEmpty() && (!hasUv || topo::hasUvSets(PB))) { for (size_t s = 0; s < PB.uvSetNames.size(); s++) if (PB.uvSetNames[s] == want) { si = int(s); break; } if (si < 0) continue; }
      PJ p{&PB, pi->poly.get(), {}, id};
      if (hasUv) { std::vector<double> rt; for (int f = 0; f < PB.faceCount(); f++) { const double a = std::abs(uv::faceArea(PB, f)), a3 = uv::faceArea3(PB, f); if (a3 > 1e-12 && a > 1e-12) rt.push_back(a / a3); }
        double med = 0; if (!rt.empty()) { std::nth_element(rt.begin(), rt.begin() + long(rt.size() / 2), rt.end()); med = rt[rt.size() / 2]; }   // (not unfolded: no UV area, or a sliver: under a thousandth of the mesh's usual UV area to 3D area)
        for (int f = 0; f < PB.faceCount(); f++) { const double a = std::abs(uv::faceArea(PB, f)), a3 = uv::faceArea3(PB, f); if (a3 > 1e-12 && (si < 0 || (size_t(f) < PB.faceSets.size() && (PB.faceSets[size_t(f)] >> si & 1u))) && (a < 1e-10 || (med > 0 && a / a3 < med * 1e-3))) p.fs.push_back(f); } }
      else for (int f = 0; f < PB.faceCount(); f++) if (si >= 0 ? (size_t(f) < PB.faceSets.size() && (PB.faceSets[size_t(f)] >> si & 1u)) : (size_t(f) < PB.faceSets.size() && PB.faceSets[size_t(f)])) p.fs.push_back(f);
      if (p.fs.empty() || p.W->pos.size() != PB.pos.size()) continue;
      for (int f : p.fs) for (int k = PB.faceStart[size_t(f)]; k < PB.faceStart[size_t(f) + 1]; k++) { const Vec3& q = p.W->pos[size_t(PB.faceVerts[size_t(k)])]; x0 = std::min(x0, q.x); x1 = std::max(x1, q.x); y0 = std::min(y0, q.y); y1 = std::max(y1, q.y); }
      pj.push_back(std::move(p)); }
    if (!pj.empty() && x1 > x0 && y1 > y0) {   /* (the polygons not unfolded, above the tile — a small island is easy to miss on the model) */
      const double sc = 1.0 / std::max(x1 - x0, y1 - y0); const double ox = 0.5 - (x1 - x0) * sc * 0.5;
      uvProj_.on = true; uvProj_.ox = ox; uvProj_.x0 = x0; uvProj_.y0 = y0; uvProj_.sc = sc; for (const PJ& p : pj) uvProj_.meshes.push_back({p.id, p.fs});   // (for picking in it)
      { UvDraw::Tris st; st.op = 0.6f;                                  // (the picked ones of them lit)
        for (const PJ& p : pj) { const std::set<int>* sel = p.id == am ? &tsel_.f : (tselX_.count(p.id) ? &tselX_.at(p.id).f : nullptr); if (!sel || tKey() != 'f') continue;
          for (int f : p.fs) { if (!sel->count(f)) continue; const int s = p.B->faceStart[size_t(f)], e = p.B->faceStart[size_t(f) + 1];
            auto pt = [&](int k) { const Vec3& a = p.W->pos[size_t(p.B->faceVerts[size_t(k)])]; return std::array<float, 2>{float(ox + (a.x - x0) * sc), float(1.12 + (a.y - y0) * sc)}; };
            for (int k = s + 1; k + 1 < e; k++) { st.p.push_back(pt(s)); st.p.push_back(pt(k)); st.p.push_back(pt(k + 1)); st.c.push_back(SEL); } } }
        if (!st.p.empty()) D.tris.push_back(std::move(st)); }
      UvDraw::Lines l; l.c = QColor(0x8c, 0x92, 0xa0); l.w = 1.f;
      for (const PJ& p : pj) for (int f : p.fs) { const int s = p.B->faceStart[size_t(f)], e = p.B->faceStart[size_t(f) + 1];
        for (int k = s; k < e; k++) { const Vec3& a = p.W->pos[size_t(p.B->faceVerts[size_t(k)])]; const Vec3& b = p.W->pos[size_t(p.B->faceVerts[size_t(k + 1 < e ? k + 1 : s)])];
          l.p.push_back({float(ox + (a.x - x0) * sc), float(1.12 + (a.y - y0) * sc)}); l.p.push_back({float(ox + (b.x - x0) * sc), float(1.12 + (b.y - y0) * sc)}); } }
      D.lines.push_back(std::move(l));
      { UvDraw::Lines fr; fr.c = QColor(0x6a, 0x72, 0x86); fr.w = 1.4f;       // (their own frame: the box they stand in — not the tile)
        const float fx0 = float(ox - 0.03), fx1 = float(ox + (x1 - x0) * sc + 0.03), fy0 = 1.12f - 0.03f, fy1 = float(1.12 + (y1 - y0) * sc + 0.03);
        fr.p = {{fx0, fy0}, {fx1, fy0}, {fx1, fy0}, {fx1, fy1}, {fx1, fy1}, {fx0, fy1}, {fx0, fy1}, {fx0, fy0}}; D.lines.push_back(std::move(fr)); } } }
  if (am < 0) return D;
  const Item* it = scene_->item(am); const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) return D;
  const UvCache* c = uvCache(); if (!c) return D; const topo::TTopo& T = *c->T;
  if (uvo.tex && looks::shown(*it)) { D.texMesh = am; if (uvPicked.id == am) D.texMat = uvPicked.mat; }
  D.checker = uvo.checker; D.op = float(uvo.dispOp);                // (the checker under the UV first; the opacity of what is under it)
  if (D.checker) for (int id : uvMeshes()) { const Item* ci = scene_->item(id); if (!ci || !uvShown(id)) continue; const PolyMesh& CB = *baseOf(*ci); if (!uv::has(CB)) continue; const auto ch = uvHidden(*ci, CB);   // (only inside the shells: the gaps stay dark — the layout is seen)
    for (int f = 0; f < CB.faceCount(); f++) { if (!ch.empty() && size_t(f) < ch.size() && ch[size_t(f)]) continue; const int s = CB.faceStart[size_t(f)], e = CB.faceStart[size_t(f) + 1]; for (int k = s + 1; k + 1 < e; k++) { D.chk.push_back(CB.cornerUV[size_t(s)]); D.chk.push_back(CB.cornerUV[size_t(k)]); D.chk.push_back(CB.cornerUV[size_t(k + 1)]); } } }
  const std::vector<char> hidF = uvHidden(*it, B); auto shown = [&](int f) { return hidF.empty() || size_t(f) >= hidF.size() || !hidF[size_t(f)]; };
  auto faceTris = [&](UvDraw::Tris& t, int f, const QColor& col) { int s = T.fo[size_t(f)]; for (int k = s + 1; k + 1 < T.fo[size_t(f) + 1]; k++) { t.p.push_back(B.cornerUV[size_t(s)]); t.p.push_back(B.cornerUV[size_t(k)]); t.p.push_back(B.cornerUV[size_t(k + 1)]); t.c.push_back(col); } };
  // the stretch: each polygon's UV area to its 3D area, against the mesh's (red — squeezed, blue — stretched)
  if (uvo.stretch && uvo.stretchAngle) {                         // (the angles: grey kept, through amber to red — twice as long one way as the other)
    const auto a = uv::angleStretch(B, T, tex.asp()); UvDraw::Tris t; t.op = float(0.85 * uvo.dispOp);
    for (int f = 0; f < T.nf; f++) if (shown(f)) { const double q = std::clamp((double(a[size_t(f)]) - 1) / 1.0, 0.0, 1.0);
      const QColor g(0x80, 0x80, 0x88), am(0xf0, 0xa0, 0x30), rd(0xe0, 0x30, 0x30); const QColor& x0 = q < 0.5 ? g : am; const QColor& x1 = q < 0.5 ? am : rd; const double w = q < 0.5 ? q * 2 : (q - 0.5) * 2;
      faceTris(t, f, QColor(int(x0.red() + (x1.red() - x0.red()) * w), int(x0.green() + (x1.green() - x0.green()) * w), int(x0.blue() + (x1.blue() - x0.blue()) * w))); }
    D.tris.push_back(std::move(t));
  } else if (uvo.stretch) {
    double au = 0, a3 = 0; std::vector<double> r(size_t(T.nf), 0);
    for (int f = 0; f < T.nf; f++) { double x = std::abs(uv::faceArea(B, f)), y = uv::faceArea3(B, f); au += x; a3 += y; r[size_t(f)] = y > 0 ? x / y : 0; }
    double k = au > 0 && a3 > 0 ? a3 / au : 1; UvDraw::Tris t; t.op = float(0.85 * uvo.dispOp);
    for (int f = 0; f < T.nf; f++) if (shown(f)) { double q = r[size_t(f)] * k; QColor col;
      col = stretchColor(q);
      faceTris(t, f, col); }
    D.tris.push_back(std::move(t));
  }
  // turned over: red
  { const auto& fl = uvHotOf(am).flipped; if (!fl.empty()) { UvDraw::Tris t; t.op = 0.5f; for (int f : fl) if (f < T.nf && shown(f)) faceTris(t, f, QColor(0xe0, 0x30, 0x30)); D.tris.push_back(std::move(t)); } }
  // the checks shown: over another — magenta, past the tile — orange, the padding — yellow, the density — cyan, a mirror
  // pair not mirrored — violet (the later over the earlier)
  // overlaps (a shell on another, a shell on itself): always shown, in a loud colour — the polygons that lie over others are never left unseen
  if (am >= 0 && T.nf < 80000) { const auto& ro = uvCheck(); if (!ro.overlap.empty()) { UvDraw::Tris to; to.op = 0.75f; for (int f : ro.overlap) if (f < T.nf && shown(f)) faceTris(to, f, QColor(0xff, 0x1f, 0xe6)); if (!to.p.empty()) D.tris.push_back(std::move(to)); } }
  if (uvo.diag) { const auto& r = uvCheck(); UvDraw::Tris t; t.op = 0.45f;
    for (auto [fs, col] : {std::pair<const std::vector<int>*, QColor>{&r.asym, QColor(0x90, 0x60, 0xff)}, {&r.density, QColor(0x30, 0xc0, 0xe0)}, {&r.padding, QColor(0xf0, 0xd0, 0x20)}, {&r.outside, QColor(0xff, 0x8c, 0x1a)}, {&r.overlap, QColor(0xe0, 0x40, 0xe0)}})
      for (int f : *fs) if (f < T.nf && shown(f)) faceTris(t, f, col);
    if (!t.p.empty()) D.tris.push_back(std::move(t)); }
  // the set worked in: what is not in it darkened (its polygons, their edges dim)
  std::vector<char> dimF; if (const int ws = uvWorkSet(am, B); ws >= 0 && topo::hasUvSets(B)) {
    dimF.assign(size_t(T.nf), 0); UvDraw::Tris t; t.op = 0.82f;   // (well dark: what is not in the set must not read as part of it)
    for (int f = 0; f < T.nf; f++) if (!(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> ws & 1u))) { dimF[size_t(f)] = 1; if (shown(f)) faceTris(t, f, QColor(0x08, 0x08, 0x0c)); }
    if (!t.p.empty()) D.tris.push_back(std::move(t)); }
  auto dim = [&](int f) { return !dimF.empty() && dimF[size_t(f)]; };
  if (tset.sym && uvo.symTint) { const auto mx = uvMinusXFaces(); UvDraw::Tris t; t.op = 0.85f;   // (the −X half: burgundy, dark and saturated)
    for (int f = 0; f < T.nf && size_t(f) < mx.size(); f++) if (mx[size_t(f)] && shown(f) && !dim(f)) faceTris(t, f, QColor(0x6e, 0x06, 0x1e));
    if (!t.p.empty()) D.tris.push_back(std::move(t)); }
  // each shell's own settings: a locked one slate (its outline too, below); a pack group — a colour of its own
  const auto& shAttr = uvHotOf(am).attrs; bool anyLock = false;
  { static const QColor gc[] = {QColor(0x9c, 0x64, 0xf0), QColor(0x4a, 0x6c, 0xe0), QColor(0x3c, 0xb4, 0x6e), QColor(0x28, 0xa0, 0xb4), QColor(0xd2, 0x5a, 0x96), QColor(0x6e, 0x8c, 0xa8)};   // (no yellow or orange among them: the selection is that colour, it must stand out on any shell)
    UvDraw::Tris lt, gt; lt.op = 0.62f; gt.op = 0.55f;
    for (int f = 0; f < T.nf && size_t(f) < shAttr.size(); f++) { if (!shown(f) || dim(f)) continue; const auto& a = shAttr[size_t(f)];
      if (a.lock) { anyLock = true; faceTris(lt, f, QColor(0x5a, 0x6a, 0x8c)); } else if (a.group > 0) faceTris(gt, f, gc[(a.group - 1) % 6]); }
    for (auto* t : {&lt, &gt}) if (!t->p.empty()) D.tris.push_back(std::move(*t)); }
  // the padding: round each shell a band of half the padding (in the texture's pixels) — two bands overlapping (darker)
  // or one past the tile's edge: closer than the padding
  if (uvShowPad && tex.pad > 0) {
    const double hu = tex.pad / 2 / tex.w, hv = tex.pad / 2 / tex.h; UvDraw::Tris t; t.op = 0.22f; const QColor pc(0x6a, 0xd0, 0xff);
    auto disc = [&](const std::array<float, 2>& c) { const int n = 10; for (int i = 0; i < n; i++) { const double a0 = 2 * M_PI * i / n, a1 = 2 * M_PI * (i + 1) / n;
      t.p.push_back(c); t.p.push_back({float(c[0] + hu * std::cos(a0)), float(c[1] + hv * std::sin(a0))}); t.p.push_back({float(c[0] + hu * std::cos(a1)), float(c[1] + hv * std::sin(a1))}); t.c.push_back(pc); } };
    for (int f = 0; f < T.nf; f++) { if (!shown(f) || dim(f)) continue; const double sg = uv::faceArea(B, f) >= 0 ? 1 : -1;
      for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; if (c->cont[size_t(e)]) continue;
        const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; const auto &a = B.cornerUV[size_t(k)], &b = B.cornerUV[size_t(nk)];
        double dx = (double(b[0]) - a[0]) * tex.w, dy = (double(b[1]) - a[1]) * tex.h; const double L = std::hypot(dx, dy); if (!(L > 0)) continue;
        const double nx = sg * dy / L * hu, ny = -sg * dx / L * hv;          // (outwards: to the right of a wound-on outline)
        const std::array<float, 2> a2{float(a[0] + nx), float(a[1] + ny)}, b2{float(b[0] + nx), float(b[1] + ny)};
        t.p.insert(t.p.end(), {a, b, b2, a, b2, a2}); t.c.push_back(pc); t.c.push_back(pc); disc(a); } }
    if (!t.p.empty()) D.tris.push_back(std::move(t));
  }
  const char key = tKey(); const bool comp = topoMode != "object";
  if (comp && key == 'f' && tsel_.id == am) { UvDraw::Tris t; t.op = 0.38f; for (int f : tsel_.f) if (f < T.nf && shown(f)) faceTris(t, f, SEL); D.tris.push_back(std::move(t)); }
  // (Symmetry: what the mirror of the pick covers is shown picked too — only shown: the pick itself, its centre and the moves are the picked side's; the mirror follows)
  std::set<int> ghostF; std::map<int, std::set<int>> ghostE;
  if (tset.sym && comp && tsel_.id == am) { if (key == 'f' && !tsel_.f.empty()) for (int f : tSymList(am, std::vector<int>(tsel_.f.begin(), tsel_.f.end()), 'f')) { if (!tsel_.f.count(f)) ghostF.insert(f); }
    if (key == 'e' && !tsel_.e.empty()) ghostE = uvMirrorSides(am, tsel_); }
  if (!ghostF.empty()) { UvDraw::Tris t; t.op = 0.38f; for (int f : ghostF) if (f < T.nf && shown(f)) faceTris(t, f, SEL); D.tris.push_back(std::move(t)); }
  auto ghostLit = [&](int e, int f) { const auto g = ghostE.find(e); return g != ghostE.end() && (g->second.empty() || g->second.count(f) > 0); };   // (the mirror of a side: that side only)
  // the edges; the shells' outlines; the seams; the selected edges
  const std::vector<char>& seam = c->seam; const std::vector<char>& cont = c->cont;
  UvDraw::Lines dl, all, out, sm, se; dl.c = QColor(0x4a, 0x4a, 0x52); all.c = uvo.cWire; out.c = BORDER; out.w = 1.8f; sm.c = SEAM; sm.w = 2.6f; se.c = SEL; se.w = 2.8f;
  for (int f = 0; f < T.nf; f++) if (shown(f)) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) {
    int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)], e = T.ce[size_t(k)]; const auto &a = B.cornerUV[size_t(k)], &b = B.cornerUV[size_t(nk)];
    UvDraw::Lines& L = dim(f) ? dl : seam[size_t(e)] ? sm : !cont[size_t(e)] ? out : all; L.p.push_back(a); L.p.push_back(b);
    if (comp && key == 'e' && tsel_.id == am && ((tsel_.e.count(e) && (cont[size_t(e)] || uvSideOn(tsel_, e, f))) || ghostLit(e, f))) { se.p.push_back(a); se.p.push_back(b); }
  }
  for (auto* L : {&dl, &all, &out, &sm}) if (!L->p.empty()) D.lines.push_back(std::move(*L));
  if (anyLock) { UvDraw::Lines ll; ll.c = QColor(0xc8, 0xd4, 0xf0); ll.w = 3.0f;   // (the locked shells' outlines)
    for (int f = 0; f < T.nf && size_t(f) < shAttr.size(); f++) if (shown(f) && shAttr[size_t(f)].lock) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) {
      if (c->cont[size_t(T.ce[size_t(k)])]) continue; const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; ll.p.push_back(B.cornerUV[size_t(k)]); ll.p.push_back(B.cornerUV[size_t(nk)]); }
    if (!ll.p.empty()) D.lines.push_back(std::move(ll)); }
  { const auto feats = uvFeaturesOf(am);                              // (the face's lips / lids: the upper rim orange, the lower pink — shut: amber; a dot at each corner)
    UvDraw::Lines up, lo, cl; up.c = QColor(0xff, 0x9a, 0x3c); lo.c = QColor(0xff, 0x5c, 0xa8); cl.c = QColor(0xff, 0xd0, 0x40); up.w = lo.w = cl.w = 2.6f; UvDraw::Pts cp; cp.size = 8;
    for (auto& f : feats) { int side = 0; for (auto* c : {&f.a, &f.b}) { UvDraw::Lines& L = f.closed ? cl : (side == 0 ? up : lo); side++;
      for (size_t i = 0; i + 1 < c->size(); i++) {
        const int e = T.edgeOf((*c)[i], (*c)[i + 1]); if (e < 0) continue;
        for (int fi : {T.ef0[size_t(e)], T.ef1[size_t(e)]}) { if (fi < 0 || !shown(fi) || dim(fi)) continue; const int k0 = uv::cornerAt(T, fi, (*c)[i]), k1 = uv::cornerAt(T, fi, (*c)[i + 1]);
          if (k0 >= 0 && k1 >= 0) { L.p.push_back(B.cornerUV[size_t(k0)]); L.p.push_back(B.cornerUV[size_t(k1)]); } } } }
      for (int v : {f.a.front(), f.a.back()}) for (int i = T.vfo[size_t(v)]; i < T.vfo[size_t(v) + 1]; i++) { const int fi = T.vF[size_t(i)]; if (!shown(fi) || dim(fi)) continue; const int k = uv::cornerAt(T, fi, v); if (k >= 0) { cp.p.push_back(B.cornerUV[size_t(k)]); cp.c.push_back(QColor(0xff, 0xff, 0xff)); break; } } }
    for (auto* L : {&up, &lo, &cl}) if (!L->p.empty()) D.lines.push_back(std::move(*L)); if (!cp.p.empty()) D.pts.push_back(std::move(cp)); }  { const uv::Pins pins = uvPinsOf(am);                               // (the pinned edges: yellow, over the rest)
    if (!pins.empty()) {                                              // (each kind of constraint its own colour: horizontal blue, vertical green, a line as it is violet, other pins yellow)
      std::map<std::pair<int, int>, QString> kind; for (const QJsonValue& x : scene_->item(am)->raw.value("uvPins").toArray()) { const QJsonArray q = x.toArray(); if (q.size() >= 3 && q[0].toInt() != q[1].toInt()) { const int e0 = q.size() >= 4 ? T.edgeOf(q[0].toInt(), q[1].toInt()) : -1; if (e0 >= 0 && q[3].toInt() >= 0) kind[{-1 - q[3].toInt(), e0}] = q[2].toString(); else kind[{std::min(q[0].toInt(), q[1].toInt()), std::max(q[0].toInt(), q[1].toInt())}] = q[2].toString(); } }
      UvDraw::Lines ph, pv, pa, pl; ph.c = QColor(0x3c, 0xb4, 0xff); pv.c = QColor(0x5c, 0xd8, 0x6a); pa.c = QColor(0xc8, 0x6a, 0xff); pl.c = QColor(0xff, 0xe0, 0x3a); ph.w = pv.w = pa.w = pl.w = 3.2f;
      for (int f = 0; f < T.nf; f++) if (shown(f) && !dim(f)) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
        const int a = T.fv[size_t(k)], b = T.fv[size_t(nk)]; std::pair<int, int> key2{std::min(a, b), std::max(a, b)}; const std::pair<int, int> keyS{-1 - f, T.ce[size_t(k)]};
        if (const int al = uv::edgeAligned(pins, f, T.ce[size_t(k)])) { UvDraw::Lines& L = al == 1 ? ph : pv; L.p.push_back(B.cornerUV[size_t(k)]); L.p.push_back(B.cornerUV[size_t(nk)]); continue; }   // (an alignment: blue across, green up)
        if (pins.count(keyS)) key2 = keyS; else if (!pins.count(key2)) continue;   // (one shell's constraint: seen from its polygon only)
        auto kd = kind.find(key2); if (kd == kind.end()) kd = kind.find({std::min(a, b), std::max(a, b)});   /* (an old constraint with no side kept: its kind by its vertices) */ const QString t = kd == kind.end() ? QString() : kd->second; UvDraw::Lines& L = t == "h" ? ph : t == "v" ? pv : t == "a" ? pa : pl;
        L.p.push_back(B.cornerUV[size_t(k)]); L.p.push_back(B.cornerUV[size_t(nk)]); }
      for (UvDraw::Lines* L : {&ph, &pv, &pa, &pl}) if (!L->p.empty()) D.lines.push_back(std::move(*L)); }
    if (!pins.empty()) { UvDraw::Pts pp; pp.size = 7;                 // (the pinned points: yellow dots)
      for (int k = 0; k < int(T.fv.size()); k++) { const int v = T.fv[size_t(k)]; if (uv::pointPinned(pins, v, T.cf[size_t(k)]) && shown(T.cf[size_t(k)]) && !dim(T.cf[size_t(k)])) { pp.p.push_back(B.cornerUV[size_t(k)]); pp.c.push_back(QColor(0xff, 0xe0, 0x3a)); } }
      if (!pp.p.empty()) D.pts.push_back(std::move(pp)); } }
  uvPadOutline(D, tex);
  if (!se.p.empty()) { UvDraw::Lines cs; cs.c = QColor(0xff, 0xff, 0xff); cs.w = 5.2f; cs.p = se.p; D.lines.push_back(std::move(cs)); D.lines.push_back(std::move(se)); }   // (the selected edges: over the pins, the lids, the locked outlines, with a white edging — they are seen on a red seam too)
  // the points (Vertex mode)
  if (comp && key == 'v') {
    std::set<int> pts = uvPoints(); if (tset.sym) for (int g : uvGhostPts()) pts.insert(g); UvDraw::Pts p; p.size = 5;
    for (int q = 0; q < c->P.count(); q++) { int k = c->P.list[size_t(c->P.off[size_t(q)])]; if (!shown(T.cf[size_t(k)])) continue; p.p.push_back(B.cornerUV[size_t(k)]); p.c.push_back(pts.count(q) ? SEL : QColor(0xd8, 0xd8, 0xd8)); }
    D.pts.push_back(std::move(p));
  }
  if (uvTf_.on) { if (uvTf_.m != am || topoMode == "object") uvTf_ = {}; else uvTfDraw(D); }
  return D;
}

void Viewport::saveUvColors() const {
  QJsonObject o; for (auto [k, c] : std::initializer_list<std::pair<const char*, const QColor*>>{{"sel", &uvo.cSel}, {"seam", &uvo.cSeam}, {"border", &uvo.cBorder}, {"wire", &uvo.cWire}, {"padOk", &uvo.cPadOk}, {"padBad", &uvo.cPadBad}}) o.insert(k, c->name());
  QSettings().setValue("topo/uvColors", QJsonDocument(o).toJson(QJsonDocument::Compact));
}
void Viewport::loadUvColors() {
  const QJsonObject o = QJsonDocument::fromJson(QSettings().value("topo/uvColors").toByteArray()).object();
  for (auto [k, c] : std::initializer_list<std::pair<const char*, QColor*>>{{"sel", &uvo.cSel}, {"seam", &uvo.cSeam}, {"border", &uvo.cBorder}, {"wire", &uvo.cWire}, {"padOk", &uvo.cPadOk}, {"padBad", &uvo.cPadBad}}) { const QColor x(o.value(k).toString()); if (x.isValid()) *c = x; }
}

// a mesh's UV facts that take long to find, kept while its UV stays as it was: a click only changes the selection, and
// a drag of shells moves them whole — what is turned over, lying on another, in which shell stays what it was at the press
const Viewport::UvHot& Viewport::uvHotOf(int id) {
  UvHot& h = uvHot_[id]; const Item* it = scene_->item(id); if (!it) return h;
  const auto Bs = baseSp(*it); const PolyMesh& B = *Bs; const auto T = topo::ttopo(Bs); if (!T) return h;
  const bool dragging = drag_ == Drag::Uv && uvd_.type == 4 && uvd_.moved; const uint aH = qHash(QJsonDocument(it->raw.value("uvShellAttr").toArray()).toJson(QJsonDocument::Compact));
  const bool fits = h.nf == T->nf && h.ne == T->ne && h.sym == tset.sym;
  if (fits && dragging) return h;
  const size_t uvH = uv::has(B) ? qHashBits(B.cornerUV.data(), B.cornerUV.size() * sizeof(B.cornerUV[0])) : 0;   // (by what the UV is: a click on another mesh, a redraw for any other reason leave it as it was)
  if (fits && h.uvh == uvH && h.attr == aH) return h;
  h = {}; h.uvh = uvH; h.attr = aH; h.nf = T->nf; h.ne = T->ne; h.sym = tset.sym;
  if (!uv::has(B)) return h;
  const uv::Points P = uv::points(B, *T); const uv::Shells S = uv::shells(B, *T, P);
  h.shellOf = S.of; h.flipped = uv::flipped(B, *T, S); h.attrs = uvShellAttrs(id);
  h.cont.resize(size_t(T->ne)); for (int e = 0; e < T->ne; e++) h.cont[size_t(e)] = uv::continuous(B, *T, P, e);
  // (each UV set and each UV Mat apart — they lie in tiles of their own, one never over another. All the mesh's polygons checked as one tile showed
  //  the Cloth set's dress "over" the Head and Body sets' shells: magenta where nothing overlaps, on every mesh but the active one, 2026-11-10)
  if (T->nf < 80000) { const UvTex tx = uvTex(id); uv::CheckOpts co; co.w = tx.w; co.h = tx.h; co.pad = 0; const bool sets = topo::hasUvSets(B);
    std::map<std::pair<int, int>, std::vector<int>> by; for (int f = 0; f < T->nf; f++) { const int fm = size_t(f) < B.faceMat.size() && B.faceMat[size_t(f)] >= 0 && size_t(B.faceMat[size_t(f)]) < B.matNames.size() ? B.faceMat[size_t(f)] : -1;
      by[{sets && size_t(f) < B.faceLive.size() ? B.faceLive[size_t(f)] : 0, fm}].push_back(f); }
    for (const auto& [k, fs] : by) { const auto o = uv::check(B, *T, fs, co).overlap; h.overlap.insert(h.overlap.end(), o.begin(), o.end()); } }
  if (tset.sym) { const auto mc = uv::mirrorCorners(B, *T); const double eps = std::max(1e-9, B.bounds().size().length() * 1e-4);
    for (int f = 0; f < T->nf; f++) { const int s = T->fo[size_t(f)], e = T->fo[size_t(f) + 1]; if (s >= e || size_t(s) >= mc.size() || mc[size_t(s)] < 0) continue;
      double x = 0; for (int k = s; k < e; k++) x += B.pos[size_t(T->fv[size_t(k)])].x; if (x / (e - s) < -eps) h.minusX.push_back(f); } }
  return h;
}

// round every shell shown a thin line at half the padding: blue — it keeps its padding; red — where it comes closer than
// the padding to another shell (of any mesh shown) or its line goes past the tile
void Viewport::uvPadOutline(UvDraw& D, const UvTex& tex) {
  if (tex.pad <= 0 || tex.w <= 0 || tex.h <= 0) return;
  struct Seg { double ax, ay, bx, by; int sh; }; std::vector<Seg> segs; std::vector<std::array<float, 2>> off, joins;   // (segs in the texture's pixels; off — two ends a segment)
  const double hu = tex.pad / 2.0 / tex.w, hv = tex.pad / 2.0 / tex.h; int shBase = 0; std::unordered_map<int, qint64> stackOf;
  std::vector<int> ids = topoMeshes(); const int am = topoMesh(); if (am >= 0 && std::find(ids.begin(), ids.end(), am) == ids.end()) ids.push_back(am);
  for (int id : ids) { const Item* it = scene_->item(id); if (!it || !uvShown(id)) continue; const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) continue;
    const auto T = topo::ttopo(baseSp(*it)); if (!T) continue; const UvHot& h = uvHotOf(id); if (h.cont.size() != size_t(T->ne) || h.shellOf.size() != size_t(T->nf)) continue;
    const std::vector<char> hid = uvHidden(*it, B); std::map<std::pair<int, std::array<float, 2>>, std::array<float, 2>> outN; int maxS = 0;   // (the shell and a border point: the shifted start of the segment leaving it)
    int onlySet = -2; if (uvPicked.id >= 0 && uvPicked.set >= 0 && uvPicked.mat == -2 && topo::hasUvSets(B)) { onlySet = -1; const auto S0 = uvSets(uvPicked.id); if (size_t(uvPicked.set) < S0.size()) { const auto Sx = uvSets(id); for (size_t q = 0; q < Sx.size(); q++) if (Sx[q].name == S0[size_t(uvPicked.set)].name) onlySet = int(q); } }   // (a set worked in: the padding outline of its shells only)
    struct E { std::array<float, 2> b, b2; int sh; }; std::vector<E> ends;
    { const auto at = uvShellAttrs(id); for (int f = 0; f < T->nf && size_t(f) < at.size(); f++) if (at[size_t(f)].stack > 0) stackOf[shBase + h.shellOf[size_t(f)]] = qint64(id) * 100000 + at[size_t(f)].stack; }   // (its Overlap stacks: their shells lie on each other on purpose)
    for (int f = 0; f < T->nf; f++) { maxS = std::max(maxS, h.shellOf[size_t(f)]); if (!hid.empty() && size_t(f) < hid.size() && hid[size_t(f)]) continue; if (onlySet != -2 && !(onlySet >= 0 && size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> onlySet & 1u))) continue; const double sg = uv::faceArea(B, f) >= 0 ? 1 : -1;
      for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { if (h.cont[size_t(T->ce[size_t(k)])]) continue;
        const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const auto& a = B.cornerUV[size_t(k)]; const auto& b = B.cornerUV[size_t(nk)];
        const double dx = (double(b[0]) - a[0]) * tex.w, dy = (double(b[1]) - a[1]) * tex.h, L = std::hypot(dx, dy); if (!(L > 0)) continue;
        const double nx = sg * dy / L * hu, ny = -sg * dx / L * hv; const int sh = shBase + h.shellOf[size_t(f)];
        const std::array<float, 2> a2{float(a[0] + nx), float(a[1] + ny)}, b2{float(b[0] + nx), float(b[1] + ny)};
        segs.push_back({double(a[0]) * tex.w, double(a[1]) * tex.h, double(b[0]) * tex.w, double(b[1]) * tex.h, sh}); off.push_back(a2); off.push_back(b2);
        outN[{sh, a}] = a2; ends.push_back({b, b2, sh}); } }
    for (const E& e : ends) if (auto n = outN.find({e.sh, e.b}); n != outN.end()) { joins.push_back(e.b2); joins.push_back(n->second); }
    shBase += maxS + 1; }
  if (segs.empty()) return;
  // (two shells lying on each other on purpose — one Overlap stack, or alike shells stacked (the same box): never red for each other)
  std::unordered_map<int, std::array<double, 4>> sbox; for (const Seg& s : segs) { auto& b = sbox.emplace(s.sh, std::array<double, 4>{1e300, 1e300, -1e300, -1e300}).first->second; b[0] = std::min({b[0], s.ax, s.bx}); b[1] = std::min({b[1], s.ay, s.by}); b[2] = std::max({b[2], s.ax, s.bx}); b[3] = std::max({b[3], s.ay, s.by}); }
  auto together = [&](int a, int b) { if (auto x = stackOf.find(a), y = stackOf.find(b); x != stackOf.end() && y != stackOf.end() && x->second == y->second) return true;
    const auto& A = sbox[a]; const auto& B = sbox[b]; return std::abs(A[0] - B[0]) < 1 && std::abs(A[1] - B[1]) < 1 && std::abs(A[2] - B[2]) < 1 && std::abs(A[3] - B[3]) < 1; };
  // who is too close: the segments by cells of the texture, a pair of different shells nearer than the padding
  std::vector<char> hot(segs.size(), 0); const double g = std::max(tex.pad * 3.0, 16.0), r = tex.pad / 2.0;
  auto cellsOf = [&](const Seg& s, auto&& fn) { const int x0 = int(std::floor((std::min(s.ax, s.bx) - r) / g)), x1 = int(std::floor((std::max(s.ax, s.bx) + r) / g)), y0 = int(std::floor((std::min(s.ay, s.by) - r) / g)), y1 = int(std::floor((std::max(s.ay, s.by) + r) / g));
    for (int x = x0; x <= x1; x++) for (int y = y0; y <= y1; y++) fn((qint64(x) << 32) ^ qint64(uint(y))); };
  auto ptSeg = [](double px, double py, const Seg& s) { const double dx = s.bx - s.ax, dy = s.by - s.ay, l = dx * dx + dy * dy, t = l > 0 ? std::clamp(((px - s.ax) * dx + (py - s.ay) * dy) / l, 0.0, 1.0) : 0.0; return std::hypot(px - s.ax - t * dx, py - s.ay - t * dy); };
  auto cross = [](const Seg& p, const Seg& q) { auto o = [](double ax, double ay, double bx, double by, double cx, double cy) { return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); };
    return o(p.ax, p.ay, p.bx, p.by, q.ax, q.ay) * o(p.ax, p.ay, p.bx, p.by, q.bx, q.by) < 0 && o(q.ax, q.ay, q.bx, q.by, p.ax, p.ay) * o(q.ax, q.ay, q.bx, q.by, p.bx, p.by) < 0; };
  // (a segment over too many cells — a shell blown up far past the tile, or not a number — is not put into the grid: it is shown red at once.
  // One such segment over a million cells filled memory without end and the program hung on drawing, 2026-11-10)
  auto tooBig = [&](const Seg& s) { if (!std::isfinite(s.ax) || !std::isfinite(s.ay) || !std::isfinite(s.bx) || !std::isfinite(s.by)) return true;
    if (std::max({std::abs(s.ax), std::abs(s.ay), std::abs(s.bx), std::abs(s.by)}) / g > 1e8) return true;   // (past what a cell's number holds)
    return ((std::abs(s.bx - s.ax) + 2 * r) / g + 2) * ((std::abs(s.by - s.ay) + 2 * r) / g + 2) > 3e5; };      // (an edge corner to corner of an 8K tile is ~2.6e5 cells)
  for (size_t i = 0; i < segs.size(); i++) if (tooBig(segs[i])) hot[i] = 2;
  if (segs.size() < 200000) { std::unordered_map<qint64, std::vector<int>> grid; grid.reserve(segs.size() * 2); for (int i = 0; i < int(segs.size()); i++) if (hot[size_t(i)] != 2) cellsOf(segs[size_t(i)], [&](qint64 c) { grid[c].push_back(i); });
    for (const auto& [c, v] : grid) for (size_t a = 0; a < v.size(); a++) for (size_t b = a + 1; b < v.size(); b++) { const Seg& p = segs[size_t(v[a])]; const Seg& q = segs[size_t(v[b])]; if (p.sh == q.sh || (hot[size_t(v[a])] && hot[size_t(v[b])]) || together(p.sh, q.sh)) continue;
      const double d = std::min({ptSeg(p.ax, p.ay, q), ptSeg(p.bx, p.by, q), ptSeg(q.ax, q.ay, p), ptSeg(q.bx, q.by, p)}); if (d < tex.pad - 1e-6 || cross(p, q)) hot[size_t(v[a])] = hot[size_t(v[b])] = 1; } }
  UvDraw::Lines ok, bad; ok.c = uvo.cPadOk; ok.w = 1.0f; bad.c = uvo.cPadBad; bad.w = 1.8f;
  auto out = [](const std::array<float, 2>& p) { return p[0] < 0 || p[0] > 1 || p[1] < 0 || p[1] > 1; };
  for (size_t i = 0; i < segs.size(); i++) { UvDraw::Lines& L = hot[i] || out(off[2 * i]) || out(off[2 * i + 1]) ? bad : ok; L.p.push_back(off[2 * i]); L.p.push_back(off[2 * i + 1]); }
  ok.p.insert(ok.p.end(), joins.begin(), joins.end());
  if (!ok.p.empty()) D.lines.push_back(std::move(ok)); if (!bad.p.empty()) D.lines.push_back(std::move(bad));
}

// the element under the pointer (its polygon, its edge's side, its point): a small layer of its own
const Viewport::UvDraw& Viewport::uvHoverDraw() {
  const int am = topoMesh();
  QByteArray sig = QByteArray::number(uvHovRev_) + "|" + QByteArray::number(uvRev_) + "|" + QByteArray::number(am) + topoMode.toLatin1() + QByteArray::number(int(tset.uv));
  sig += "|s" + QByteArray::number(uvStamp_);
  if (sig == uvHovSig_) return uvHovDraw_;
  uvHovSig_ = sig; uvHovDraw_ = {}; UvDraw& D = uvHovDraw_;
  if (uvTiles) return D;
  if (room() != "topo" || !tset.uv || am < 0 || topoMode == "object" || drag_ == Drag::Uv) return D;
  const PolyMesh& B = *baseOf(*scene_->item(am)); const UvCache* c = uvCache(); if (!c || !uv::has(B)) return D; const topo::TTopo& T = *c->T;
  const char key = tKey();
  if (key == 'f' && uvHov_.f >= 0 && uvHov_.f < T.nf) { UvDraw::Tris t; t.op = 0.16f; int s = T.fo[size_t(uvHov_.f)];
    for (int k = s + 1; k + 1 < T.fo[size_t(uvHov_.f) + 1]; k++) { t.p.push_back(B.cornerUV[size_t(s)]); t.p.push_back(B.cornerUV[size_t(k)]); t.p.push_back(B.cornerUV[size_t(k + 1)]); t.c.push_back(Qt::white); }
    D.tris.push_back(std::move(t)); }
  if (key == 'e' && uvHov_.ek >= 0 && size_t(uvHov_.ek) < T.cf.size()) { int f = T.cf[size_t(uvHov_.ek)], k = uvHov_.ek, nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
    UvDraw::Lines h; h.c = Qt::white; h.w = 2.4f; h.p = {B.cornerUV[size_t(k)], B.cornerUV[size_t(nk)]}; D.lines.push_back(h); }
  if (key == 'v' && uvHov_.p >= 0 && uvHov_.p < c->P.count()) { UvDraw::Pts h; h.size = 9; h.p = {B.cornerUV[size_t(c->P.list[size_t(c->P.off[size_t(uvHov_.p)])])]}; h.c = {Qt::white}; D.pts.push_back(h); }
  return D;
}

QString Viewport::uvStatus() {
  int am = topoMesh(); if (am < 0) return tr("Select a mesh");
  const Item* it = scene_->item(am); const PolyMesh& B = *baseOf(*it);
  if (!uv::has(B)) return tr("No UV yet — mark seams (Edge mode), then Unfold");
  const UvCache* c = uvCache(); if (!c) return {};
  int fl = int(uv::flipped(B, *c->T, c->S).size()); double cov = 0; { std::vector<uv::Covered> cs; std::vector<int> ids = uvMeshes(); if (std::find(ids.begin(), ids.end(), am) == ids.end()) ids.push_back(am); for (int id : ids) if (const Item* oi = scene_->item(id); oi && oi->poly) { const PolyMesh& OB = *baseOf(*oi); if (!uv::has(OB)) continue; const auto hid = uvHidden(*oi, OB); cs.push_back({&OB, hid}); } cov = uv::tileCoverage(cs); }   /* (all the meshes in the tile — only what is shown: the set worked in) */
  return tr("%1 shell%2 · %3 seam%4 · the tile %5% filled%6").arg(c->S.count).arg(c->S.count == 1 ? "" : "s").arg(B.seams.size()).arg(B.seams.size() == 1 ? "" : "s")
         .arg(int(std::lround(cov * 100))).arg(fl ? tr(" · %1 turned over (red)").arg(fl) : QString());
}

// ---------- the edits ----------
// the line constraints follow the shells' turns: a shell turned by 90° — by the user (Turn, the rotate drag) or by Pack — has its horizontal ones made vertical
// and the other way. Only then (uvNormNext_, set by those), and a shell as a whole: when most of its constrained edges now run the other way. Never edge by
// edge after an Unfold or any other edit — an edge laid a little off its line flipped its constraint by itself (2026-11-10)
void Viewport::uvPinsNormalize(int m, const PolyMesh& B, const topo::TTopo& T) {
  Item* it = scene_->item(m); if (!it || B.cornerUV.size() != B.faceVerts.size()) return;
  QJsonArray arr = it->raw.value("uvPins").toArray(); if (arr.isEmpty()) return;
  const uv::Points P = uv::points(B, T); const uv::Shells S = uv::shells(B, T, P);
  std::vector<int> shellOf(size_t(arr.size()), -1); std::vector<char> off(size_t(arr.size()), 0); std::map<int, std::array<int, 2>> cnt;   // (shell → constrained, run the other way)
  for (int i = 0; i < arr.size(); i++) { const QJsonArray p = arr[i].toArray(); if (p.size() < 3 || p[0].toInt() == p[1].toInt()) continue;
    const QString t = p[2].toString(); if (t != "h" && t != "v") continue;
    const int a = p[0].toInt(), b = p[1].toInt(); const int e = T.edgeOf(a, b); if (e < 0) continue; const int f = p.size() >= 4 && p[3].toInt() >= 0 ? p[3].toInt() : (T.ef0[size_t(e)] >= 0 ? T.ef0[size_t(e)] : T.ef1[size_t(e)]); if (f < 0 || f >= T.nf) continue;
    const int k0 = uv::cornerAt(T, f, a), k1 = uv::cornerAt(T, f, b); if (k0 < 0 || k1 < 0) continue;
    const double du = std::abs(double(B.cornerUV[size_t(k0)][0]) - B.cornerUV[size_t(k1)][0]), dv = std::abs(double(B.cornerUV[size_t(k0)][1]) - B.cornerUV[size_t(k1)][1]);
    shellOf[size_t(i)] = S.of[size_t(f)]; off[size_t(i)] = (t == "h" && dv > du * 1.2 + 1e-7) || (t == "v" && du > dv * 1.2 + 1e-7); auto& c = cnt[S.of[size_t(f)]]; c[0]++; c[1] += off[size_t(i)]; }
  bool ch = false;
  for (int i = 0; i < arr.size(); i++) { const int s = shellOf[size_t(i)]; if (s < 0) continue; const auto& c = cnt[s]; if (c[1] * 2 <= c[0]) continue;   // (most of the shell's run the other way: it was turned)
    QJsonArray p = arr[i].toArray(); p[2] = p[2].toString() == "h" ? "v" : "h"; arr[i] = p; ch = true; }
  if (ch) it->raw.insert("uvPins", arr);
}
QString Viewport::uvEdit(const std::function<QString(PolyMesh&, const topo::TTopo&)>& fn) {
  const bool norm = uvNormNext_; uvNormNext_ = false;          // (taken once: an edit that stops early leaves no turn behind for the next one)
  int am = topoMesh(); if (am < 0) { ui::toast(tr("Select a mesh first")); return "!"; }
  
  uvAttrUpgrade(am);                                           // (older shell settings given their spread polygons first: a cut leaves them with the bigger part)
  Item* it = scene_->item(am); auto B = baseSp(*it); auto T = topo::ttopo(B); if (!T) return "!";
  auto nb = std::make_shared<PolyMesh>(*B);
  const int pts0 = [this] { const UvCache* c = uvCache(); return c ? c->P.count() : -1; }();   // (the UV points as numbered before the edit)
  QString msg = fn(*nb, *T);
  if (msg.startsWith('!')) { if (msg.size() > 1) ui::toast(msg.mid(1)); return msg; }
  if (nb->cornerUV.size() == nb->faceVerts.size()) uvSymApply(nb->cornerUV);   // (the UV symmetry: the mirrored shells follow)
  // (a set worked in: the UV work is done inside it only — a polygon of another set touched by the edit (picked by chance before, a mirror, the
  //  whole mesh) keeps its UV. Eye holes and the mouth of Head_UVSet shrank and moved while the Body set was worked in, 2026-11-10)
  if (const int ws = uvWorkSet(am, *B); ws >= 0 && topo::hasUvSets(*B) && nb->cornerUV.size() == B->cornerUV.size() && nb->faceCount() == B->faceCount())
    for (int f = 0; f < B->faceCount(); f++) if (!(size_t(f) < B->faceSets.size() && (B->faceSets[size_t(f)] >> ws & 1u))) for (int k = B->faceStart[size_t(f)]; k < B->faceStart[size_t(f) + 1]; k++) nb->cornerUV[size_t(k)] = B->cornerUV[size_t(k)];
  if (norm) uvPinsNormalize(am, *nb, *T);   // (only after a turn by the user or a Pack)
  topo::shareTopo(nb, B); topoWrite(am, nb); uvStamp_++; uvRev_++;
  scene_->revision++; commit(true);
  // (a sew, a cut, an unfold: the UV points are numbered anew — the picked ones named other points, or none at all past the end.
  // Their count changed: back to the points of the picked vertices)
  if (const UvCache* c = uvCache(); !c || c->P.count() != pts0) uvPtsVer_ = -1;
  emit topoState(); if (!msg.isEmpty()) ui::toast(msg);
  return msg;
}

namespace {
// the edges the selection means (Edge mode: them; polygons: their outline; vertices: edges between two of them)
std::vector<int> selEdges(const topo::TTopo& T, char key, const std::set<int>& f, const std::set<int>& e, const std::set<int>& v) {
  std::vector<int> o;
  if (key == 'e') o.assign(e.begin(), e.end());
  else if (key == 'f') { for (int x = 0; x < T.ne; x++) { bool a = f.count(T.ef0[size_t(x)]) > 0, b = T.ef1[size_t(x)] >= 0 && f.count(T.ef1[size_t(x)]) > 0; if (a != b) o.push_back(x); } }
  else for (int x = 0; x < T.ne; x++) if (v.count(T.ea[size_t(x)]) && v.count(T.eb[size_t(x)])) o.push_back(x);
  return o;
}
// the polygons the selection touches
std::vector<int> selFaces(const topo::TTopo& T, char key, const std::set<int>& f, const std::set<int>& e, const std::set<int>& v) {
  std::set<int> o;
  if (key == 'f') o = f;
  else if (key == 'e') { for (int x : e) if (x >= 0 && x < T.ne) { o.insert(T.ef0[size_t(x)]); if (T.ef1[size_t(x)] >= 0) o.insert(T.ef1[size_t(x)]); } }
  else for (int x : v) if (x >= 0 && x < T.n) for (int i = T.vfo[size_t(x)]; i < T.vfo[size_t(x) + 1]; i++) o.insert(T.vF[size_t(i)]);
  return {o.begin(), o.end()};
}
}  // namespace

QString Viewport::uvMarkSeams(bool on) {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvMarkSeams(on); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  const int am = topoMesh(); const char key = tKey();
  if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select edges (Edge mode) — or polygons: their outline")); return "!"; }
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; const bool sym = tset.sym;
  return uvEdit([&, key, sym](PolyMesh& m, const topo::TTopo& T) -> QString {
    auto es = selEdges(T, key, F, E, V); if (es.empty()) return tr("!Select edges (Edge mode) — or polygons: their outline");
    if (sym) { const auto me = uv::mirrorEdges(m, T); const size_t n0 = es.size(); for (size_t i = 0; i < n0; i++) if (me[size_t(es[i])] >= 0) es.push_back(me[size_t(es[i])]);   // (Symmetry: the mirror seams too)
      std::sort(es.begin(), es.end()); es.erase(std::unique(es.begin(), es.end()), es.end()); }
    auto fl = uv::seamFlags(m, T); int n = 0; for (int e : es) if (bool(fl[size_t(e)]) != on) { fl[size_t(e)] = on; n++; }
    if (!n) return on ? tr("!These edges are seams already") : tr("!None of these edges is a seam");
    uv::setSeams(m, T, fl);
    int parted = 0;
    if (on && uv::has(m)) {                                       // (a seam that cuts a UV shell in two: the smaller part moved off by a small gap — two shells, not one with a line in it)
      const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P);
      std::vector<int> comp(size_t(T.nf), -1); int nc = 0;          // (the pieces along the seams)
      for (int f0 = 0; f0 < T.nf; f0++) { if (comp[size_t(f0)] >= 0) continue; std::vector<int> st{f0}; comp[size_t(f0)] = nc;
        while (!st.empty()) { const int f = st.back(); st.pop_back(); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; if (T.ef1[size_t(e)] < 0 || fl[size_t(e)]) continue;
            const int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (g >= 0 && comp[size_t(g)] < 0) { comp[size_t(g)] = nc; st.push_back(g); } } }
        nc++; }
      std::map<int, std::map<int, std::vector<int>>> byShell;       // (a UV shell → its pieces → their polygons)
      for (int f = 0; f < T.nf; f++) byShell[S.of[size_t(f)]][comp[size_t(f)]].push_back(f);
      for (auto& [sh, pieces] : byShell) { if (pieces.size() < 2) continue;
        auto big = pieces.begin(); for (auto it = pieces.begin(); it != pieces.end(); ++it) if (it->second.size() > big->second.size()) big = it;
        const auto bb = uv::box(m, uv::faceCorners(T, big->second)); const double bx = (bb[0] + bb[2]) / 2, by = (bb[1] + bb[3]) / 2;
        for (auto& [c, fs] : pieces) { if (c == big->first) continue;
          const auto ks = uv::faceCorners(T, fs); const auto b = uv::box(m, ks); double dx = (b[0] + b[2]) / 2 - bx, dy = (b[1] + b[3]) / 2 - by; const double L = std::hypot(dx, dy);
          if (L < 1e-9) { dx = 1; dy = 0; } else { dx /= L; dy /= L; }
          const double gap = 0.03 * std::max(b[2] - b[0], b[3] - b[1]) + 0.004;
          uv::transform(m, ks, [&](const uv::UV& p) { return uv::UV{float(p[0] + dx * gap), float(p[1] + dy * gap)}; }); parted++; } } }
    return on ? (parted ? tr("%1 seam%2 marked — %3 shell%4 parted").arg(n).arg(n == 1 ? "" : "s").arg(parted).arg(parted == 1 ? "" : "s") : tr("%1 seam%2 marked").arg(n).arg(n == 1 ? "" : "s")) : tr("%1 seam%2 cleared").arg(n).arg(n == 1 ? "" : "s");
  });
}
// the seams between the two pieces of the given polygon counts taken off and the joined shell unfolded again (a clean-up of a piece a fold cut made)
QString Viewport::uvDropSeamsBetween(int nA, int nB) {
  const int am = topoMesh(); if (am < 0) return "!no mesh"; const uv::Pins pins = uvPinsOf(am); const uv::PackOpts po = uvPackOpts(am); const auto feats = uvFeaturesOf(am); const double axis = uvo.symAxis;
  return uvEdit([&, nA, nB, axis](PolyMesh& m, const topo::TTopo& T) -> QString {
    auto fl = uv::seamFlags(m, T); std::vector<int> c(size_t(T.nf), -1); int n = 0;
    for (int f0 = 0; f0 < T.nf; f0++) { if (c[size_t(f0)] >= 0) continue; std::vector<int> st{f0}; c[size_t(f0)] = n; while (!st.empty()) { const int f = st.back(); st.pop_back(); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; if (T.ef1[size_t(e)] < 0 || fl[size_t(e)]) continue; const int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (c[size_t(g)] < 0) { c[size_t(g)] = n; st.push_back(g); } } } n++; }
    std::vector<int> sz(size_t(n), 0); for (int x : c) sz[size_t(x)]++; int off = 0; std::vector<int> fs;
    for (int e = 0; e < T.ne; e++) { const int a = T.ef0[size_t(e)], b = T.ef1[size_t(e)]; if (b < 0 || !fl[size_t(e)] || c[size_t(a)] == c[size_t(b)]) continue; const int sa = sz[size_t(c[size_t(a)])], sb = sz[size_t(c[size_t(b)])]; if ((sa == nA && sb == nB) || (sa == nB && sb == nA)) { fl[size_t(e)] = 0; off++; fs.push_back(a); } }
    if (!off) return tr("!No seams between such pieces"); uv::setSeams(m, T, fl);
    uv::SymUnfold su; su.mc = uv::mirrorCorners(m, T); su.pairs = true; su.axis = axis; const auto r = uv::unfold(m, T, fs, true, true, &pins, &su, &po, &feats);
    return tr("%1 seams off, %2 shell unfolded").arg(off).arg(r.done); });
}

QString Viewport::uvSew() {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvSew(); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  const int am = topoMesh(); const char key = tKey();
  // (nothing picked: every tear sewn — the UV broken where no seam is, a part of a shell dragged off: set back onto its shell; the seams kept)
  if (am >= 0 && (topoMode == "object" || tsel_.id != am || (tsel_.f.empty() && tsel_.e.empty() && tsel_.v.empty()))) {
    std::vector<int> shown; if (const Item* it = scene_->item(am)) { const PolyMesh& B = *baseOf(*it); const auto h = uvHidden(*it, B); for (int f = 0; f < B.faceCount(); f++) if (h.empty() || size_t(f) >= h.size() || !h[size_t(f)]) shown.push_back(f); }
    return uvEdit([shown](PolyMesh& m, const topo::TTopo& T) -> QString { if (!uv::has(m)) return tr("!This mesh has no UV");
      const int n = uv::healTears(m, T, shown); return n ? tr("%1 torn-off piece%2 sewn back where no seam is — the seams kept").arg(n).arg(n == 1 ? "" : "s") : tr("!No tears — the UV breaks only along the seams. To sew a seam, select its edges (Edge mode)"); }); }
  if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select the edges to sew (Edge mode)")); return "!"; }
  const TSel opS = uvOpSel(); const std::set<int> F = opS.f, E = opS.e, V = opS.v; const uv::Pins pins = uvPinsOf(am); const uv::PackOpts po = uvPackOpts(am); const auto feats = uvFeaturesOf(am);
  return uvEdit([&, key](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV");
    auto es = selEdges(T, key, F, E, V); if (es.empty()) return tr("!Select the edges to sew (Edge mode)");
    auto fl = uv::seamFlags(m, T); int n = 0;
    auto cornerAt = [&](int f, int v) { for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) if (T.fv[size_t(k)] == v) return k; return -1; };
    {                                                            // (the shells apart: the smaller one brought to the bigger along the sewn edges — placed by the first edge (turned, scaled to its length, mirrored if it lies over),
                                                                 //  its points on the sewn edges set on the other side's, the rest of it relaxed between them — the bigger one is not touched)
      const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P); std::vector<int> cnt(size_t(S.count) + 1, 0); for (int f = 0; f < T.nf; f++) cnt[size_t(S.of[size_t(f)])]++;
      std::map<std::pair<int, int>, std::vector<int>> pairs; std::set<int> movedSh;      // (anchor shell, mover shell) → the edges between them
      for (int e : es) { const int f0 = T.ef0[size_t(e)], f1 = T.ef1[size_t(e)]; if (f1 < 0) continue; int sa = S.of[size_t(f0)], sb = S.of[size_t(f1)]; if (sa == sb) continue;
        if (cnt[size_t(sa)] < cnt[size_t(sb)]) std::swap(sa, sb); pairs[{sa, sb}].push_back(e); }
      for (auto& [key, ed] : pairs) {
        const int sm = key.second; if (movedSh.count(sm) || movedSh.count(key.first)) continue; movedSh.insert(sm);
        auto sides = [&](int e, int& fa, int& fm) { fa = T.ef0[size_t(e)]; fm = T.ef1[size_t(e)]; if (S.of[size_t(fm)] != sm) std::swap(fa, fm); };
        { int fa, fm; sides(ed[0], fa, fm); const int va = T.ea[size_t(ed[0])], vb = T.eb[size_t(ed[0])];
          const int a0 = cornerAt(fa, va), a1 = cornerAt(fa, vb), b0 = cornerAt(fm, va), b1 = cornerAt(fm, vb); if (a0 < 0 || a1 < 0 || b0 < 0 || b1 < 0) continue;
          const std::array<float, 2> ua = m.cornerUV[size_t(a0)], uaB = m.cornerUV[size_t(a1)], ub = m.cornerUV[size_t(b0)], ubB = m.cornerUV[size_t(b1)];
          const double ax = uaB[0] - ua[0], ay = uaB[1] - ua[1], bx = ubB[0] - ub[0], by = ubB[1] - ub[1], la = std::hypot(ax, ay), lb = std::hypot(bx, by); if (la < 1e-9 || lb < 1e-9) continue;
          const double ang = std::atan2(ay, ax) - std::atan2(by, bx), s = la / lb, c = std::cos(ang), sn = std::sin(ang);
          auto rot = [&](const std::array<float, 2>& p) { const double x = p[0] - ub[0], y = p[1] - ub[1]; return std::array<double, 2>{ua[0] + s * (c * x - sn * y), ua[1] + s * (sn * x + c * y)}; };
          auto side = [&](const std::array<double, 2>& p) { return ax * (p[1] - ua[1]) - ay * (p[0] - ua[0]); };
          auto centre = [&](int f, bool turned) { double x = 0, y = 0; int nn = 0; for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { std::array<double, 2> p = turned ? rot(m.cornerUV[size_t(k)]) : std::array<double, 2>{m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]}; x += p[0]; y += p[1]; nn++; } return std::array<double, 2>{x / nn, y / nn}; };
          const bool mirror = side(centre(fa, false)) * side(centre(fm, true)) > 0; const double dx = ax / la, dy = ay / la;
          std::vector<int> fsm; for (int f = 0; f < T.nf; f++) if (S.of[size_t(f)] == sm) fsm.push_back(f);
          uv::transform(m, uv::faceCorners(T, fsm), [&](const uv::UV& p) { auto q = rot(p);
            if (mirror) { const double vx = q[0] - ua[0], vy = q[1] - ua[1], pr = vx * dx + vy * dy; q = {ua[0] + 2 * pr * dx - vx, ua[1] + 2 * pr * dy - vy}; }
            return uv::UV{float(q[0]), float(q[1])}; });
          // the points of the mover: the sewn ones set on the other side's; the rest of its rim held; the inside relaxed
          std::map<int, std::array<double, 2>> tgt; std::map<int, int> tn; std::set<int> sewnE(ed.begin(), ed.end());
          for (int e : ed) { int fA, fM; sides(e, fA, fM); for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { const int kA = cornerAt(fA, v), kM = cornerAt(fM, v); if (kA < 0 || kM < 0) continue;
              auto& t = tgt[P.of[size_t(kM)]]; t[0] += m.cornerUV[size_t(kA)][0]; t[1] += m.cornerUV[size_t(kA)][1]; tn[P.of[size_t(kM)]]++; } }
          std::map<int, std::array<double, 2>> pos; std::set<int> fixedP; std::map<int, std::vector<int>> nbr;
          for (int f : fsm) { const int s0 = T.fo[size_t(f)], e0 = T.fo[size_t(f) + 1];
            for (int k = s0; k < e0; k++) { const int p = P.of[size_t(k)]; auto& u = m.cornerUV[size_t(k)]; if (!pos.count(p)) pos[p] = {u[0], u[1]};
              const int nk = k + 1 < e0 ? k + 1 : s0, pk = k > s0 ? k - 1 : e0 - 1; nbr[p].push_back(P.of[size_t(nk)]); nbr[p].push_back(P.of[size_t(pk)]);
              const int e2 = T.ce[size_t(k)]; if ((T.ef1[size_t(e2)] < 0 || fl[size_t(e2)]) && !sewnE.count(e2)) { fixedP.insert(p); fixedP.insert(P.of[size_t(nk)]); } } }
          for (auto& [p, t] : tgt) { pos[p] = {t[0] / tn[p], t[1] / tn[p]}; fixedP.insert(p); }
          for (int it = 0; it < 300; it++) for (auto& [p, q] : pos) { if (fixedP.count(p)) continue; double x = 0, y = 0; for (int o : nbr[p]) { x += pos[o][0]; y += pos[o][1]; } const double nn = double(nbr[p].size()); if (nn > 0) q = {x / nn, y / nn}; }
          for (int f : fsm) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const auto& q = pos[P.of[size_t(k)]]; m.cornerUV[size_t(k)] = {float(q[0]), float(q[1])}; }
        } }
    }
    for (int e : es) {
      const int f0 = T.ef0[size_t(e)], f1 = T.ef1[size_t(e)]; if (f1 < 0) continue; bool moved = false;
      for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { const int k0 = cornerAt(f0, v), k1 = cornerAt(f1, v); if (k0 < 0 || k1 < 0) continue;
        auto& a = m.cornerUV[size_t(k0)]; auto& b = m.cornerUV[size_t(k1)]; if (a == b) continue;
        const std::array<float, 2> c{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2}; a = c; b = c; moved = true; }
      if (fl[size_t(e)]) { fl[size_t(e)] = 0; moved = true; }
      if (moved) n++;
    }
    if (!n) return tr("!Nothing to sew — the UV is joined along these edges already");
    uv::setSeams(m, T, fl);
    return tr("%1 edge%2 sewn").arg(n).arg(n == 1 ? "" : "s");
  });
}
QString Viewport::uvSeamsByAngle(double deg) {
  return uvEdit([deg](PolyMesh& m, const topo::TTopo& T) -> QString {
    auto es = uv::seamsByAngle(m, T, deg); auto fl = uv::seamFlags(m, T); int n = 0; for (int e : es) if (!fl[size_t(e)]) { fl[size_t(e)] = 1; n++; }
    if (!n) return QObject::tr("!No edge is sharper than %1° (or they are seams already)").arg(deg);
    uv::setSeams(m, T, fl); return QObject::tr("%1 seam%2 where the polygons meet at more than %3°").arg(n).arg(n == 1 ? "" : "s").arg(deg);
  });
}
QString Viewport::uvSeamsFromBorders() {
  return uvEdit([](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return QObject::tr("!This mesh has no UV");
    auto P = uv::points(m, T); auto es = uv::borderEdges(m, T, P); std::vector<char> fl(size_t(T.ne), 0); for (int e : es) fl[size_t(e)] = 1;
    uv::setSeams(m, T, fl); return QObject::tr("Seams = the UV shells' borders: %1 edge%2").arg(es.size()).arg(es.size() == 1 ? "" : "s");
  });
}
// every seam of the set worked in cleared (the shells go back to the projection, the polygons stay in the set); no set lit — every seam of the mesh.
// A part in another set keeps its seams
QString Viewport::uvClearSeams() {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvClearSeams(); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  QString want; int wi = -1;
  if (uvPicked.id >= 0 && uvPicked.set >= 0 && uvPicked.mat == -2) { const auto S0 = uvSets(uvPicked.id); if (size_t(uvPicked.set) < S0.size()) want = S0[size_t(uvPicked.set)].name; wi = uvPicked.id; }
  else want = uvFreePicked;
  (void)wi;
  return uvEdit([want](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (m.seams.empty()) return QObject::tr("!No seams");
    int si = -1; if (!want.isEmpty() && topo::hasUvSets(m)) for (size_t s = 0; s < m.uvSetNames.size(); s++) if (m.uvSetNames[s] == want) { si = int(s); break; }
    if (want.isEmpty() || si < 0) { int n = int(m.seams.size()); m.seams.clear(); return QObject::tr("%1 seam%2 cleared").arg(n).arg(n == 1 ? "" : "s"); }
    auto fl = uv::seamFlags(m, T); int n = 0; auto inSet = [&](int f) { return f >= 0 && size_t(f) < m.faceSets.size() && (m.faceSets[size_t(f)] >> si & 1u); };
    for (int e = 0; e < T.ne; e++) if (fl[size_t(e)] && (inSet(T.ef0[size_t(e)]) || inSet(T.ef1[size_t(e)]))) { fl[size_t(e)] = 0; n++; }
    if (!n) return QObject::tr("!No seams in this set"); uv::setSeams(m, T, fl);
    return QObject::tr("%1 seam%2 of the set cleared — the rest (other sets) stay").arg(n).arg(n == 1 ? "" : "s"); });
}// Auto seams: the selection's closed shells (none selected — every shown one) — seams to open each into one piece
QString Viewport::uvAutoSeams() {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvAutoSeams(); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const TSel opS = uvOpSel(); const std::set<int> F = opS.f, E = opS.e, V = opS.v; const auto G = uvMatGroups(am);
  return uvEdit([&, key, comp](PolyMesh& m, const topo::TTopo& T) -> QString {
    std::vector<int> fs; if (comp) fs = selFaces(T, key, F, E, V); if (fs.empty()) for (auto& g : G) fs.insert(fs.end(), g.begin(), g.end());
    auto fl = uv::seamFlags(m, T); auto cut = [&](int e) { return T.ef1[size_t(e)] < 0 || fl[size_t(e)]; };
    std::vector<int> sh(size_t(T.nf), -1); int ns = 0; std::vector<char> want(size_t(T.nf), 0); for (int f : fs) if (f >= 0 && f < T.nf) want[size_t(f)] = 1;
    std::vector<int> closed; int nClosed = 0;
    for (int f0 = 0; f0 < T.nf; f0++) { if (!want[size_t(f0)] || sh[size_t(f0)] >= 0) continue;
      std::vector<int> st{f0}, cf; sh[size_t(f0)] = ns; bool open = false;
      while (!st.empty()) { const int f = st.back(); st.pop_back(); cf.push_back(f);
        for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; if (cut(e)) { open = true; continue; } const int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (g >= 0 && sh[size_t(g)] < 0) { sh[size_t(g)] = ns; st.push_back(g); } } }
      ns++; if (!open) { closed.insert(closed.end(), cf.begin(), cf.end()); nClosed++; } }
    if (closed.empty()) return tr("!No closed shell here — each has a border Unfold opens it by");
    int n = 0; for (int e : uv::autoSeams(m, T, closed)) if (!fl[size_t(e)]) { fl[size_t(e)] = 1; n++; }
    uv::setSeams(m, T, fl); return tr("Auto seams: %1 closed shell%2 opened — %3 edge%4 (at the back, below, on the sharp edges)").arg(nClosed).arg(nClosed == 1 ? "" : "s").arg(n).arg(n == 1 ? "" : "s");
  });
}
// the UV Mats' polygons regrouped by shell: a shell with polygons in two mats packed once — with the mat most of it is in
// (packed with each, its second place lands on the first mat's shells). n: how many such shells
static std::vector<std::vector<int>> groupsByShell(const PolyMesh& m, const topo::TTopo& T, const std::vector<std::vector<int>>& G, int* n = nullptr) {
  if (n) *n = 0; if (G.size() < 2) return G;
  const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P);
  std::vector<int> gOf(size_t(T.nf), -1); for (size_t g = 0; g < G.size(); g++) for (int f : G[g]) if (f < T.nf) gOf[size_t(f)] = int(g);
  std::vector<std::map<int, int>> cnt(size_t(S.count)); for (int f = 0; f < T.nf; f++) if (gOf[size_t(f)] >= 0) cnt[size_t(S.of[size_t(f)])][gOf[size_t(f)]]++;
  std::vector<int> best(size_t(S.count), -1);
  for (int s = 0; s < S.count; s++) { int b = -1, bc = 0; for (auto& [g, c] : cnt[size_t(s)]) if (c > bc) { b = g; bc = c; } best[size_t(s)] = b; if (n && cnt[size_t(s)].size() > 1) (*n)++; }
  std::vector<std::vector<int>> out(G.size()); for (int f = 0; f < T.nf; f++) if (gOf[size_t(f)] >= 0) out[size_t(best[size_t(S.of[size_t(f)])])].push_back(f);
  out.erase(std::remove_if(out.begin(), out.end(), [](const std::vector<int>& g) { return g.empty(); }), out.end()); return out;
}
static uv::Pins partHold(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& sel, const uv::Pins& base, std::vector<int>& full);
QString Viewport::uvUnfold(bool all, bool symPairs) {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvUnfold(all, symPairs); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const TSel opS = uvOpSel(); const std::set<int> F = opS.f, E = opS.e, V = opS.v; const auto G = uvMatGroups(am); const double pad = uvo.pad; const bool rot = uvo.rotate; const uv::Pins pins = uvPinsOf(am); const uv::PackOpts po = uvPackOpts(am); const auto feats = uvFeaturesOf(am);
  const bool symOn = uvo.symUnfold || symPairs, pairsOn = tset.sym || symPairs; const double axis = uvo.symAxis;
  return uvEdit([&, key, comp, all](PolyMesh& m, const topo::TTopo& T) -> QString {
    std::vector<int> fs; if (!all) { if (comp) fs = selFaces(T, key, F, E, V); if (fs.empty()) return tr("!Select polygons of the shells to unfold — or Unfold all"); }
    else for (auto& g : G) fs.insert(fs.end(), g.begin(), g.end());
    ui::BusyScope bz(tr("Unfolding"));
    const auto attrs = uvShellAttrs(am); uv::PackOpts po2 = po; po2.attr = &attrs;   // (each shell's own settings; Unfold all: the locked left)
    bool anyLock = false; for (int f : fs) if (size_t(f) < attrs.size() && attrs[size_t(f)].lock) anyLock = true;
    if (all) fs.erase(std::remove_if(fs.begin(), fs.end(), [&](int f) { return size_t(f) < attrs.size() && attrs[size_t(f)].lock; }), fs.end());
    if (all && fs.empty()) return tr("!Nothing to unfold here: no polygon of the UV Set worked in is shown, or every shell is locked");   // (an empty list means "every shell" to the unfolding: the whole mesh went, the locked and the other sets' too)
    const bool repack = all && (G.size() > 1 || anyLock);   // (the locked stay: the rest packed round them)
    // (a part of a shell picked: only it is laid out anew, and the change dies out softly — the three rows of polygons round the zone take part, the further the less; the rest of the shell stays exactly)
    const int SOFT = 3; std::vector<std::array<float, 2>> oldUV; std::vector<int> fdist; bool soft = false;
    if (!all && comp && uv::has(m)) { const auto sf = uv::seamFlags(m, T); fdist.assign(size_t(T.nf), -1); std::vector<int> q; for (int f : fs) if (f >= 0 && f < T.nf && fdist[size_t(f)] < 0) { fdist[size_t(f)] = 0; q.push_back(f); }
      for (size_t h = 0; h < q.size(); h++) { const int f = q[h]; if (fdist[size_t(f)] >= SOFT) continue;
        for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; if (T.ef1[size_t(e)] < 0 || sf[size_t(e)]) continue; const int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (g >= 0 && fdist[size_t(g)] < 0) { fdist[size_t(g)] = fdist[size_t(f)] + 1; q.push_back(g); } } }
      fs = q; oldUV = m.cornerUV; soft = true; }
    std::vector<int> fullShells; const uv::Pins held = !all && comp ? partHold(m, T, fs, pins, fullShells) : pins; if (!all && comp && !fullShells.empty()) fs = fullShells;   // (a part of a shell picked: only it is laid out anew, the rest of the shell held where it lies)
    uv::SymUnfold su; if (symOn) { su.mc = uv::mirrorCorners(m, T); su.pairs = pairsOn; su.axis = axis; }
    auto r = uv::unfold(m, T, fs, !all || repack, true, &held, symOn ? &su : nullptr, &po2, &feats);
    if (soft && r.done) { std::vector<int> vd(m.pos.size(), 1 << 20); for (int f = 0; f < T.nf; f++) if (fdist[size_t(f)] >= 0) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) vd[size_t(T.fv[size_t(k)])] = std::min(vd[size_t(T.fv[size_t(k)])], fdist[size_t(f)]);
      for (int f = 0; f < T.nf; f++) if (fdist[size_t(f)] >= 0) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const double w = std::clamp((SOFT - vd[size_t(T.fv[size_t(k)])] + 1.0) / (SOFT + 1.0), 0.0, 1.0); auto& q = m.cornerUV[size_t(k)]; const auto& o = oldUV[size_t(k)]; q = {float(o[0] + w * (q[0] - o[0])), float(o[1] + w * (q[1] - o[1]))}; } }
    const int autoN = 0;   // (closed shells: left as they are — seams are only ever put by the user (Mark seam, Auto seams); Unfold never cuts by itself)
    if (repack) for (auto& g : groupsByShell(m, T, G)) { uv::PackOpts o = po2; o.rotate = o.rotate && !r.pinned; o.freeTurn = o.freeTurn && !r.pinned; if (symOn && pairsOn) { o.mc = &su.mc; o.axis = axis; } if (!uv::packShapes(m, T, g, o).ok) uv::pack(m, T, g, pad, rot && !r.pinned); }   // (each UV Mat into the tile of its own — they overlap)
    if (!r.done) return r.closed ? tr("!Closed — no border to open it: mark a seam on it (Edge mode → Mark seam)") : tr("!Nothing to unfold");
    QString s = tr("%1 shell%2 unfolded").arg(r.done).arg(r.done == 1 ? "" : "s");
    if (r.mended) s += tr(" · %1 shell%2 folded over itself — unbent until nothing lies on itself").arg(r.mended).arg(r.mended == 1 ? "" : "s");
{ uv::CheckOpts co; co.w = po.w; co.h = po.h; co.pad = 0; const auto rep = uv::check(m, T, fs, co); const size_t nbad = rep.overlap.size() + rep.flipped.size(); if (nbad) s += tr(" · %1 polygon%2 lie over others or are turned over (pink / red) — a seam there, then Unfold again").arg(nbad).arg(nbad == 1 ? "" : "s"); }   // (an unfolded shell that folds over itself is told, not left unseen)
    if (autoN) s += tr(" · closed ones opened by Auto seams (%1 edge%2 — see them, move them, Unfold again)").arg(autoN).arg(autoN == 1 ? "" : "s");
    if (r.closed) s += tr(" · %1 closed shell%2 left as %3 — no border to open %4: mark a seam on %4 (Edge mode → Mark seam, or Auto seams)").arg(r.closed).arg(r.closed == 1 ? "" : "s").arg(r.closed == 1 ? tr("it was") : tr("they were")).arg(r.closed == 1 ? tr("it") : tr("them"));
    if (r.symmetric) s += tr(" · %1 symmetric").arg(r.symmetric) + (r.selfSym ? tr(" (%1 of them symmetric in itself, off the axis)").arg(r.selfSym) : QString());
    if (r.mirrored) s += tr(" · %1 mirrored from %2 other side").arg(r.mirrored).arg(r.mirrored == 1 ? tr("its") : tr("their"));
    if (r.asym) s += tr(" · %1 across the middle, seams not symmetric — unfolded as %2 %3").arg(r.asym).arg(r.asym == 1 ? tr("it") : tr("they")).arg(r.asym == 1 ? tr("is") : tr("are"));
    if (r.closed) s += tr(" · %1 closed one%2 left — mark a seam on %3").arg(r.closed).arg(r.closed == 1 ? "" : "s").arg(r.closed == 1 ? tr("it") : tr("them"));
    if (r.holes) s += tr(" · %1 with holes or handles — more seams make %2 flatter").arg(r.holes).arg(r.holes == 1 ? tr("it") : tr("them"));
    if (r.asymPair) s += tr(" · %1 mirror pair%2 cut differently on the two sides — each unfolded on its own, not mirrored: mark the same seams on both").arg(r.asymPair).arg(r.asymPair == 1 ? "" : "s");
    if (r.collapsed) s += tr(" · %1 ring%2 left as %3 — no seam across: mark one across it, then Unfold").arg(r.collapsed).arg(r.collapsed == 1 ? "" : "s").arg(r.collapsed == 1 ? tr("it was") : tr("they were"));
    if (r.blown) s += tr(" · %1 shell%2 came out far past the tile — left as %3; check its constraints and seams").arg(r.blown).arg(r.blown == 1 ? "" : "s").arg(r.blown == 1 ? tr("it was") : tr("they were"));
    return s;
  });
}
// Unfold / Optimize of a PART of a shell: the shells the polygons belong to are worked whole, but every point that touches a polygon NOT picked is held where
// it lies (a pinned point), so only the picked part is laid out anew. Nothing but whole shells picked: no hold. `full`: the polygons of those shells
static uv::Pins partHold(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& sel, const uv::Pins& base, std::vector<int>& full) {
  full = sel; if (sel.empty() || !uv::has(m)) return base;
  const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P); std::set<int> shells; for (int f : sel) if (f >= 0 && f < T.nf) shells.insert(S.of[size_t(f)]);
  std::vector<char> in(size_t(T.nf), 0); for (int f : sel) if (f >= 0 && f < T.nf) in[size_t(f)] = 1;
  uv::Pins held = base; bool part = false; full.clear();
  for (int f = 0; f < T.nf; f++) { if (!shells.count(S.of[size_t(f)])) continue; full.push_back(f);
    if (!in[size_t(f)]) { part = true; for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) held.insert({T.fv[size_t(k)], T.fv[size_t(k)]}); } }
  if (!part) { full = sel; return base; }
  return held;
}
// Optimize (as RizomUV's): the selection's shells (none picked — all) relaxed as they lie — less stretch; no seams cut,
// nothing unfolded anew or packed; pinned edges held
QString Viewport::uvOptimize() {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvOptimize(); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const TSel opS = uvOpSel(); const std::set<int> F = opS.f, E = opS.e, V = opS.v; const uv::Pins pins = uvPinsOf(am); const uv::PackOpts po = uvPackOpts(am); const auto feats = uvFeaturesOf(am); const auto attrs = uvShellAttrs(am);
  return uvEdit([&, key, comp](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    std::vector<int> fs; if (comp) fs = selFaces(T, key, F, E, V);
    if (fs.empty()) { bool any = false; for (auto& a : attrs) any = any || a.lock; if (any) { for (int f = 0; f < T.nf; f++) if (size_t(f) >= attrs.size() || !attrs[size_t(f)].lock) fs.push_back(f); if (fs.empty()) return tr("!Nothing to optimize: every shell is locked"); } }   // (all but the locked)
    ui::BusyScope bz(tr("Optimizing"));
    std::vector<int> full; uv::Pins held = comp && !fs.empty() ? partHold(m, T, fs, pins, full) : pins; if (comp && !full.empty()) fs = full;   // (a part of a shell picked: only it is relaxed, the rest of the shell held)
    // (vertices picked: only they move — every other point of their shells held, the polygons' other corners too. Before, the polygons round
    //  the picked points were relaxed whole, and points not picked — the strip's other rim — moved with them, 2026-11-10)
    if (comp && key == 'v' && !V.empty()) for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int v = T.fv[size_t(k)]; if (!V.count(v)) held.insert({v, v}); }
    const int n = uv::optimize(m, T, fs, &held, 60, &po, &feats); const int mendedO = n && held.empty() ? uv::mendSelfOverlap(m, T, fs, &po) : 0;   // (a shell that still folds over itself: unbent step by step)
    if (!n) return tr("!Nothing to optimize");
    return tr("%1 shell%2 optimized — less stretch").arg(n).arg(n == 1 ? "" : "s") + (mendedO ? tr(" · %1 folded shell%2 unbent").arg(mendedO).arg(mendedO == 1 ? "" : "s") : QString());
  });
}
// Symmetrize (Maya's): the selected side's UV put on the other — the side the selection mostly lies on in 3D (the mesh's
// X); a pair's other shell mirrored across U = symAxis, a shell across the plane across its own middle
QString Viewport::uvSymmetrize() {
  const int am = topoMesh(); const auto ks = uvMoving();
  if (am < 0 || ks.empty()) { ui::toast(tr("Select the side to copy — its polygons, edges or UV points (a shell, a half of one)")); return "!"; }
  const double axis = uvo.symAxis;
  return uvEdit([&, axis](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    const double eps = std::max(1e-9, m.bounds().size().length() * 1e-4);
    double sx = 0; for (int k : ks) sx += m.pos[size_t(T.fv[size_t(k)])].x;
    const double side = sx < -eps * double(ks.size()) ? -1 : 1; std::vector<int> from;   // (both sides alike — a whole shell across the middle: from +X)
    for (int k : ks) { const double x = m.pos[size_t(T.fv[size_t(k)])].x * side; if (x > -eps) from.push_back(k); }
    const auto mc = uv::mirrorCorners(m, T);
    int none = 0; for (int k : from) if (mc[size_t(k)] < 0) none++;
    const int n = uv::symmetrize(m, T, mc, from, axis);
    if (!n) return tr("!No mirror found for the selection — is the mesh symmetric across its X = 0?");
    QString s = tr("Symmetrized: %1 UV corner%2 from the %3 side").arg(n).arg(n == 1 ? "" : "s").arg(side > 0 ? tr("+X") : tr("−X"));
    if (none) s += tr(" · %1 without a mirror").arg(none);
    return s;
  });
}
// Pack of a PART (the picked polygons' shells): the rest of what is worked on (`room`) stays where it lies, locked — in the way — and the picked ones go
// into the free places at the texel density the rest has (not grown to fill the holes); before, the rest was not in the way at all and the picked shells
// were laid over it (2026-11-10). Adds the rest to `sub`, locks it in `attrs`, sets o.keepDensity
static void packAmongRest(const PolyMesh& m, const topo::TTopo& T, std::vector<int>& sub, const std::vector<char>& room, std::vector<uv::ShellAttr>& attrs, uv::PackOpts& o) {
  const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P);
  std::set<int> picked; for (int f : sub) if (f >= 0 && f < T.nf) picked.insert(S.of[size_t(f)]);
  attrs.resize(size_t(T.nf)); const double M = std::max(o.w, o.h), Dw = o.w / M, Dh = o.h / M; double au = 0, a3 = 0;
  for (int f = 0; f < T.nf; f++) { if (size_t(f) >= room.size() || !room[size_t(f)] || picked.count(S.of[size_t(f)])) continue;
    sub.push_back(f); attrs[size_t(f)].lock = true; au += std::abs(uv::faceArea(m, f)) * Dw * Dh; a3 += uv::faceArea3(m, f); }
  // (Sizes — Keep the shells' size ratio: the picked shells stay as large as they lie — what was scaled by hand against the rest is kept; before, they
  //  were brought to the rest's texel density in either mode, 2026-11-12)
  if (au > 0 && a3 > 0) o.keepDensity = o.equalize ? au / a3 : -1;
}
// Pack of several meshes at once: every shell of the selected meshes laid into ONE tile together (as the UV of one texture set), by the texture of the active
// one — each mesh then gets back its own UV. The meshes are joined for the pack only: their points, polygons, seams and shell settings side by side
QString Viewport::uvPackMulti(bool all) {
  ThinkBox think(uvo.packFinal, toGlobal(QPointF(paneW() / 2.0, paneH() / 2.0)));
  std::vector<int> ids; for (int id : tselTargets()) { const Item* it = scene_->item(id); if (it && uv::has(*baseOf(*it))) ids.push_back(id); }
  if (ids.size() < 2) { ui::toast(tr("Pack all: select two or more meshes that have UV")); return "!"; }
  const int am = topoMesh(); const uv::PackOpts po0 = uvPackOpts(am); uv::PackOpts o = po0;
  auto J = std::make_shared<PolyMesh>(); std::vector<int> faceOff, cornerOff, vertOff; std::vector<uv::ShellAttr> attrs; int gOff = 0, sOff = 0;   // (each mesh's groups and stacks numbered apart: group 1 of one mesh is not group 1 of another)
  for (int id : ids) { const PolyMesh& B = *baseOf(*scene_->item(id)); const int v0 = int(J->pos.size()), c0 = int(J->faceVerts.size()), f0 = J->faceCount();
    vertOff.push_back(v0); cornerOff.push_back(c0); faceOff.push_back(f0);
    J->pos.insert(J->pos.end(), B.pos.begin(), B.pos.end());
    if (J->faceStart.empty()) J->faceStart.push_back(0);
    for (int f = 0; f < B.faceCount(); f++) J->faceStart.push_back(c0 + B.faceStart[size_t(f) + 1]);
    for (int v : B.faceVerts) J->faceVerts.push_back(v0 + v);
    J->cornerUV.insert(J->cornerUV.end(), B.cornerUV.begin(), B.cornerUV.end());
    for (auto& s : B.seams) J->seams.push_back({v0 + s[0], v0 + s[1]});
    auto a = uvShellAttrs(id); a.resize(size_t(B.faceCount())); int mg = 0, ms = 0; for (auto& x : a) { mg = std::max(mg, x.group); ms = std::max(ms, x.stack); if (x.group) x.group += gOff; if (x.stack) x.stack += sOff; } gOff += mg; sOff += ms;
    attrs.insert(attrs.end(), a.begin(), a.end()); }   // (one mesh's group 1 and another's were one group: their shells packed as one block — the tile 25% full, 2026-11-11)
  auto TJ = topo::ttopo(J); if (!TJ) return "!"; o.margin = uvo.pad;   /* (Margin: into the dashed frame) */
  o.attr = &attrs; std::vector<int> allF(size_t(J->faceCount())); std::iota(allF.begin(), allF.end(), 0);
  ui::BusyScope bz(tr("Packing"));
  std::vector<int> sub;                                         // (the shells to pack: a mesh with polygons picked — theirs; the others — every shell)
  // (only what the editor shows and works on — the UV Set worked in, by its name in each mesh: the head's own set left where it is. Packed with it, the
  //  hidden face shells took most of the tile — the body's set filled 16% of it, 2026-11-10)
  std::vector<char> inWork(size_t(J->faceCount()), 0);
  for (size_t i = 0; i < ids.size(); i++) for (const auto& g : uvMatGroups(ids[i])) for (int f : g) if (size_t(faceOff[i] + f) < inWork.size()) inWork[size_t(faceOff[i] + f)] = 1;
  if (all) { for (int f : allF) if (inWork[size_t(f)]) sub.push_back(f); }
  else { const std::map<int, TSel> picks = [&] { auto p = tselX_; if (tsel_.id >= 0) p[tsel_.id] = tsel_; return p; }(); const char key = tKey();
    bool anyPicked = false; for (int id : ids) if (const auto f = picks.find(id); f != picks.end() && (!f->second.f.empty() || !f->second.e.empty() || !f->second.v.empty())) anyPicked = true;
    for (size_t i = 0; i < ids.size(); i++) { const int id = ids[i]; const Item* it = scene_->item(id); const auto Tm = topo::ttopo(baseSp(*it)); const int nf = baseOf(*it)->faceCount(); std::vector<int> pf;
      if (const auto f = picks.find(id); f != picks.end() && Tm && (!f->second.f.empty() || !f->second.e.empty() || !f->second.v.empty())) pf = selFaces(*Tm, key, f->second.f, f->second.e, f->second.v);
      if (pf.empty() && !anyPicked) for (int f = 0; f < nf; f++) pf.push_back(f);   // (polygons picked anywhere: only their shells go, the meshes with none stay where they are)
      for (int f : pf) if (inWork[size_t(faceOff[i] + f)]) sub.push_back(faceOff[i] + f); } }
  if (sub.empty()) { ui::toast(tr("Nothing to pack here — no shell of the set worked in")); return "!"; }   // (none: packShapes would take every shell)
  if (!all && sub.size() < size_t(std::count(inWork.begin(), inWork.end(), char(1)))) packAmongRest(*J, *TJ, sub, inWork, attrs, o);   // (a part picked: into the free places, the rest in the way)
  std::vector<int> mcJ; if (tset.sym || std::any_of(attrs.begin(), attrs.end(), [](const uv::ShellAttr& a) { return a.sym; })) { mcJ = uv::mirrorCorners(*J, *TJ); o.mc = &mcJ; o.axis = uvo.symAxis; o.symAll = tset.sym; }   // (Symmetry on, or a shell tagged: the mirror shells — across the meshes too — packed mirrored)
  const uv::PackResult r = uv::packShapes(*J, *TJ, sub, o);
  if (!r.ok) { const bool lockedOnly = !sub.empty() && std::all_of(sub.begin(), sub.end(), [&](int f) { return size_t(f) < attrs.size() && attrs[size_t(f)].lock; }); ui::toast(lockedOnly ? tr("The selected shells are locked — unlock them to pack") : tr("Nothing to pack")); return "!"; }
  // (each mesh gets its corners back; the others are active in turn for the one undo step each)
  const std::vector<int> order = sel_.items; int done = 0;
  eachBusy_ = true; const int h0 = histIdx_;
  for (size_t i = 0; i < ids.size(); i++) { const int id = ids[i]; sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), id), sel_.items.end()); sel_.items.push_back(id);
    const int c0 = cornerOff[i]; const int n = int(baseOf(*scene_->item(id))->faceVerts.size());
    uvNormNext_ = true; uvEdit([&](PolyMesh& m, const topo::TTopo&) -> QString { if (int(m.cornerUV.size()) != n) return "!"; for (int k = 0; k < n; k++) m.cornerUV[size_t(k)] = J->cornerUV[size_t(c0 + k)]; return QString(); }); done++; }
  eachBusy_ = false; histMerge(h0); sel_.items = order; onSelChange();
  const QString msg = tr("%1 meshes packed into one tile: %2% filled · %3 × %4").arg(done).arg(int(std::lround(r.fill * 100))).arg(po0.w).arg(po0.h);
  ui::toast(msg); return msg;
}
QString Viewport::uvPack(bool all) {
  ThinkBox think(uvo.packFinal && !(!eachBusy_ && (all ? uvMulti() : (tselTargets().size() > 1 || uvMulti()))), toGlobal(QPointF(paneW() / 2.0, paneH() / 2.0)));
  if (!eachBusy_ && (all ? uvMulti() : (tselTargets().size() > 1 || uvMulti()))) return uvPackMulti(all);   // (several meshes: their shells into one tile — the picked polygons' shells of a mesh with some, every shell of a picked object with none)
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const TSel opS = uvOpSel(); const std::set<int> F = opS.f, E = opS.e, V = opS.v; const auto G = uvMatGroups(am);
  const uv::PackOpts po = uvPackOpts(am); const bool sym = tset.sym; const double axis = uvo.symAxis;
  uvNormNext_ = true;                                          // (a Pack may turn shells: the constraints turn with them)
  return uvEdit([&, key, comp, all, sym, axis](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    bool allE = all; std::vector<int> fs; if (!all) { if (comp) fs = selFaces(T, key, F, E, V); if (fs.empty()) allE = true; }   // (nothing selected: every shell, as Pack all)
    ui::BusyScope bz(tr("Packing"));
    // (by shape into the texture the set is laid out for; Symmetry on: across the plane on the axis, pairs mirrored)
    const auto attrs = uvShellAttrs(am); const bool tagSym = std::any_of(attrs.begin(), attrs.end(), [](const uv::ShellAttr& a) { return a.sym; });   // (the tag "Pack symmetrically")
    std::vector<int> mc; uv::PackOpts o = po; o.margin = uvo.pad; if (sym || tagSym) { mc = uv::mirrorCorners(m, T); o.mc = &mc; o.axis = axis; o.symAll = sym; }
    auto attrs2 = attrs; o.attr = &attrs2;   // (each shell's own: locked, not turned, its scale, its group)
    int straddle = 0; std::vector<std::vector<int>> groups; if (allE) groups = groupsByShell(m, T, G, &straddle); else groups = {fs};
    if (!allE) { std::vector<char> room(size_t(T.nf), 0), inFs(size_t(T.nf), 0); for (int f : fs) if (f >= 0 && f < T.nf) inFs[size_t(f)] = 1;   // (a part picked: into the free places of its UV Mat / set, the rest there in the way)
      for (const auto& g : G) { bool hit = false; for (int f : g) if (size_t(f) < inFs.size() && inFs[size_t(f)]) { hit = true; break; } if (hit) for (int f : g) if (f >= 0 && f < T.nf) room[size_t(f)] = 1; }
      packAmongRest(m, T, groups[0], room, attrs2, o); }
    double fill = 0; uv::PackResult tot; int n = 0;
    // (the UV Mats at once — a thread each, on a copy of the mesh: no mat reads another's UV half moved; their corners back)
    std::vector<uv::PackResult> rs(groups.size()); std::vector<std::unique_ptr<PolyMesh>> cp(groups.size());
    { std::vector<std::thread> th; for (size_t i = 0; i < groups.size(); i++) { if (groups.size() > 1) cp[i] = std::make_unique<PolyMesh>(m);
        PolyMesh* w = groups.size() > 1 ? cp[i].get() : &m; th.emplace_back([&, i, w] { rs[i] = uv::packShapes(*w, T, groups[i], o); }); }
      for (auto& t : th) t.join(); }
    for (size_t i = 0; i < groups.size(); i++) if (cp[i]) for (int f : groups[i]) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) m.cornerUV[size_t(k)] = cp[i]->cornerUV[size_t(k)];
    for (const auto& r : rs) { if (!r.ok) continue; n++; fill += r.fill; tot.centred += r.centred; tot.offAxis += r.offAxis; tot.pairs += r.pairs; tot.split += r.split; tot.shells += r.shells; tot.locked += r.locked; tot.stacked += r.stacked; }
    if (!n) return tr("!Nothing to pack");
    QString s = groups.size() > 1 ? tr("%1 UV Mats packed, each into its tile: %2% filled on average").arg(n).arg(int(std::lround(fill / n * 100)))
                                  : tr("Packed into the tile: %1% filled").arg(int(std::lround(fill * 100)));
    s += tr(" · %1 × %2, padding %3 px").arg(po.w).arg(po.h).arg(po.pad);
    { double au = 0, a3 = 0; for (const auto& g : groups) for (int f : g) { au += std::abs(uv::faceArea(m, f)); a3 += uv::faceArea3(m, f); }   // (the real texel density the pack came to, against the wanted one)
      if (au > 0 && a3 > 0) { const double real = std::sqrt(au * po.w * po.h / a3), want = uvTex(am).td; s += want > 0 ? tr(" · density %1 px/cm (wanted %2, %3%)").arg(real, 0, 'f', 1).arg(want, 0, 'f', 1).arg(int(std::lround(real / want * 100))) : tr(" · density %1 px/cm").arg(real, 0, 'f', 1); } }
    if (straddle) s += tr(" · %1 shell%2 in two UV Mats — packed with the mat most of it is in").arg(straddle).arg(straddle == 1 ? "" : "s");
    if (tot.locked) s += tr(" · %1 locked left in place").arg(tot.locked); if (tot.stacked) s += tr(" · %1 stacked kept on their twins").arg(tot.stacked);
    if (sym || tagSym) { s += tr(" · symmetric: %1 on the axis, %2 pair%3 mirrored").arg(tot.centred - tot.offAxis).arg(tot.pairs - tot.split).arg(tot.pairs - tot.split == 1 ? "" : "s");
      if (tot.offAxis) s += tr(" · %1 across the middle with no room on the axis — upright beside it").arg(tot.offAxis);
      if (tot.split) s += tr(" · %1 pair%2 too wide for a half — laid apart").arg(tot.split).arg(tot.split == 1 ? "" : "s"); }
    return s;
  });
}
namespace {
QString needSel() { return QObject::tr("!Select what to turn in the UV Editor (polygons, edges or UV points)"); }
}
QString Viewport::uvTurn(int q) {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvTurn(q); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(needSel().mid(1)); return "!"; }
  const double ka = cam_.uvAsp;                                // (a quarter turn as texels turn: V in the texture's proportion)
  uvNormNext_ = true;                                          // (the constraints turn with it)
  return uvEdit([ks, q, ka](PolyMesh& m, const topo::TTopo&) -> QString {
    auto b = uv::box(m, ks); double cu = (b[0] + b[2]) / 2, cv = (b[1] + b[3]) / 2;
    uv::transform(m, ks, [&](const uv::UV& p) { double x = p[0] - cu, y = (p[1] - cv) * ka; return q > 0 ? uv::UV{float(cu - y), float(cv + x / ka)} : uv::UV{float(cu + y), float(cv - x / ka)}; });
    return QObject::tr("Turned by 90° %1").arg(q > 0 ? QObject::tr("anticlockwise") : QObject::tr("clockwise"));
  });
}
QString Viewport::uvFlip(bool u) {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvFlip(u); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(needSel().mid(1)); return "!"; }
  return uvEdit([ks, u](PolyMesh& m, const topo::TTopo&) -> QString {
    auto b = uv::box(m, ks); double cu = (b[0] + b[2]) / 2, cv = (b[1] + b[3]) / 2;
    uv::transform(m, ks, [&](const uv::UV& p) { return u ? uv::UV{float(2 * cu - p[0]), p[1]} : uv::UV{p[0], float(2 * cv - p[1])}; });
    return u ? QObject::tr("Mirrored in U") : QObject::tr("Mirrored in V");
  });
}
QString Viewport::uvStraighten(bool alongU) {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvStraighten(alongU); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  auto ks = uvMoving(); if (ks.size() < 2) { ui::toast(tr("Select the edges or UV points to line up")); return "!"; }
  return uvEdit([ks, alongU](PolyMesh& m, const topo::TTopo&) -> QString {
    double s = 0; for (int k : ks) s += m.cornerUV[size_t(k)][alongU ? 1 : 0]; const float c = float(s / ks.size());
    uv::transform(m, ks, [&](const uv::UV& p) { return alongU ? uv::UV{p[0], c} : uv::UV{c, p[1]}; });
    return alongU ? QObject::tr("Lined up along U (one V)") : QObject::tr("Lined up along V (one U)");
  });
}
QString Viewport::uvSelectShells() {
  const int am = topoMesh(); if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons, edges or vertices of the shells")); return "!"; }
  const UvCache* c = uvCache(); if (!c) return "!"; const topo::TTopo& T = *c->T;
  auto fs = selFaces(T, tKey(), tsel_.f, tsel_.e, tsel_.v); if (fs.empty()) { ui::toast(tr("Select polygons, edges or vertices of the shells")); return "!"; }
  std::set<int> sh; for (int f : fs) sh.insert(c->S.of[size_t(f)]);
  std::vector<int> all; for (int f = 0; f < T.nf; f++) if (sh.count(c->S.of[size_t(f)])) all.push_back(f);
  const bool wasP = uvPicking_; uvPicking_ = true; int n = tSelectFaces(am, all); uvPicking_ = wasP; ui::toast(tr("%1 shell%2 selected (%3)").arg(sh.size()).arg(sh.size() == 1 ? "" : "s").arg(n));
  return {};
}


// hair strands: the selection's shells (all — every shown one) unbent
QString Viewport::uvStraightenStrands(bool all) {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvStraightenStrands(all); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const TSel opS = uvOpSel(); const std::set<int> F = opS.f, E = opS.e, V = opS.v; const double amt = uvo.strAmt; const bool up = uvo.strUp, spread = uvo.strSpread; const double pad = uvo.pad;
  std::vector<int> shown; if (all && am >= 0) for (auto& g : uvMatGroups(am)) shown.insert(shown.end(), g.begin(), g.end());
  return uvEdit([&, key, comp, all, amt, up, spread, pad](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    std::vector<int> fs = all ? shown : comp ? selFaces(T, key, F, E, V) : std::vector<int>{};
    if (fs.empty()) return all ? tr("!Nothing shown to straighten") : tr("!Select polygons of the strands (their shells)");
    ui::BusyScope bz(tr("Straightening"));
    // (each shell: its middle and its area before, put back after: straightened where it was, as large as it was)
    const uv::Points P0 = uv::points(m, T); const uv::Shells S0 = uv::shells(m, T, P0); std::set<int> mine; for (int f : fs) mine.insert(S0.of[size_t(f)]);
    struct Was { double cx = 0, cy = 0, area = 0; }; std::map<int, Was> was;
    auto measure = [&](std::map<int, Was>& out) { std::map<int, std::array<double, 4>> bx; std::map<int, double> ar;
      for (int f = 0; f < T.nf; f++) { const int sh = S0.of[size_t(f)]; if (!mine.count(sh)) continue; ar[sh] += std::abs(uv::faceArea(m, f));
        auto& q4 = bx.emplace(sh, std::array<double, 4>{1e300, 1e300, -1e300, -1e300}).first->second;
        for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const auto& q = m.cornerUV[size_t(k)]; q4[0] = std::min(q4[0], double(q[0])); q4[1] = std::min(q4[1], double(q[1])); q4[2] = std::max(q4[2], double(q[0])); q4[3] = std::max(q4[3], double(q[1])); } }
      for (auto& [sh, q4] : bx) out[sh] = {(q4[0] + q4[2]) / 2, (q4[1] + q4[3]) / 2, ar[sh]}; };
    measure(was);
    // (what each shell was: its corners and how much of it was turned over / lay over itself — a shell Straighten would make worse is put back)
    std::map<int, std::vector<int>> shFaces; for (int f = 0; f < T.nf; f++) if (mine.count(S0.of[size_t(f)])) shFaces[S0.of[size_t(f)]].push_back(f);
    uv::CheckOpts cko; { const uv::PackOpts pk = uvPackOpts(topoMesh()); cko.w = pk.w; cko.h = pk.h; cko.pad = 0; }
    auto badOf = [&](const std::vector<int>& fl) { const auto rp = uv::check(m, T, fl, cko); return rp.overlap.size() + rp.flipped.size(); };
    std::map<int, size_t> bad0; std::map<int, std::vector<std::array<float, 2>>> saved;
    for (auto& [sh, fl] : shFaces) { bad0[sh] = badOf(fl); auto& sv = saved[sh]; for (int f : fl) for (int c = T.fo[size_t(f)]; c < T.fo[size_t(f) + 1]; c++) sv.push_back(m.cornerUV[size_t(c)]); }
    const int n = uv::straighten(m, T, fs, amt, up); if (!n) return tr("!Nothing to straighten");
    { std::map<int, Was> now; measure(now);
      for (auto& [sh, w1] : now) { const Was& w0 = was[sh]; const double k = w1.area > 1e-12 && w0.area > 1e-12 ? std::sqrt(w0.area / w1.area) : 1.0;
        for (int f = 0; f < T.nf; f++) if (S0.of[size_t(f)] == sh) for (int c = T.fo[size_t(f)]; c < T.fo[size_t(f) + 1]; c++) { auto& q = m.cornerUV[size_t(c)]; q = {float(w0.cx + (q[0] - w1.cx) * k), float(w0.cy + (q[1] - w1.cy) * k)}; } } }
    int skipped = 0;                                          // (a shell that Straighten made worse — its top folded in, polygons turned over: back as it was)
    for (auto& [sh, fl] : shFaces) { if (badOf(fl) <= bad0[sh]) continue; size_t i = 0; for (int f : fl) for (int c = T.fo[size_t(f)]; c < T.fo[size_t(f) + 1]; c++) m.cornerUV[size_t(c)] = saved[sh][i++]; skipped++; }
    if (skipped >= int(shFaces.size())) return tr("!Straighten would fold this strand over itself — not applied (a seam across its bend, then again)");
    if (spread) uv::pack(m, T, fs, pad, false);              // (laid apart in the tile, as they stand: none over another. Off by default: a shell is not moved)
    return tr("%1 strand%2 straightened by %3%").arg(n).arg(n == 1 ? "" : "s").arg(int(std::lround(amt * 100)));
  });
}

// hair strands as strips: the selection's shells (all — every shown one)
QString Viewport::uvRectangleStrands(bool all) {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvRectangleStrands(all); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const TSel opS = uvOpSel(); const std::set<int> F = opS.f, E = opS.e, V = opS.v; const bool spread = uvo.strSpread; const double pad = uvo.pad; const int rmode = uvo.rectMode;
  std::vector<int> shown; if (all && am >= 0) for (auto& g : uvMatGroups(am)) shown.insert(shown.end(), g.begin(), g.end());
  return uvEdit([&, key, comp, all, spread, pad](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    std::vector<int> fs = all ? shown : comp ? selFaces(T, key, F, E, V) : std::vector<int>{};
    if (fs.empty()) return all ? tr("!Nothing shown to lay out") : tr("!Select polygons of the strands (their shells)");
    ui::BusyScope bz(tr("Laying out strips"));
    const int n = uv::rectangle(m, T, fs, rmode); if (!n) return tr("!Nothing laid out — a shell with no UV yet (or one squeezed to a point): Unfold it first");
    if (spread) uv::pack(m, T, fs, pad, false);
    return (rmode == uv::RectGrid ? tr("%1 strand%2 laid out as even grids") : tr("%1 strand%2 laid out as strips — the inside relaxed")).arg(n).arg(n == 1 ? "" : "s");
  });
}

// ---------- the transform: the selection's box typed in; the tile's edges ----------
std::optional<std::array<double, 4>> Viewport::uvSelBox() {
  const int m = topoMesh(); if (m < 0) return std::nullopt; const Item* it = scene_->item(m); if (!it || !uv::has(*baseOf(*it))) return std::nullopt;
  auto ks = uvMoving(); std::optional<std::array<double, 4>> b; if (!ks.empty()) b = uv::box(*baseOf(*it), ks);
  // (the other picked meshes' picks too: the gizmo's frame round all of it — it went round the active mesh's only, and a scale by its handles left the rest
  //  where they were, 2026-11-10)
  for (const auto& [id, s] : tselX_) { if (!sel_.hasItem(id) || id == m) continue; const Item* oi = scene_->item(id); if (!oi || !uv::has(*baseOf(*oi))) continue; const auto cs = uvMovingOf(id, s); if (cs.empty()) continue;
    const auto o = uv::box(*baseOf(*oi), cs); if (!b) b = o; else b = std::array<double, 4>{std::min((*b)[0], o[0]), std::min((*b)[1], o[1]), std::max((*b)[2], o[2]), std::max((*b)[3], o[3])}; }
  return b;
}
// one map of the UV put on every picked mesh's pick (the active one's and the others' — each its own undo step, as Pack all does); false: only the active one is picked
bool Viewport::uvMapAllPicked(const std::function<uv::UV(const uv::UV&)>& fn) {
  const int am = topoMesh(); std::vector<std::pair<int, std::vector<int>>> jobs; { auto ks = uvMoving(); if (!ks.empty()) jobs.push_back({am, std::move(ks)}); }
  for (const auto& [id, s] : tselX_) { if (!sel_.hasItem(id) || id == am) continue; auto cs = uvMovingOf(id, s); if (!cs.empty()) jobs.push_back({id, std::move(cs)}); }
  if (jobs.size() < 2) return false;
  const std::vector<int> order = sel_.items; eachBusy_ = true; const int h0 = histIdx_;
  for (auto& [id, cs] : jobs) { sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), id), sel_.items.end()); sel_.items.push_back(id);
    const auto c2 = cs; uvEdit([&fn, c2](PolyMesh& m, const topo::TTopo&) -> QString { uv::transform(m, c2, fn); return QString(); }); }
  eachBusy_ = false; histMerge(h0); sel_.items = order; onSelChange(); return true;
}
QString Viewport::uvSetBox(double u, double v, double w, double h) {
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(tr("Select what to place in the UV Editor (polygons, edges or UV points)")); return "!"; }
  const double axf = (uvo.anch % 3) * 0.5, ayf = (uvo.anch / 3) * 0.5;   // (the anchor: 0 — the box's left / bottom, 0.5 — its middle, 1 — its right / top)
  if (const auto B = uvSelBox()) { const auto b = *B; const double bw = b[2] - b[0], bh = b[3] - b[1], nu0 = u - axf * w, nv0 = v - ayf * h, ku = bw > 1e-12 ? w / bw : 1, kv = bh > 1e-12 ? h / bh : 1;   // (several meshes picked: one frame round all of it)
    if (uvMapAllPicked([=](const uv::UV& p) { return uv::UV{float(nu0 + (p[0] - b[0]) * ku), float(nv0 + (p[1] - b[1]) * kv)}; })) return QString(); }
  return uvEdit([ks, u, v, w, h, axf, ayf](PolyMesh& m, const topo::TTopo&) -> QString {
    auto b = uv::box(m, ks); const double bw = b[2] - b[0], bh = b[3] - b[1];
    const double nu0 = u - axf * w, nv0 = v - ayf * h;
    const double ku = bw > 1e-12 ? w / bw : 1, kv = bh > 1e-12 ? h / bh : 1;   // (a line: its width stays)
    uv::transform(m, ks, [&](const uv::UV& p) { return uv::UV{float(nu0 + (p[0] - b[0]) * ku), float(nv0 + (p[1] - b[1]) * kv)}; });
    return QString();
  });
}
// the selection scaled by a number, about its box's anchor point (the Gizmo pivot on each shell: each shell about its own)
QString Viewport::uvScaleBy(double k) {
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(tr("Select what to scale in the UV Editor (polygons, edges or UV points)")); return "!"; }
  if (!(k > 0)) return "!";
  const double axf = (uvo.anch % 3) * 0.5, ayf = (uvo.anch / 3) * 0.5;
  if (!uvo.pivShells) if (const auto B = uvSelBox()) { const auto b = *B; const double cu = b[0] + axf * (b[2] - b[0]), cv = b[1] + ayf * (b[3] - b[1]);   // (several meshes picked: about one anchor of the frame round all of it)
    if (uvMapAllPicked([=](const uv::UV& p) { return uv::UV{float(cu + (p[0] - cu) * k), float(cv + (p[1] - cv) * k)}; })) return QString(); }
  std::vector<std::vector<int>> groups;
  if (const UvCache* c = uvo.pivShells ? uvCache() : nullptr) { std::map<int, std::vector<int>> by; for (int q : ks) by[c->S.of[size_t(c->T->cf[size_t(q)])]].push_back(q); for (auto& [s, g] : by) groups.push_back(std::move(g)); }
  else groups.push_back(ks);
  return uvEdit([groups, k, axf, ayf](PolyMesh& m, const topo::TTopo&) -> QString {
    for (const auto& g : groups) { const auto b = uv::box(m, g); const double cu = b[0] + axf * (b[2] - b[0]), cv = b[1] + ayf * (b[3] - b[1]);
      uv::transform(m, g, [&](const uv::UV& p) { return uv::UV{float(cu + (p[0] - cu) * k), float(cv + (p[1] - cv) * k)}; }); }
    return QString();
  });
}
QString Viewport::uvSnapEdge(const QString& side) {
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(tr("Select what to snap in the UV Editor (polygons, edges or UV points)")); return "!"; }
  const double M = uvo.pad;
  return uvEdit([ks, side, M](PolyMesh& m, const topo::TTopo&) -> QString {
    auto b = uv::box(m, ks); const double bw = b[2] - b[0], bh = b[3] - b[1];
    double du = 0, dv = 0, k = 1, ou = b[0], ov = b[1];
    if (side == "left") du = M - b[0]; else if (side == "right") du = 1 - M - b[2];
    else if (side == "bottom") dv = M - b[1]; else if (side == "top") dv = 1 - M - b[3];
    else if (side == "centre") { du = 0.5 - (b[0] + b[2]) / 2; dv = 0.5 - (b[1] + b[3]) / 2; }
    else if (side == "fit") { const double room = std::max(1e-6, 1 - 2 * M); k = std::min(bw > 1e-12 ? room / bw : 1e9, bh > 1e-12 ? room / bh : 1e9); if (k > 1e8) k = 1;
      du = M + (room - bw * k) / 2 - b[0]; dv = M + (room - bh * k) / 2 - b[1]; }
    uv::transform(m, ks, [&](const uv::UV& p) { return uv::UV{float(ou + (p[0] - ou) * k + du), float(ov + (p[1] - ov) * k + dv)}; });
    const QString what = side == "fit" ? QObject::tr("fitted into the tile") : side == "centre" ? QObject::tr("to the tile's middle") : QObject::tr("to the %1 edge").arg(side);
    return QObject::tr("Snapped %1 (margin %2%)").arg(what).arg(M * 100, 0, 'g', 3);
  });
}
// Stretch: the UV stretched (each side by itself — the proportions change) to fill the tile inside the margin. Several meshes in the editor — all of them, as one set; one mesh — its selection, none selected — all of it
QString Viewport::uvStretchFill() {
  std::vector<int> ids = uvMulti() ? uvMeshes() : std::vector<int>{topoMesh()}; if (ids.empty() || ids[0] < 0) { ui::toast(tr("Select a mesh that has UV")); return "!"; }
  const bool multi = ids.size() > 1; std::vector<int> mv; if (!multi) mv = uvMoving();
  if (!multi && mv.empty()) if (const Item* it = scene_->item(ids[0])) if (const auto Tm = topo::ttopo(baseSp(*it))) {   // (nothing picked: what the mesh shows — the set worked in, not every set's corners)
    const PolyMesh& B = *baseOf(*it); const auto hid = uvHidden(*it, B); std::vector<int> fs;
    for (int f = 0; f < B.faceCount(); f++) if (hid.empty() || size_t(f) >= hid.size() || !hid[size_t(f)]) fs.push_back(f);
    if (fs.empty() && B.faceCount() > 0) { ui::toast(tr("Nothing to stretch")); return "!"; }
    mv = uv::faceCorners(*Tm, fs); }
  // (several meshes: each one's picked polygons — the picks on any of them say what goes; none picked anywhere — what each shows (the set worked in), never the
  //  hidden polygons of another set. All of them took every corner, the head's own set too, and the stretch missed the selection, 2026-11-10)
  std::map<int, std::vector<int>> per;
  if (multi) { const std::map<int, TSel> picks = [&] { auto p = tselX_; if (tsel_.id >= 0) p[tsel_.id] = tsel_; return p; }(); const char key = tKey(); bool anyPick = false;
    for (int id : ids) { const Item* it = scene_->item(id); if (!it) continue; const auto Tm = topo::ttopo(baseSp(*it)); const auto f = picks.find(id);
      if (Tm && topoMode != "object" && f != picks.end() && (!f->second.f.empty() || !f->second.e.empty() || !f->second.v.empty())) { const auto fs = selFaces(*Tm, key, f->second.f, f->second.e, f->second.v); if (!fs.empty()) { per[id] = uv::faceCorners(*Tm, fs); anyPick = true; } } }
    if (!anyPick) for (int id : ids) { const Item* it = scene_->item(id); if (!it) continue; const PolyMesh& B = *baseOf(*it); const auto Tm = topo::ttopo(baseSp(*it)); if (!Tm || !uv::has(B)) continue;
      const auto hid = uvHidden(*it, B); std::vector<int> fs; for (int f = 0; f < B.faceCount(); f++) if (hid.empty() || size_t(f) >= hid.size() || !hid[size_t(f)]) fs.push_back(f); if (!fs.empty()) per[id] = uv::faceCorners(*Tm, fs); } }
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  for (int id : ids) { const Item* it = scene_->item(id); if (!it) continue; const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) continue;
    if (!multi && !mv.empty()) { const auto b = uv::box(B, mv); x0 = b[0]; y0 = b[1]; x1 = b[2]; y1 = b[3]; }
    else if (multi) { const auto g = per.find(id); if (g == per.end()) continue; const auto b = uv::box(B, g->second); x0 = std::min(x0, b[0]); y0 = std::min(y0, b[1]); x1 = std::max(x1, b[2]); y1 = std::max(y1, b[3]); }
    else for (const auto& q : B.cornerUV) { x0 = std::min(x0, double(q[0])); y0 = std::min(y0, double(q[1])); x1 = std::max(x1, double(q[0])); y1 = std::max(y1, double(q[1])); } }
  if (!(x1 > x0) || !(y1 > y0)) { ui::toast(tr("Nothing to stretch")); return "!"; }
  const double M = uvo.pad;
  // (the room to fill: the tile within its margin, less what the shells NOT picked take — each one shown that lies beside the selection cuts the room
  //  on its side, at the padding. Before, the selection filled the whole tile and went over a strip left out of it, 2026-11-11)
  std::array<double, 4> R{M, M, 1 - M, 1 - M}; int kept = 0;
  { std::map<int, std::set<int>> selF; const bool some = multi ? !per.empty() : !mv.empty();
    if (some) { for (int id : ids) { const Item* it = scene_->item(id); if (!it) continue; const auto Tm = topo::ttopo(baseSp(*it)); if (!Tm) continue;
        const std::vector<int>* cs = multi ? (per.count(id) ? &per.at(id) : nullptr) : (id == topoMesh() ? &mv : nullptr); if (!cs) continue;
        for (int k : *cs) if (size_t(k) < Tm->cf.size()) selF[id].insert(Tm->cf[size_t(k)]); }
      std::vector<int> all = uvMeshes(); for (int id : ids) if (std::find(all.begin(), all.end(), id) == all.end()) all.push_back(id);
      std::vector<std::array<double, 4>> obs; const UvTex tx = uvTex(topoMesh()); const double pu = tx.w > 0 ? tx.pad / tx.w : 0, pv = tx.h > 0 ? tx.pad / tx.h : 0;
      for (int id : all) { const Item* it = scene_->item(id); if (!it) continue; const PolyMesh& B = *baseOf(*it); const auto Tm = topo::ttopo(baseSp(*it)); if (!Tm || !uv::has(B)) continue;
        const auto hid = uvHidden(*it, B); const uv::Points P = uv::points(B, *Tm); const uv::Shells S = uv::shells(B, *Tm, P); const auto sf = selF.find(id);
        std::vector<char> picked(size_t(S.count), 0); if (sf != selF.end()) for (int f : sf->second) picked[size_t(S.of[size_t(f)])] = 1;
        std::vector<std::array<double, 4>> bx(size_t(S.count), std::array<double, 4>{1e300, 1e300, -1e300, -1e300});
        for (int f = 0; f < Tm->nf; f++) { if (!hid.empty() && size_t(f) < hid.size() && hid[size_t(f)]) continue; auto& b = bx[size_t(S.of[size_t(f)])];
          for (int k = Tm->fo[size_t(f)]; k < Tm->fo[size_t(f) + 1]; k++) { const auto& q = B.cornerUV[size_t(k)]; b[0] = std::min(b[0], double(q[0])); b[1] = std::min(b[1], double(q[1])); b[2] = std::max(b[2], double(q[0])); b[3] = std::max(b[3], double(q[1])); } }
        for (int s = 0; s < S.count; s++) if (!picked[size_t(s)] && bx[size_t(s)][0] < 1e299) obs.push_back({bx[size_t(s)][0] - pu, bx[size_t(s)][1] - pv, bx[size_t(s)][2] + pu, bx[size_t(s)][3] + pv}); }
      const double eps = 1e-9;
      for (int pass = 0; pass < 3; pass++) for (const auto& u : obs) {
        if (u[2] <= R[0] || u[0] >= R[2] || u[3] <= R[1] || u[1] >= R[3]) continue;   // (not in the room)
        const double cx = (u[0] + u[2]) / 2, cy = (u[1] + u[3]) / 2;   // (beside the selection — or its middle past the selection's edge: it cuts the room on that side)
        if (u[2] <= x0 + eps || cx < x0) { R[0] = std::max(R[0], u[2]); kept++; } else if (u[0] >= x1 - eps || cx > x1) { R[2] = std::min(R[2], u[0]); kept++; }
        else if (u[3] <= y0 + eps || cy < y0) { R[1] = std::max(R[1], u[3]); kept++; } else if (u[1] >= y1 - eps || cy > y1) { R[3] = std::min(R[3], u[1]); kept++; } } } }   // (one in the selection's own box: it cannot be kept clear — left as it is)
  if (!(R[2] > R[0]) || !(R[3] > R[1])) { ui::toast(tr("No room to stretch into — the shells not picked fill it")); return "!"; }
  const double kx = (R[2] - R[0]) / (x1 - x0), ky = (R[3] - R[1]) / (y1 - y0), rx = R[0], ry = R[1];
  auto run = [&](const std::vector<int>& only) { return uvEdit([&, x0, y0, kx, ky, M, rx, ry, kept](PolyMesh& m, const topo::TTopo&) -> QString {
      std::vector<int> ks = only; if (ks.empty()) { ks.resize(m.cornerUV.size()); for (size_t k = 0; k < ks.size(); k++) ks[k] = int(k); }
      uv::transform(m, ks, [&](const uv::UV& p) { return uv::UV{float(rx + (p[0] - x0) * kx), float(ry + (p[1] - y0) * ky)}; });
      return (kept ? tr("Stretched into the free room (the shells not picked kept clear; margin %1%): ×%2 across, ×%3 up") : tr("Stretched to fill the tile (margin %1%): ×%2 across, ×%3 up")).arg(M * 100, 0, 'g', 3).arg(kx, 0, 'f', 2).arg(ky, 0, 'f', 2); }); };
  if (!multi) return run(mv);
  const std::vector<int> order = sel_.items; QString r; eachBusy_ = true; const int h0 = histIdx_;
  for (int id : ids) { const auto g = per.find(id); if (g == per.end() || g->second.empty()) continue; sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), id), sel_.items.end()); sel_.items.push_back(id); r = run(g->second); }
  eachBusy_ = false; histMerge(h0); sel_.items = order; onSelChange(); ui::toast(r); return r;
}

}  // namespace hm
