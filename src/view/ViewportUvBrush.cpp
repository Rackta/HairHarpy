// The UV Editor's brushes (stage 9): Relax — SLIM on the UV points under the circle, the rest of their shells held (the
// stretch evened out where it goes), the shell's border held (Shift: it too), no polygon turned over, the
// pins held; Pin — the UV points under it pinned (Ctrl: unpinned): Unfold lays the shell round them, Optimize keeps
// them. The circle: the brush's radius in screen pixels; its strength the Relax's step
#include "scene/Uv.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace hm {

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
}

void Viewport::setUvBrush(char kind) {
  uvBrush = uvBrush == kind ? 0 : kind;                       // (the same again — off)
  if (uvBrush && uvTf_.on) uvTfStop();
  overlayRev_++; requestUpdate(); emit topoState();
  ui::hud(uvBrush == 'r' ? tr("Relax brush: drag over the UV — Shift: the border too; [ ] the size") : uvBrush == 'p' ? tr("Pin brush: drag over the UV points — Ctrl: unpin") : tr("Brush off"), 1400);
}

void Viewport::uvBrushOverlay(std::vector<Overlay>& out) const {
  if (!uvBrush || cam_.view != "uv" || uvBrushAt_.x() < 0) return;
  const QColor col = uvBrush == 'r' ? QColor(0x6a, 0xd0, 0xff) : QColor(0xff, 0xe0, 0x3a);
  for (double r : {uvBrushR, uvBrushR * 0.5}) { Overlay o; o.color = r == uvBrushR ? col : QColor(col.red(), col.green(), col.blue(), 110); o.closed = true;
    for (int i = 0; i < 48; i++) { const double a = 2 * M_PI * i / 48; o.pts.push_back({uvBrushAt_.x() + r * std::cos(a), uvBrushAt_.y() + r * std::sin(a)}); }
    out.push_back(std::move(o)); }
}

bool Viewport::uvBrushPress(const QPointF& mp, bool ctrl) {
  const int am = topoMesh(); Item* it = am >= 0 ? scene_->item(am) : nullptr; const UvCache* c = uvCache();
  if (!it || topoMode == "object" || !c) return false;
  const PolyMesh& B = *baseSp(*it); if (!uv::has(B)) { ui::toast(tr("This mesh has no UV — Unfold first")); return false; }
  const topo::TTopo& T = *c->T; const int n = c->P.count(); const std::vector<char> hid = uvHidden(*it, B);
  UvBrushS& S = uvBr_; S = {}; S.pf.assign(size_t(n), {}); S.border.assign(size_t(n), 0); S.fixed.assign(size_t(n), 0); S.vis.assign(size_t(n), 0);
  S.showF.assign(size_t(T.nf), 0);
  for (int f = 0; f < T.nf; f++) { const bool shown = hid.empty() || size_t(f) >= hid.size() || !hid[size_t(f)];
    S.showF[size_t(f)] = shown;
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)], q = c->P.of[size_t(k)], w = c->P.of[size_t(nk)];
      S.pf[size_t(q)].push_back(f); if (shown) S.vis[size_t(q)] = 1;
      if (!c->cont[size_t(T.ce[size_t(k)])]) S.border[size_t(q)] = S.border[size_t(w)] = 1; } }
  const uv::Pins pins = uvPinsOf(am);
  for (int q = 0; q < n; q++) { const int v = c->P.vert[size_t(q)]; if (pins.count({v, v})) S.fixed[size_t(q)] = 1; }
  for (const auto& [a, b] : pins) if (a != b) for (int q = 0; q < n; q++) { const int v = c->P.vert[size_t(q)]; if (v == a || v == b) S.fixed[size_t(q)] = 1; }
  { std::set<int> fv; for (const auto& ft : uvFeaturesOf(am)) { fv.insert(ft.a.begin(), ft.a.end()); fv.insert(ft.b.begin(), ft.b.end()); }   // (the lips' and lids' rims: held — their pairs kept)
    if (!fv.empty()) for (int q = 0; q < n; q++) if (fv.count(c->P.vert[size_t(q)])) S.fixed[size_t(q)] = 1; }
  S.U = B.cornerUV; S.po = uvPackOpts(am); if (uvBrush == 'r') S.W = std::make_shared<PolyMesh>(B);
  uvd_.m = am; uvd_.uv0 = B.cornerUV; uvd_.mode = ctrl ? "remove" : "add"; uvd_.op = uvBrush;
  uvBrushStroke(mp, ctrl ? Qt::ControlModifier : Qt::NoModifier);
  return true;
}

void Viewport::uvBrushStroke(const QPointF& mp, Qt::KeyboardModifiers mods) {
  uvBrushAt_ = mp; overlayRev_++;
  const UvCache* c = uvCache(); Item* it = scene_->item(uvd_.m); if (!c || !it) return;
  const PolyMesh& B = *baseSp(*it); const topo::TTopo& T = *c->T; UvBrushS& S = uvBr_; const int n = c->P.count();
  if (int(S.pf.size()) != n || S.U.size() != B.cornerUV.size()) return;
  const double R = uvBrushR, R2 = R * R;
  if (std::hypot(mp.x() - S.last.x(), mp.y() - S.last.y()) < std::max(2.0, R * 0.2)) return;   // (a dab every fifth of the radius)
  S.last = mp;
  auto at = [&](int q) { return S.U[size_t(c->P.list[size_t(c->P.off[size_t(q)])])]; };
  std::vector<int> in;                                         // (the shown points under the circle)
  for (int q = 0; q < n; q++) { if (!S.vis[size_t(q)]) continue; const auto u = at(q); const QPointF p = uvToPx(u[0], u[1]);
    if ((p.x() - mp.x()) * (p.x() - mp.x()) + (p.y() - mp.y()) * (p.y() - mp.y()) < R2) in.push_back(q); }
  if (uvd_.op == 'p') {                                         // (the pins: the points under it)
    const bool add = uvd_.mode != "remove"; QJsonArray arr = it->raw.value("uvPins").toArray(); std::set<int> had;
    for (const QJsonValue& x : arr) { const QJsonArray a = x.toArray(); if (a.size() >= 2 && a[0].toInt() == a[1].toInt()) had.insert(a[0].toInt()); }
    std::set<int> vs; for (int q : in) vs.insert(c->P.vert[size_t(q)]);
    bool changed = false;
    if (add) { for (int v : vs) if (!had.count(v)) { arr.append(QJsonArray{v, v, "p"}); changed = true; S.touched++; } }
    else { QJsonArray keep; for (const QJsonValue& x : arr) { const QJsonArray a = x.toArray(); if (a.size() >= 2 && a[0].toInt() == a[1].toInt() && vs.count(a[0].toInt())) { changed = true; S.touched++; continue; } keep.append(x); } arr = keep; }
    if (changed) { if (arr.isEmpty()) it->raw.remove("uvPins"); else it->raw.insert("uvPins", arr); uvRev_++; }
    requestUpdate(); return;
  }
  // Relax: SLIM on the shells under it — the points under the circle free (the border only with Shift), every other
  // point of those shells held: the stretch there evened out, nothing turned over, the rest not moved
  const bool rim = mods & Qt::ShiftModifier; std::set<int> freeV; std::set<int> shells;
  for (int q : in) { if (S.fixed[size_t(q)] || (S.border[size_t(q)] && !rim)) continue; freeV.insert(c->P.vert[size_t(q)]); for (int f : S.pf[size_t(q)]) shells.insert(c->S.of[size_t(f)]); }
  if (freeV.empty() || !S.W) return;
  std::vector<int> fs; uv::Pins hold;
  for (int f = 0; f < T.nf; f++) if (shells.count(c->S.of[size_t(f)])) { fs.push_back(f);
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int v = T.fv[size_t(k)]; if (!freeV.count(v)) hold.insert({v, v}); } }
  S.W->cornerUV = S.U; const int iters = 2 + int(std::lround(std::clamp(uvBrushK, 0.1, 1.0) * 10));
  // (the border let go: no new overlap allowed — the shells' overlaps counted before and after; more — that dab again
  // with the border held)
  uv::CheckOpts co; co.w = S.po.w; co.h = S.po.h; co.pad = 0;
  auto overlaps = [&](const PolyMesh& w) { return uv::check(w, T, fs, co).overlap.size(); };
  size_t ov0 = 0; if (rim) { PolyMesh& w = *S.W; w.cornerUV = S.U; ov0 = overlaps(w); }
  int done = uv::optimize(*S.W, T, fs, &hold, iters, &S.po, nullptr);
  if (done > 0 && rim && overlaps(*S.W) > ov0) {
    for (int q : in) if (S.border[size_t(q)]) { const int v = c->P.vert[size_t(q)]; freeV.erase(v); }
    for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int v = T.fv[size_t(k)]; if (!freeV.count(v)) hold.insert({v, v}); }
    S.W->cornerUV = S.U; done = freeV.empty() ? 0 : uv::optimize(*S.W, T, fs, &hold, iters, &S.po, nullptr); }
  if (done > 0) {
    int moved = 0; for (size_t k = 0; k < S.U.size(); k++) if (S.U[k] != S.W->cornerUV[k]) moved++;
    S.U = S.W->cornerUV; S.touched += moved; uvLive(uvd_.m, S.U); }
}

void Viewport::uvBrushRelease(const UvDragS& d) {
  Item* it = scene_->item(d.m); const int n = uvBr_.touched; uvBr_ = {}; if (!it) return;
  if (d.op == 'p') { if (n) { stepLabel_ = d.mode == "remove" ? tr("UV pins off (brush)") : tr("UV points pinned (brush)"); scene_->revision++; commit(false); uvChanged(); emit topoState(); } return; }
  if (!n) return;
  stepLabel_ = tr("UV relaxed (brush)"); topoWrite(d.m, baseSp(*it)); uvStamp_++; uvRev_++; commit(false); emit topoState();
}

}  // namespace hm

namespace hm {
// (tests: a stroke of the brush from UV a to UV b — the stretch of what it touched before / after, the pins)
QString Viewport::uvBrushTest(char kind, double u0, double v0, double u1, double v1, bool mod) {
  const char was = uvBrush; uvBrush = kind; const QPointF a = uvToPx(u0, v0), b = uvToPx(u1, v1);
  const int am = topoMesh(); const Item* it = am >= 0 ? scene_->item(am) : nullptr; if (!it) return "!no mesh";
  auto stat = [&] { const PolyMesh& B = *(it->base ? it->base : it->poly); auto T = topo::ttopo(it->base ? it->base : it->poly); const auto s = uv::angleStretch(B, *T);
    int over = 0, flip = 0; for (int f = 0; f < T->nf; f++) { if (s[size_t(f)] > 1.5f) over++; if (uv::faceArea(B, f) < 0) flip++; } return QString("over1.5 %1 turned %2 pins %3").arg(over).arg(flip).arg(it->raw.value("uvPins").toArray().size()); };
  const QString before = stat();
  uvd_ = {}; if (!uvBrushPress(a, mod)) { uvBrush = was; return "!press"; }
  for (int i = 1; i <= 24; i++) { const double t = i / 24.0; uvBrushStroke(a + (b - a) * t, mod ? (kind == 'p' ? Qt::ControlModifier : Qt::ShiftModifier) : Qt::NoModifier); }
  const UvDragS d = uvd_; const int n = uvBr_.touched; uvd_ = {}; uvBrushRelease(d); uvBrush = was;
  return QString("brush %1: touched %2 · before %3 · after %4").arg(kind).arg(n).arg(before, stat());
}
}  // namespace hm
