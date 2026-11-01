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
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSettings>
#include <QWheelEvent>
#include <memory>
#include <thread>
#include <cmath>
#include <numeric>

namespace hm {

namespace {
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
  QSettings().setValue("topo/uv", QJsonDocument(QJsonObject{{"edChecker", uvo.checker}, {"c3d", uvo.c3d}, {"s3d", uvo.s3d}, {"dispOp", uvo.dispOp}, {"tex", uvo.tex}, {"stretch", uvo.stretch}, {"rotate", uvo.rotate},
                                                            {"cells", uvo.density}, {"strAmt", uvo.strAmt}, {"strUp", uvo.strUp}, {"strSpread", uvo.strSpread}, {"boxMid", uvo.boxMid}, {"symUnfold", uvo.symUnfold}, {"symTint", uvo.symTint}, {"pivShells", uvo.pivShells}, {"diag", uvo.diag}, {"stretchAngle", uvo.stretchAngle}, {"symAxis", uvo.symAxis}, {"warpN", uvo.warpN}, {"pad", uvo.pad}, {"angle", uvo.angle}}).toJson(QJsonDocument::Compact));
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
  if (uvPtsVer_ == tsel_.ver && tsel_.id == topoMesh()) return uvPts_;
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
  auto a = pxToUv({x0, y1}), b = pxToUv({x1, y0}); auto in = [&](double u, double v) { return u >= a[0] && u <= b[0] && v >= a[1] && v <= b[1]; };
  const std::vector<char> hidF = uvHidden(*it, B); auto shown = [&](int f) { return hidF.empty() || size_t(f) >= hidF.size() || !hidF[size_t(f)]; };
  if (key == 'f') { for (int f = 0; f < T.nf; f++) if (shown(f)) { double u = 0, v = 0; int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; for (int k = s; k < e; k++) { u += B.cornerUV[size_t(k)][0]; v += B.cornerUV[size_t(k)][1]; } if (in(u / (e - s), v / (e - s))) o.push_back(f); } }
  else if (key == 'e') { std::set<int> es; for (int f = 0; f < T.nf; f++) if (shown(f)) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      if (in((B.cornerUV[size_t(k)][0] + B.cornerUV[size_t(nk)][0]) / 2.0, (B.cornerUV[size_t(k)][1] + B.cornerUV[size_t(nk)][1]) / 2.0)) es.insert(T.ce[size_t(k)]); } o.assign(es.begin(), es.end()); }
  else for (int p = 0; p < c->P.count(); p++) { int k = c->P.list[size_t(c->P.off[size_t(p)])]; if (shown(T.cf[size_t(k)]) && in(B.cornerUV[size_t(k)][0], B.cornerUV[size_t(k)][1])) o.push_back(p); }
  return o;
}
std::vector<int> Viewport::uvMoving() const {
  int m = topoMesh(); const UvCache* c = uvCache(); if (m < 0 || !c || tsel_.id != m) return {};
  const topo::TTopo& T = *c->T; const char key = tKey(); std::vector<int> ks;
  if (key == 'f') ks = uv::faceCorners(T, {tsel_.f.begin(), tsel_.f.end()});
  else if (key == 'e') { for (int e : tsel_.e) if (e >= 0 && e < T.ne) for (int f : {T.ef0[size_t(e)], T.ef1[size_t(e)]}) if (f >= 0) for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { int k = uv::cornerAt(T, f, v); if (k >= 0) ks.push_back(k); } }
  else { auto pts = const_cast<Viewport*>(this)->uvPoints(); for (int p : pts) for (int i = c->P.off[size_t(p)]; i < c->P.off[size_t(p) + 1]; i++) ks.push_back(c->P.list[size_t(i)]); }
  return uv::pointCorners(c->P, ks);
}

// ---------- the mouse in the editor ----------
void Viewport::uvPress(QMouseEvent* e) {
  const QPointF mp = e->position(); const bool alt = e->modifiers() & Qt::AltModifier, ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  uvd_ = {}; uvd_.p0 = uvd_.last = mp;
  if (alt || e->button() == Qt::MiddleButton) { uvd_.type = e->button() == Qt::RightButton ? 2 : 1; if (cam_.uvPpu <= 0) cam_.uvPpu = cam_.uvScale(paneW(), paneH()); drag_ = Drag::Uv; return; }
  if (e->button() == Qt::LeftButton && uvTf_.on && uvTfPress(mp)) { uvd_.type = 9; drag_ = Drag::Uv; return; }   // (the frame / the warp: its handles)
  if (e->button() == Qt::LeftButton && uvBrush && room() == "topo" && uvMode() && uvBrushPress(mp, ctrl)) { uvd_.type = 10; drag_ = Drag::Uv; return; }   // (the Relax / Pin brush)
  if (e->button() == Qt::RightButton && shift) { emit pieRequested(toGlobal(mp)); return; }
  if (e->button() != Qt::LeftButton || room() != "topo") return;
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
  // the gizmo (Move / Rotate / Scale): a handle pressed — that operation, along its axis, about the pivot
  if (uvGizmoOn()) if (const int h = uvGizmoHit(mp)) {
    bool take = true;
    if (h == 5) {                                                // (inside the frame on what isn't selected: that is picked, not moved)
      const UvPick pk = uvPickAt(mp); const char k = tKey(); const int el = k == 'f' ? pk.f : k == 'e' ? pk.e : pk.p;
      if (el >= 0 && !(k == 'f' ? tsel_.f.count(el) > 0 : k == 'e' ? tsel_.e.count(el) > 0 : uvPoints().count(el) > 0)) take = false;
    }
    const auto mv = take ? uvMoving() : std::vector<int>{};
    if (!mv.empty()) {
      const auto b = uv::box(B, mv); const double uc = (b[0] + b[2]) / 2, vc = (b[1] + b[3]) / 2;
      uvd_.type = 4; uvd_.m = am; uvd_.corners = mv; uvd_.uv0 = B.cornerUV; uvd_.cu = uc; uvd_.cv = vc; uvd_.axis = 0;
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
  if (dbl) {                                                   // the shell (polygons, points) / the edge loop
    if (key == 'e') { auto L = topo::edgeLoop(T, el); tselApply(am, L, "add"); ui::hud(tr("Edge loop: %1").arg(L.size()), 1000); return; }
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
    if (last >= 0 && last != el && any) { auto p = topo::path(T, tWorld(am), key, last, el); if (p) tselApply(am, *p, "add"); else { ui::toast(tr("No path between them (different parts of the mesh)")); tselApply(am, {el}, "add"); } }
    else tselApply(am, {el}, "add");
    tsel_.last = el; return;
  }
  if (key == 'v') uvLastPt_ = el;
  if (ctrl) { if (key == 'v') { auto pts = uvPoints(); pts.erase(el); pickPts(pts); } else tselApply(am, {el}, "remove"); return; }
  bool had = key == 'f' ? tsel_.f.count(el) > 0 : key == 'e' ? tsel_.e.count(el) > 0 : uvPoints().count(el) > 0;
  if (!had || shift) { if (key == 'v') { std::set<int> pts = shift ? uvPoints() : std::set<int>{}; pts.insert(el); pickPts(pts); } else tselApply(am, {el}, shift ? "add" : "set"); }
  // a drag on the selection moves it
  uvd_.type = 4; uvd_.op = 'm'; uvd_.m = am; uvd_.corners = uvMoving(); uvd_.uv0 = B.cornerUV; drag_ = Drag::Uv;
}

void Viewport::uvMove(QMouseEvent* e) {
  const QPointF mp = e->position();
  if (drag_ != Drag::Uv) {                                     // (the element under the pointer lit)
    if (uvBrush) { uvBrushAt_ = mp; overlayRev_++; requestUpdate(); }   // (the brush's circle follows)
    if (!uvMode() || topoMode == "object" || ui::Pie::current()) { if (uvHov_.f >= 0 || uvHov_.e >= 0 || uvHov_.p >= 0) { uvHov_ = {}; uvHovRev_++; requestUpdate(); } return; }
    UvPick p = uvPickAt(mp);
    if (p.f != uvHov_.f || p.ek != uvHov_.ek || p.p != uvHov_.p) { uvHov_ = {p.f, p.e, p.ek, p.p}; uvHovRev_++; requestUpdate();
      if (uvo.stretch) {                                         // (the stretch on: the polygon under the pointer — by how much)
        const int am = topoMesh(); const Item* it = am >= 0 ? scene_->item(am) : nullptr; const PolyMesh* B = it ? baseOf(*it) : nullptr;
        if (B && uv::has(*B) && p.f >= 0 && p.f < B->faceCount()) { double au = 0, a3 = 0; for (int f = 0; f < B->faceCount(); f++) { au += std::abs(uv::faceArea(*B, f)); a3 += uv::faceArea3(*B, f); }
          const double y = uv::faceArea3(*B, p.f), q = y > 0 && au > 0 ? std::abs(uv::faceArea(*B, p.f)) / y * (a3 / au) : 0;
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
  if (uvd_.type == 10) { uvBrushStroke(mp, e->modifiers()); requestUpdate(); return; }
  if (uvd_.type == 9) { uvTfMove(mp, e->modifiers()); return; }
  if (uvd_.type == 3) { lasso_ = {{uvd_.p0.x(), uvd_.p0.y()}, {mp.x(), uvd_.p0.y()}, {mp.x(), mp.y()}, {uvd_.p0.x(), mp.y()}}; uvd_.moved = true; overlayRev_++; requestUpdate(); return; }
  if (uvd_.type != 4 || uvd_.corners.empty()) return;
  if (!uvd_.moved && std::hypot(mp.x() - uvd_.p0.x(), mp.y() - uvd_.p0.y()) < 3) return;
  uvd_.moved = true; if (uvHov_.f >= 0 || uvHov_.ek >= 0 || uvHov_.p >= 0) { uvHov_ = {}; uvHovRev_++; }
  const bool shift = e->modifiers() & Qt::ShiftModifier;
  std::vector<std::array<float, 2>> U = uvd_.uv0;
  // (the pivot of each corner: the selection's middle — or, the gizmo's per shell setting, its shell's own)
  auto pv = [&](int k) { return uvd_.piv.size() == uvd_.uv0.size() ? uvd_.piv[size_t(k)] : std::array<double, 2>{uvd_.cu, uvd_.cv}; };
  if (uvd_.op == 'm') {
    double du = (mp.x() - uvd_.p0.x()) / s, dv = -(mp.y() - uvd_.p0.y()) / (s * cam_.uvAsp);
    if (uvd_.axis == 1) dv = 0; else if (uvd_.axis == 2) du = 0;   // (the gizmo's U / V arrow)
    else if (shift) { if (std::abs(du) > std::abs(dv)) dv = 0; else du = 0; }   // (Shift: along one axis)
    for (int k : uvd_.corners) U[size_t(k)] = {float(uvd_.uv0[size_t(k)][0] + du), float(uvd_.uv0[size_t(k)][1] + dv)};
    const UvTex tx = uvTex(uvd_.m);
    ui::hud(tr("Move U %1 · V %2  (%3 · %4 px)").arg(du, 0, 'f', 4).arg(dv, 0, 'f', 4).arg(du * tx.w, 0, 'f', 1).arg(dv * tx.h, 0, 'f', 1));
  } else {
    QPointF c = uvToPx(uvd_.cu, uvd_.cv);
    if (uvd_.op == 'r') {
      double a = std::atan2(-(mp.y() - c.y()), mp.x() - c.x()) - std::atan2(-(uvd_.p0.y() - c.y()), uvd_.p0.x() - c.x());
      if (shift) a = std::round(a / (M_PI / 12)) * (M_PI / 12);
      const double cs = std::cos(a), sn = std::sin(a);
      const double ka = cam_.uvAsp;   // (turned as texels are: V in the texture's proportion)
      for (int k : uvd_.corners) { const auto p = pv(k); double x = uvd_.uv0[size_t(k)][0] - p[0], y = (uvd_.uv0[size_t(k)][1] - p[1]) * ka; U[size_t(k)] = {float(p[0] + x * cs - y * sn), float(p[1] + (x * sn + y * cs) / ka)}; }
      ui::hud(tr("Rotate %1°").arg(a * 180 / M_PI, 0, 'f', 1));
    } else {
      double r0 = std::max(2.0, std::hypot(uvd_.p0.x() - c.x(), uvd_.p0.y() - c.y())), r = std::hypot(mp.x() - c.x(), mp.y() - c.y()), k0 = r / r0;
      double ku = k0, kv = k0;
      if (uvd_.axis == 3) {                                       // (the frame's corner: both ways, each its own — Shift: evenly)
        ku = (mp.x() - c.x()) / (std::abs(uvd_.p0.x() - c.x()) < 2 ? 2.0 : uvd_.p0.x() - c.x()); kv = (mp.y() - c.y()) / (std::abs(uvd_.p0.y() - c.y()) < 2 ? 2.0 : uvd_.p0.y() - c.y());
        if (shift) { const double k = std::abs(ku) > std::abs(kv) ? ku : kv; ku = kv = k; } }
      else if (uvd_.axis == 1) { ku = (mp.x() - c.x()) / (std::abs(uvd_.p0.x() - c.x()) < 2 ? 2.0 : uvd_.p0.x() - c.x()); kv = 1; }   // (the frame's side: one way)
      else if (uvd_.axis == 2) { kv = (mp.y() - c.y()) / (std::abs(uvd_.p0.y() - c.y()) < 2 ? -2.0 : uvd_.p0.y() - c.y()); ku = 1; }
      else if (shift) { double dx = std::abs(mp.x() - uvd_.p0.x()), dy = std::abs(mp.y() - uvd_.p0.y());   // (Shift: along the axis the pointer goes)
        if (dx > dy) { ku = std::abs(mp.x() - c.x()) / std::max(2.0, std::abs(uvd_.p0.x() - c.x())); kv = 1; } else { kv = std::abs(mp.y() - c.y()) / std::max(2.0, std::abs(uvd_.p0.y() - c.y())); ku = 1; } }
      for (int k : uvd_.corners) { const auto p = pv(k); U[size_t(k)] = {float(p[0] + (uvd_.uv0[size_t(k)][0] - p[0]) * ku), float(p[1] + (uvd_.uv0[size_t(k)][1] - p[1]) * kv)}; }
      ui::hud(tr("Scale U %1 · V %2").arg(ku, 0, 'f', 3).arg(kv, 0, 'f', 3));
    }
  }
  uvLive(uvd_.m, U);
}

void Viewport::uvRelease(QMouseEvent* e) {
  const QPointF mp = e->position(); UvDragS d = uvd_; uvd_ = {};
  if (drag_ == Drag::Uv) drag_ = Drag::None;
  if (d.type == 10) { uvBrushRelease(d); requestUpdate(); return; }
  if (d.type == 9) { uvTfRelease(); return; }
  if (d.type == 3) {
    lasso_.clear(); overlayRev_++;
    int am = topoMesh(); if (am < 0) { requestUpdate(); return; }
    const char key = tKey();
    if (!d.moved || std::hypot(mp.x() - d.p0.x(), mp.y() - d.p0.y()) < 4) {
      if (d.mode == "set" && tsel_.f.size() + tsel_.e.size() + tsel_.v.size()) tselClear();
      requestUpdate(); return;
    }
    auto list = uvBox(std::min(d.p0.x(), mp.x()), std::min(d.p0.y(), mp.y()), std::max(d.p0.x(), mp.x()), std::max(d.p0.y(), mp.y()), key);
    if (key == 'v') {
      std::set<int> pts = d.mode == "set" ? std::set<int>{} : uvPoints();
      for (int p : list) { if (d.mode == "remove") pts.erase(p); else pts.insert(p); }
      const UvCache* c = uvCache(); std::set<int> vs; for (int p : pts) vs.insert(c->P.vert[size_t(p)]);
      tselApply(am, {vs.begin(), vs.end()}, "set", 'v'); uvPts_ = pts; uvPtsVer_ = tsel_.ver;
    } else tselApply(am, list, d.mode);
    ui::hud(tr("%1 %2").arg(list.size()).arg(d.mode == "remove" ? tr("deselected") : tr("selected")), 900);
    requestUpdate(); return;
  }
  if (d.type == 4 && d.moved) {
    ui::hud({});
    if (Item* it = scene_->item(d.m)) { topoWrite(d.m, baseSp(*it)); uvStamp_++; uvRev_++; commit(false); emit topoState(); }
    if (d.op == 'm' && tKey() == 'v') uvPtsVer_ = tsel_.ver;     // (the points stay picked as they were)
  }
  requestUpdate();
}

// the UV shown while dragging (a copy of the base — the undo step keeps the one before)
void Viewport::uvLive(int m, const std::vector<std::array<float, 2>>& U) {
  overlayRev_++;                                               // (what is drawn over the editor — the Puppet's pins — follows)
  if (!scene_->item(m)) return;                               // (checks: a puppet with no mesh)
  if (tset.sym) { auto S = U; uvSymApply(S); liveApply(m, nullptr, &S); return; }   // (the UV symmetry: the mirrored shells follow)
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
  if (am >= 0) if (const Item* pi = scene_->item(am)) sig += "|a" + QByteArray::number(qHash(QJsonDocument(pi->raw.value("uvShellAttr").toArray()).toJson(QJsonDocument::Compact)));   // (the locked, the groups)
  if (sig == uvSig_) return uvDraw_;
  uvSig_ = sig; uvDraw_ = {}; UvDraw& D = uvDraw_;
  if (tset.sym && uvMode()) { UvDraw::Lines a; a.c = QColor(0xc8, 0x6b, 0xff); a.w = 1.6f; a.p = {{float(uvo.symAxis), -3}, {float(uvo.symAxis), 4}}; D.lines.push_back(a); }   // (the UV symmetry's axis)
  // the grid: the 0…1 tile in tenths, the tiles round it, the tile's outline
  { UvDraw::Lines g; g.c = QColor(0x36, 0x36, 0x36); for (int i = 1; i < 10; i++) { float t = i / 10.f; g.p.push_back({t, 0}); g.p.push_back({t, 1}); g.p.push_back({0, t}); g.p.push_back({1, t}); } D.grid.push_back(g);
    UvDraw::Lines o; o.c = QColor(0x44, 0x44, 0x44); for (int i = -3; i <= 4; i++) { o.p.push_back({float(i), -3}); o.p.push_back({float(i), 4}); o.p.push_back({-3, float(i)}); o.p.push_back({4, float(i)}); } D.grid.push_back(o);
    UvDraw::Lines t; t.c = QColor(0x8a, 0x8a, 0x8a); t.w = 1.5f; for (auto [a, b] : {std::pair<std::array<float, 2>, std::array<float, 2>>{{0, 0}, {1, 0}}, {{1, 0}, {1, 1}}, {{1, 1}, {0, 1}}, {{0, 1}, {0, 0}}}) { t.p.push_back(a); t.p.push_back(b); } D.grid.push_back(t); }
  if (room() != "topo" || !tset.uv) return D;
  // the other selected meshes: dim
  for (int id : topoMeshes()) {
    if (id == am || !uvShown(id)) continue; const Item* it = scene_->item(id); const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) continue;
    const std::vector<char> hidF = uvHidden(*it, B); UvDraw::Lines l; l.c = QColor(0x5c, 0x5c, 0x66);
    for (int f = 0; f < B.faceCount(); f++) { if (!hidF.empty() && hidF[size_t(f)]) continue;
      int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { l.p.push_back(B.cornerUV[size_t(k)]); l.p.push_back(B.cornerUV[size_t(k + 1 < e ? k + 1 : s)]); } }
    D.lines.push_back(std::move(l));
  }
  if (am < 0) return D;
  const Item* it = scene_->item(am); const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) return D;
  const UvCache* c = uvCache(); if (!c) return D; const topo::TTopo& T = *c->T;
  if (uvo.tex && looks::shown(*it)) { D.texMesh = am; if (uvPicked.id == am) D.texMat = uvPicked.mat; }
  D.checker = uvo.checker; D.op = float(uvo.dispOp);                // (the checker under the UV first; the opacity of what is under it)
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
  { auto fl = uv::flipped(B, T, c->S); if (!fl.empty()) { UvDraw::Tris t; t.op = 0.5f; for (int f : fl) if (shown(f)) faceTris(t, f, QColor(0xe0, 0x30, 0x30)); D.tris.push_back(std::move(t)); } }
  // the checks shown: over another — magenta, past the tile — orange, the padding — yellow, the density — cyan, a mirror
  // pair not mirrored — violet (the later over the earlier)
  if (uvo.diag) { const auto& r = uvCheck(); UvDraw::Tris t; t.op = 0.45f;
    for (auto [fs, col] : {std::pair<const std::vector<int>*, QColor>{&r.asym, QColor(0x90, 0x60, 0xff)}, {&r.density, QColor(0x30, 0xc0, 0xe0)}, {&r.padding, QColor(0xf0, 0xd0, 0x20)}, {&r.outside, QColor(0xff, 0x8c, 0x1a)}, {&r.overlap, QColor(0xe0, 0x40, 0xe0)}})
      for (int f : *fs) if (f < T.nf && shown(f)) faceTris(t, f, col);
    if (!t.p.empty()) D.tris.push_back(std::move(t)); }
  // the set worked in: what is not in it darkened (its polygons, their edges dim)
  std::vector<char> dimF; if (uvPicked.id == am && uvPicked.mat == -2 && uvPicked.set >= 0 && topo::hasUvSets(B)) {
    dimF.assign(size_t(T.nf), 0); UvDraw::Tris t; t.op = 0.55f;
    for (int f = 0; f < T.nf; f++) if (!(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> uvPicked.set & 1u))) { dimF[size_t(f)] = 1; if (shown(f)) faceTris(t, f, QColor(0x08, 0x08, 0x0c)); }
    if (!t.p.empty()) D.tris.push_back(std::move(t)); }
  auto dim = [&](int f) { return !dimF.empty() && dimF[size_t(f)]; };
  if (tset.sym && uvo.symTint) { const auto mx = uvMinusXFaces(); UvDraw::Tris t; t.op = 0.55f;   // (the −X half: burgundy)
    for (int f = 0; f < T.nf && size_t(f) < mx.size(); f++) if (mx[size_t(f)] && shown(f) && !dim(f)) faceTris(t, f, QColor(0x9c, 0x1a, 0x32));
    if (!t.p.empty()) D.tris.push_back(std::move(t)); }
  // each shell's own settings: a locked one slate (its outline too, below); a pack group — a colour of its own
  const auto shAttr = uvShellAttrs(am); bool anyLock = false;
  { static const QColor gc[] = {QColor(0x9c, 0x64, 0xf0), QColor(0xe0, 0x9c, 0x30), QColor(0x3c, 0xb4, 0x6e), QColor(0x28, 0xa0, 0xb4), QColor(0xd2, 0x5a, 0x96), QColor(0x96, 0xb4, 0x28)};
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
  // the edges; the shells' outlines; the seams; the selected edges
  const std::vector<char>& seam = c->seam; const std::vector<char>& cont = c->cont;
  UvDraw::Lines dl, all, out, sm, se; dl.c = QColor(0x4a, 0x4a, 0x52); all.c = QColor(0xa8, 0xa8, 0xb4); out.c = BORDER; out.w = 1.8f; sm.c = SEAM; sm.w = 2.6f; se.c = SEL; se.w = 2.8f;
  for (int f = 0; f < T.nf; f++) if (shown(f)) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) {
    int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)], e = T.ce[size_t(k)]; const auto &a = B.cornerUV[size_t(k)], &b = B.cornerUV[size_t(nk)];
    UvDraw::Lines& L = dim(f) ? dl : seam[size_t(e)] ? sm : !cont[size_t(e)] ? out : all; L.p.push_back(a); L.p.push_back(b);
    if (comp && key == 'e' && tsel_.id == am && tsel_.e.count(e)) { se.p.push_back(a); se.p.push_back(b); }
  }
  for (auto* L : {&dl, &all, &out, &sm, &se}) if (!L->p.empty()) D.lines.push_back(std::move(*L));
  if (anyLock) { UvDraw::Lines ll; ll.c = QColor(0xc8, 0xd4, 0xf0); ll.w = 3.0f;   // (the locked shells' outlines)
    for (int f = 0; f < T.nf && size_t(f) < shAttr.size(); f++) if (shown(f) && shAttr[size_t(f)].lock) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) {
      if (c->cont[size_t(T.ce[size_t(k)])]) continue; const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; ll.p.push_back(B.cornerUV[size_t(k)]); ll.p.push_back(B.cornerUV[size_t(nk)]); }
    if (!ll.p.empty()) D.lines.push_back(std::move(ll)); }
  { const auto feats = uvFeaturesOf(am);                              // (the face's lips / lids: their rims orange — shut: amber)
    UvDraw::Lines fo, fc; fo.c = QColor(0xff, 0x9a, 0x3c); fo.w = 2.6f; fc.c = QColor(0xff, 0xd0, 0x40); fc.w = 2.6f;
    for (auto& f : feats) for (auto* c : {&f.a, &f.b}) for (size_t i = 0; i + 1 < c->size(); i++) {
      const int e = T.edgeOf((*c)[i], (*c)[i + 1]); if (e < 0) continue;
      for (int fi : {T.ef0[size_t(e)], T.ef1[size_t(e)]}) { if (fi < 0 || !shown(fi)) continue; const int k0 = uv::cornerAt(T, fi, (*c)[i]), k1 = uv::cornerAt(T, fi, (*c)[i + 1]);
        if (k0 >= 0 && k1 >= 0) { auto& L = f.closed ? fc : fo; L.p.push_back(B.cornerUV[size_t(k0)]); L.p.push_back(B.cornerUV[size_t(k1)]); } } }
    for (auto* L : {&fo, &fc}) if (!L->p.empty()) D.lines.push_back(std::move(*L)); }
  { const uv::Pins pins = uvPinsOf(am);                               // (the pinned edges: yellow, over the rest)
    if (!pins.empty()) { UvDraw::Lines pl; pl.c = QColor(0xff, 0xe0, 0x3a); pl.w = 3.2f;
      for (int f = 0; f < T.nf; f++) if (shown(f)) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
        const int a = T.fv[size_t(k)], b = T.fv[size_t(nk)]; if (pins.count({std::min(a, b), std::max(a, b)})) { pl.p.push_back(B.cornerUV[size_t(k)]); pl.p.push_back(B.cornerUV[size_t(nk)]); } }
      if (!pl.p.empty()) D.lines.push_back(std::move(pl)); }
    if (!pins.empty()) { UvDraw::Pts pp; pp.size = 7;                 // (the pinned points: yellow dots)
      for (int k = 0; k < int(T.fv.size()); k++) { const int v = T.fv[size_t(k)]; if (pins.count({v, v}) && shown(T.cf[size_t(k)])) { pp.p.push_back(B.cornerUV[size_t(k)]); pp.c.push_back(QColor(0xff, 0xe0, 0x3a)); } }
      if (!pp.p.empty()) D.pts.push_back(std::move(pp)); } }
  // the points (Vertex mode)
  if (comp && key == 'v') {
    std::set<int> pts = uvPoints(); UvDraw::Pts p; p.size = 5;
    for (int q = 0; q < c->P.count(); q++) { int k = c->P.list[size_t(c->P.off[size_t(q)])]; if (!shown(T.cf[size_t(k)])) continue; p.p.push_back(B.cornerUV[size_t(k)]); p.c.push_back(pts.count(q) ? SEL : QColor(0xd8, 0xd8, 0xd8)); }
    D.pts.push_back(std::move(p));
  }
  if (uvTf_.on) { if (uvTf_.m != am || topoMode == "object") uvTf_ = {}; else uvTfDraw(D); }
  return D;
}

// the element under the pointer (its polygon, its edge's side, its point): a small layer of its own
const Viewport::UvDraw& Viewport::uvHoverDraw() {
  const int am = topoMesh();
  QByteArray sig = QByteArray::number(uvHovRev_) + "|" + QByteArray::number(uvRev_) + "|" + QByteArray::number(am) + topoMode.toLatin1() + QByteArray::number(int(tset.uv));
  sig += "|h" + QByteArray::number(thov_.m) + "," + QByteArray::number(thov_.f) + "," + QByteArray::number(thov_.e) + "," + QByteArray::number(thov_.v) + "|s" + QByteArray::number(uvStamp_);   // (the 3D hover: the preview)
  if (sig == uvHovSig_) return uvHovDraw_;
  uvHovSig_ = sig; uvHovDraw_ = {}; UvDraw& D = uvHovDraw_;
  if (room() != "topo" || !tset.uv || am < 0 || topoMode == "object" || drag_ == Drag::Uv) return D;
  { const auto& pv = uvPreviewLines();                                // (the shell under the pointer in 3D, unwrapped: cyan)
    if (!pv.empty()) { UvDraw::Tris veil; veil.op = 0.72f; const float R = 1e4f;   // (the UV under it dimmed: the preview read on its own)
      veil.p = {{-R, -R}, {R, -R}, {R, R}, {-R, -R}, {R, R}, {-R, R}}; veil.c = {QColor(0x10, 0x10, 0x14), QColor(0x10, 0x10, 0x14)}; D.tris.push_back(std::move(veil));
      UvDraw::Lines l; l.c = QColor(0x6a, 0xe4, 0xff); l.w = 1.6f; l.p = pv; D.lines.push_back(std::move(l)); } }
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
  int fl = int(uv::flipped(B, *c->T, c->S).size()); double cov = uv::coverage(B);
  return tr("%1 shell%2 · %3 seam%4 · the tile %5% filled%6").arg(c->S.count).arg(c->S.count == 1 ? "" : "s").arg(B.seams.size()).arg(B.seams.size() == 1 ? "" : "s")
         .arg(int(std::lround(cov * 100))).arg(fl ? tr(" · %1 turned over (red)").arg(fl) : QString());
}

// ---------- the edits ----------
QString Viewport::uvEdit(const std::function<QString(PolyMesh&, const topo::TTopo&)>& fn) {
  int am = topoMesh(); if (am < 0) { ui::toast(tr("Select a mesh first")); return "!"; }
  if (topoHeavy(am)) { ui::toast(tr("A high-poly mesh (over 300 000 triangles) is not edited")); return "!"; }
  Item* it = scene_->item(am); auto B = baseSp(*it); auto T = topo::ttopo(B); if (!T) return "!";
  auto nb = std::make_shared<PolyMesh>(*B);
  QString msg = fn(*nb, *T);
  if (msg.startsWith('!')) { if (msg.size() > 1) ui::toast(msg.mid(1)); return msg; }
  if (nb->cornerUV.size() == nb->faceVerts.size()) uvSymApply(nb->cornerUV);   // (the UV symmetry: the mirrored shells follow)
  topo::shareTopo(nb, B); topoWrite(am, nb); uvStamp_++; uvRev_++;
  scene_->revision++; commit(true); emit topoState(); if (!msg.isEmpty()) ui::toast(msg);
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
  const int am = topoMesh(); const char key = tKey();
  if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select edges (Edge mode) — or polygons: their outline")); return "!"; }
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; const bool sym = tset.sym;
  return uvEdit([&, key, sym](PolyMesh& m, const topo::TTopo& T) -> QString {
    auto es = selEdges(T, key, F, E, V); if (es.empty()) return tr("!Select edges (Edge mode) — or polygons: their outline");
    if (sym) { const auto me = uv::mirrorEdges(m, T); const size_t n0 = es.size(); for (size_t i = 0; i < n0; i++) if (me[size_t(es[i])] >= 0) es.push_back(me[size_t(es[i])]);   // (Symmetry: the mirror seams too)
      std::sort(es.begin(), es.end()); es.erase(std::unique(es.begin(), es.end()), es.end()); }
    auto fl = uv::seamFlags(m, T); int n = 0; for (int e : es) if (bool(fl[size_t(e)]) != on) { fl[size_t(e)] = on; n++; }
    if (!n) return on ? tr("!These edges are seams already") : tr("!None of these edges is a seam");
    uv::setSeams(m, T, fl); return on ? tr("%1 seam%2 marked").arg(n).arg(n == 1 ? "" : "s") : tr("%1 seam%2 cleared").arg(n).arg(n == 1 ? "" : "s");
  });
}
QString Viewport::uvSew() {
  const int am = topoMesh(); const char key = tKey();
  if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select the edges to sew (Edge mode)")); return "!"; }
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v;
  return uvEdit([&, key](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV");
    auto es = selEdges(T, key, F, E, V); if (es.empty()) return tr("!Select the edges to sew (Edge mode)");
    auto fl = uv::seamFlags(m, T); int n = 0;
    auto cornerAt = [&](int f, int v) { for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) if (T.fv[size_t(k)] == v) return k; return -1; };
    for (int e : es) {
      const int f0 = T.ef0[size_t(e)], f1 = T.ef1[size_t(e)]; if (f1 < 0) continue; bool moved = false;
      for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { const int k0 = cornerAt(f0, v), k1 = cornerAt(f1, v); if (k0 < 0 || k1 < 0) continue;
        auto& a = m.cornerUV[size_t(k0)]; auto& b = m.cornerUV[size_t(k1)]; if (a == b) continue;
        const std::array<float, 2> c{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2}; a = c; b = c; moved = true; }
      if (fl[size_t(e)]) { fl[size_t(e)] = 0; moved = true; }
      if (moved) n++;
    }
    if (!n) return tr("!Nothing to sew — the UV is joined along these edges already");
    uv::setSeams(m, T, fl); return tr("%1 edge%2 sewn").arg(n).arg(n == 1 ? "" : "s");
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
QString Viewport::uvClearSeams() {
  return uvEdit([](PolyMesh& m, const topo::TTopo&) -> QString { if (m.seams.empty()) return QObject::tr("!No seams"); int n = int(m.seams.size()); m.seams.clear(); return QObject::tr("%1 seam%2 cleared").arg(n).arg(n == 1 ? "" : "s"); });
}
// Auto seams: the selection's closed shells (none selected — every shown one) — seams to open each into one piece
QString Viewport::uvAutoSeams() {
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; const auto G = uvMatGroups(am);
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
QString Viewport::uvUnfold(bool all, bool symPairs) {
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; const auto G = uvMatGroups(am); const double pad = uvo.pad; const bool rot = uvo.rotate; const uv::Pins pins = uvPinsOf(am); const uv::PackOpts po = uvPackOpts(am); const auto feats = uvFeaturesOf(am);
  const bool symOn = uvo.symUnfold || symPairs, pairsOn = tset.sym || symPairs; const double axis = uvo.symAxis;
  return uvEdit([&, key, comp, all](PolyMesh& m, const topo::TTopo& T) -> QString {
    std::vector<int> fs; if (!all) { if (comp) fs = selFaces(T, key, F, E, V); if (fs.empty()) return tr("!Select polygons of the shells to unfold — or Unfold all"); }
    else for (auto& g : G) fs.insert(fs.end(), g.begin(), g.end());
    ui::BusyScope bz(tr("Unfolding"));
    const auto attrs = uvShellAttrs(am); uv::PackOpts po2 = po; po2.attr = &attrs;   // (each shell's own settings; Unfold all: the locked left)
    bool anyLock = false; for (int f : fs) if (size_t(f) < attrs.size() && attrs[size_t(f)].lock) anyLock = true;
    if (all) fs.erase(std::remove_if(fs.begin(), fs.end(), [&](int f) { return size_t(f) < attrs.size() && attrs[size_t(f)].lock; }), fs.end());
    const bool repack = all && (G.size() > 1 || anyLock);   // (the locked stay: the rest packed round them)
    uv::SymUnfold su; if (symOn) { su.mc = uv::mirrorCorners(m, T); su.pairs = pairsOn; su.axis = axis; }
    auto r = uv::unfold(m, T, fs, !all || repack, true, &pins, symOn ? &su : nullptr, &po2, &feats);
    int autoN = 0;   // (closed shells: seams of their own put on them — Auto seams — and unfolded again)
    if (r.closed && !r.closedFaces.empty()) { const auto es = uv::autoSeams(m, T, r.closedFaces); auto fl = uv::seamFlags(m, T); for (int e : es) if (!fl[size_t(e)]) { fl[size_t(e)] = 1; autoN++; }
      if (autoN) { uv::setSeams(m, T, fl); const auto cf = r.closedFaces; const auto r2 = uv::unfold(m, T, cf, !all || repack, true, &pins, symOn ? &su : nullptr, &po2, &feats); r.done += r2.done; r.closed = r2.closed; r.symmetric += r2.symmetric; } }
    if (repack) for (auto& g : groupsByShell(m, T, G)) { uv::PackOpts o = po2; o.rotate = o.rotate && !r.pinned; if (symOn && pairsOn) { o.mc = &su.mc; o.axis = axis; } if (!uv::packShapes(m, T, g, o).ok) uv::pack(m, T, g, pad, rot && !r.pinned); }   // (each UV Mat into the tile of its own — they overlap)
    if (!r.done) return r.closed ? tr("!Closed — no border to open it: mark a seam on it (Edge mode → Mark seam)") : tr("!Nothing to unfold");
    QString s = tr("%1 shell%2 unfolded").arg(r.done).arg(r.done == 1 ? "" : "s");
    if (autoN) s += tr(" · closed ones opened by Auto seams (%1 edge%2 — see them, move them, Unfold again)").arg(autoN).arg(autoN == 1 ? "" : "s");
    if (r.symmetric) s += tr(" · %1 symmetric").arg(r.symmetric);
    if (r.mirrored) s += tr(" · %1 mirrored from %2 other side").arg(r.mirrored).arg(r.mirrored == 1 ? tr("its") : tr("their"));
    if (r.asym) s += tr(" · %1 across the middle, seams not symmetric — unfolded as %2 %3").arg(r.asym).arg(r.asym == 1 ? tr("it") : tr("they")).arg(r.asym == 1 ? tr("is") : tr("are"));
    if (r.closed) s += tr(" · %1 closed one%2 left — mark a seam on %3").arg(r.closed).arg(r.closed == 1 ? "" : "s").arg(r.closed == 1 ? tr("it") : tr("them"));
    if (r.holes) s += tr(" · %1 with holes or handles — more seams make %2 flatter").arg(r.holes).arg(r.holes == 1 ? tr("it") : tr("them"));
    return s;
  });
}
// Optimize (as RizomUV's): the selection's shells (none picked — all) relaxed as they lie — less stretch; no seams cut,
// nothing unfolded anew or packed; pinned edges held
QString Viewport::uvOptimize() {
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; const uv::Pins pins = uvPinsOf(am); const uv::PackOpts po = uvPackOpts(am); const auto feats = uvFeaturesOf(am); const auto attrs = uvShellAttrs(am);
  return uvEdit([&, key, comp](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    std::vector<int> fs; if (comp) fs = selFaces(T, key, F, E, V);
    if (fs.empty()) { bool any = false; for (auto& a : attrs) any = any || a.lock; if (any) for (int f = 0; f < T.nf; f++) if (size_t(f) >= attrs.size() || !attrs[size_t(f)].lock) fs.push_back(f); }   // (all but the locked)
    ui::BusyScope bz(tr("Optimizing"));
    const int n = uv::optimize(m, T, fs, &pins, 60, &po, &feats);
    if (!n) return tr("!Nothing to optimize");
    return tr("%1 shell%2 optimized — less stretch").arg(n).arg(n == 1 ? "" : "s");
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
QString Viewport::uvPack(bool all) {
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; const auto G = uvMatGroups(am);
  const uv::PackOpts po = uvPackOpts(am); const bool sym = tset.sym; const double axis = uvo.symAxis;
  return uvEdit([&, key, comp, all, sym, axis](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    std::vector<int> fs; if (!all) { if (comp) fs = selFaces(T, key, F, E, V); if (fs.empty()) return tr("!Select polygons of the shells to pack — or Pack all"); }
    ui::BusyScope bz(tr("Packing"));
    // (by shape into the texture the set is laid out for; Symmetry on: across the plane on the axis, pairs mirrored)
    std::vector<int> mc; uv::PackOpts o = po; if (sym) { mc = uv::mirrorCorners(m, T); o.mc = &mc; o.axis = axis; }
    const auto attrs = uvShellAttrs(am); o.attr = &attrs;   // (each shell's own: locked, not turned, its scale, its group)
    int straddle = 0; std::vector<std::vector<int>> groups; if (all) groups = groupsByShell(m, T, G, &straddle); else groups = {fs};
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
    if (straddle) s += tr(" · %1 shell%2 in two UV Mats — packed with the mat most of it is in").arg(straddle).arg(straddle == 1 ? "" : "s");
    if (tot.locked) s += tr(" · %1 locked left in place").arg(tot.locked); if (tot.stacked) s += tr(" · %1 stacked kept on their twins").arg(tot.stacked);
    if (sym) { s += tr(" · symmetric: %1 on the axis, %2 pair%3 mirrored").arg(tot.centred - tot.offAxis).arg(tot.pairs - tot.split).arg(tot.pairs - tot.split == 1 ? "" : "s");
      if (tot.offAxis) s += tr(" · %1 across the middle with no room on the axis — upright beside it").arg(tot.offAxis);
      if (tot.split) s += tr(" · %1 pair%2 too wide for a half — laid apart").arg(tot.split).arg(tot.split == 1 ? "" : "s"); }
    return s;
  });
}
namespace {
QString needSel() { return QObject::tr("!Select what to turn in the UV Editor (polygons, edges or UV points)"); }
}
QString Viewport::uvTurn(int q) {
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(needSel().mid(1)); return "!"; }
  const double ka = cam_.uvAsp;                                // (a quarter turn as texels turn: V in the texture's proportion)
  return uvEdit([ks, q, ka](PolyMesh& m, const topo::TTopo&) -> QString {
    auto b = uv::box(m, ks); double cu = (b[0] + b[2]) / 2, cv = (b[1] + b[3]) / 2;
    uv::transform(m, ks, [&](const uv::UV& p) { double x = p[0] - cu, y = (p[1] - cv) * ka; return q > 0 ? uv::UV{float(cu - y), float(cv + x / ka)} : uv::UV{float(cu + y), float(cv - x / ka)}; });
    return QObject::tr("Turned by 90° %1").arg(q > 0 ? QObject::tr("anticlockwise") : QObject::tr("clockwise"));
  });
}
QString Viewport::uvFlip(bool u) {
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(needSel().mid(1)); return "!"; }
  return uvEdit([ks, u](PolyMesh& m, const topo::TTopo&) -> QString {
    auto b = uv::box(m, ks); double cu = (b[0] + b[2]) / 2, cv = (b[1] + b[3]) / 2;
    uv::transform(m, ks, [&](const uv::UV& p) { return u ? uv::UV{float(2 * cu - p[0]), p[1]} : uv::UV{p[0], float(2 * cv - p[1])}; });
    return u ? QObject::tr("Mirrored in U") : QObject::tr("Mirrored in V");
  });
}
QString Viewport::uvStraighten(bool alongU) {
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
  int n = tSelectFaces(am, all); ui::toast(tr("%1 shell%2 selected (%3)").arg(sh.size()).arg(sh.size() == 1 ? "" : "s").arg(n));
  return {};
}


// hair strands: the selection's shells (all — every shown one) unbent
QString Viewport::uvStraightenStrands(bool all) {
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; const double amt = uvo.strAmt; const bool up = uvo.strUp, spread = uvo.strSpread; const double pad = uvo.pad;
  std::vector<int> shown; if (all && am >= 0) for (auto& g : uvMatGroups(am)) shown.insert(shown.end(), g.begin(), g.end());
  return uvEdit([&, key, comp, all, amt, up, spread, pad](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    std::vector<int> fs = all ? shown : comp ? selFaces(T, key, F, E, V) : std::vector<int>{};
    if (fs.empty()) return all ? tr("!Nothing shown to straighten") : tr("!Select polygons of the strands (their shells) — or Straighten all");
    ui::BusyScope bz(tr("Straightening"));
    const int n = uv::straighten(m, T, fs, amt, up); if (!n) return tr("!Nothing to straighten");
    if (spread) uv::pack(m, T, fs, pad, false);              // (laid apart in the tile, as they stand — none over another)
    return tr("%1 strand%2 straightened by %3%").arg(n).arg(n == 1 ? "" : "s").arg(int(std::lround(amt * 100)));
  });
}

// hair strands as strips: the selection's shells (all — every shown one)
QString Viewport::uvRectangleStrands(bool all) {
  const int am = topoMesh(); const char key = tKey(); const bool comp = topoMode != "object" && tsel_.id == am;
  const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; const bool spread = uvo.strSpread; const double pad = uvo.pad;
  std::vector<int> shown; if (all && am >= 0) for (auto& g : uvMatGroups(am)) shown.insert(shown.end(), g.begin(), g.end());
  return uvEdit([&, key, comp, all, spread, pad](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    std::vector<int> fs = all ? shown : comp ? selFaces(T, key, F, E, V) : std::vector<int>{};
    if (fs.empty()) return all ? tr("!Nothing shown to lay out") : tr("!Select polygons of the strands (their shells) — or Rectangle all");
    ui::BusyScope bz(tr("Laying out strips"));
    const int n = uv::rectangle(m, T, fs); if (!n) return tr("!Nothing to lay out");
    if (spread) uv::pack(m, T, fs, pad, false);
    return tr("%1 strand%2 laid out as strips").arg(n).arg(n == 1 ? "" : "s");
  });
}

// ---------- the transform: the selection's box typed in; the tile's edges ----------
std::optional<std::array<double, 4>> Viewport::uvSelBox() {
  const int m = topoMesh(); if (m < 0) return std::nullopt; const Item* it = scene_->item(m); if (!it || !uv::has(*baseOf(*it))) return std::nullopt;
  auto ks = uvMoving(); if (ks.empty()) return std::nullopt;
  return uv::box(*baseOf(*it), ks);
}
QString Viewport::uvSetBox(double u, double v, double w, double h) {
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(tr("Select what to place in the UV Editor (polygons, edges or UV points)")); return "!"; }
  const bool mid = uvo.boxMid;
  return uvEdit([ks, u, v, w, h, mid](PolyMesh& m, const topo::TTopo&) -> QString {
    auto b = uv::box(m, ks); const double bw = b[2] - b[0], bh = b[3] - b[1];
    const double nu0 = mid ? u - w / 2 : u, nv0 = mid ? v - h / 2 : v;
    const double ku = bw > 1e-12 ? w / bw : 1, kv = bh > 1e-12 ? h / bh : 1;   // (a line: its width stays)
    uv::transform(m, ks, [&](const uv::UV& p) { return uv::UV{float(nu0 + (p[0] - b[0]) * ku), float(nv0 + (p[1] - b[1]) * kv)}; });
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

}  // namespace hm
