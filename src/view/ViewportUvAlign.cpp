// The UV Editor's lining up: the selection's UV points (Vertex / Edge mode) or whole shells (Polygon mode) to an edge or
// the middle of the selection, shells spaced evenly, nudged by texels; Grid, a loop straightened, shells turned by an
// edge or to the 3D up, stacked / set apart, snapped together along a seam (scene/UvAlign.cpp); and the texel density —
// the selected shells' against the one their UV Set wants.
#include "scene/Uv.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <functional>
#include <cmath>
#include <map>
#include <set>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
// the shells holding any of `faces`: each one's corners
std::vector<std::vector<int>> shellCorners(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces) {
  const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P);
  std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(S.of[size_t(f)]);
  std::map<int, std::vector<int>> by; for (int f = 0; f < T.nf; f++) if (want.count(S.of[size_t(f)])) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) by[S.of[size_t(f)]].push_back(k);
  std::vector<std::vector<int>> out; for (auto& [s, ks] : by) out.push_back(std::move(ks)); return out;
}
}  // namespace

QString Viewport::uvAlign(const QString& side) {
  const int am = topoMesh(); const auto ks = uvMoving();
  if (am < 0 || ks.empty()) { ui::toast(tr("Select what to line up in the UV Editor — UV points (Vertex / Edge mode) or shells (Polygon mode)")); return "!"; }
  const bool shellsMode = tKey() == 'f'; const auto fs = tSelFaces(am);
  return uvEdit([&, side, shellsMode](PolyMesh& m, const topo::TTopo& T) -> QString {
    const auto b = uv::box(m, ks); const bool alongU = side == "left" || side == "right" || side == "hmid";
    const double target = side == "left" ? b[0] : side == "right" ? b[2] : side == "bottom" ? b[1] : side == "top" ? b[3] : side == "hmid" ? (b[0] + b[2]) / 2 : (b[1] + b[3]) / 2;
    if (!shellsMode) { uv::transform(m, ks, [&](const uv::UV& p) { return alongU ? uv::UV{float(target), p[1]} : uv::UV{p[0], float(target)}; });
      return tr("%1 UV corner%2 lined up").arg(ks.size()).arg(ks.size() == 1 ? "" : "s"); }
    const auto shs = shellCorners(m, T, fs);
    for (auto& sk : shs) { const auto s = uv::box(m, sk);
      const double cur = side == "left" ? s[0] : side == "right" ? s[2] : side == "bottom" ? s[1] : side == "top" ? s[3] : side == "hmid" ? (s[0] + s[2]) / 2 : (s[1] + s[3]) / 2;
      const double d = target - cur; uv::transform(m, sk, [&](const uv::UV& p) { return alongU ? uv::UV{float(p[0] + d), p[1]} : uv::UV{p[0], float(p[1] + d)}; }); }
    return tr("%1 shell%2 lined up (%3)").arg(shs.size()).arg(shs.size() == 1 ? "" : "s").arg(side);
  });
}

QString Viewport::uvDistribute(bool alongU) {
  const int am = topoMesh(); if (am < 0 || tKey() != 'f' || tsel_.id != am) { ui::toast(tr("Select three shells or more (Polygon mode) to space evenly")); return "!"; }
  const auto fs = tSelFaces(am);
  return uvEdit([&, alongU](PolyMesh& m, const topo::TTopo& T) -> QString {
    auto shs = shellCorners(m, T, fs); if (shs.size() < 3) return tr("!Three shells or more to space evenly");
    const int a = alongU ? 0 : 1; std::vector<std::array<double, 4>> bx; for (auto& sk : shs) bx.push_back(uv::box(m, sk));
    std::vector<size_t> ord(shs.size()); for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](size_t x, size_t y) { return bx[x][a] + bx[x][a + 2] < bx[y][a] + bx[y][a + 2]; });
    const double lo = bx[ord.front()][a], hi = bx[ord.back()][a + 2]; double sum = 0; for (auto& q : bx) sum += q[a + 2] - q[a];
    const double gap = (hi - lo - sum) / double(shs.size() - 1); double at = lo;
    for (size_t i : ord) { const double d = at - bx[i][a]; uv::transform(m, shs[i], [&](const uv::UV& p) { return alongU ? uv::UV{float(p[0] + d), p[1]} : uv::UV{p[0], float(p[1] + d)}; }); at += bx[i][a + 2] - bx[i][a] + gap; }
    return tr("%1 shells spaced evenly along %2").arg(shs.size()).arg(alongU ? "U" : "V");
  });
}

QString Viewport::uvNudge(int du, int dv) {
  const int am = topoMesh(); const auto ks = uvMoving(); if (am < 0 || ks.empty()) return "!";
  const UvTex t = uvTex(am); const double su = double(du) / t.w, sv = double(dv) / t.h;
  return uvEdit([&, su, sv](PolyMesh& m, const topo::TTopo&) -> QString {
    uv::transform(m, ks, [&](const uv::UV& p) { return uv::UV{float(p[0] + su), float(p[1] + sv)}; });
    return tr("Moved by %1, %2 px").arg(du).arg(dv);
  });
}

QString Viewport::uvGrid() {
  const int am = topoMesh(); if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons of the shells to lay as a grid")); return "!"; }
  const auto fs = tSelFaces(am); const uv::PackOpts po = uvPackOpts(am);
  return uvEdit([&](PolyMesh& m, const topo::TTopo& T) -> QString {
    ui::BusyScope bz(tr("Laying out as a grid"));
    const int n = uv::rectify(m, T, fs, &po); if (!n) return tr("!No quads to lay as a grid");
    return tr("%1 shell%2 laid as a grid — the quad strips level and upright").arg(n).arg(n == 1 ? "" : "s");
  });
}

QString Viewport::uvStraightenLoop() {
  const int am = topoMesh(); if (am < 0 || tKey() != 'e' || tsel_.id != am || tsel_.e.size() < 2) { ui::toast(tr("Select a chain of edges (Edge mode)")); return "!"; }
  const std::vector<int> es(tsel_.e.begin(), tsel_.e.end()); const uv::PackOpts po = uvPackOpts(am);
  return uvEdit([&](PolyMesh& m, const topo::TTopo& T) -> QString {
    const int n = uv::straightenChain(m, T, es, &po); if (!n) return tr("!Select one chain of edges (no branches)");
    return tr("%1 points on a line, spaced as in 3D").arg(n);
  });
}

QString Viewport::uvOrientEdge() {
  const int am = topoMesh(); if (am < 0 || tKey() != 'e' || tsel_.id != am || tsel_.e.empty()) { ui::toast(tr("Select an edge (Edge mode): its shell turns so it lies level or upright")); return "!"; }
  const int e = tsel_.last >= 0 && tsel_.e.count(tsel_.last) ? tsel_.last : *tsel_.e.begin(); const uv::PackOpts po = uvPackOpts(am);
  return uvEdit([&, e](PolyMesh& m, const topo::TTopo& T) -> QString {
    const double a = uv::orientByEdge(m, T, e, &po); return tr("The shell turned by %1° — the edge level / upright").arg(a, 0, 'f', 1);
  });
}

QString Viewport::uvOrientUpright() {
  const int am = topoMesh(); if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons of the shells to turn upright")); return "!"; }
  const auto fs = tSelFaces(am); const uv::PackOpts po = uvPackOpts(am);
  return uvEdit([&](PolyMesh& m, const topo::TTopo& T) -> QString {
    const int n = uv::orientUpright(m, T, fs, &po); if (!n) return tr("!Nothing to turn");
    return tr("%1 shell%2 turned upright — the 3D up along V").arg(n).arg(n == 1 ? "" : "s");
  });
}

QString Viewport::uvStack() {
  const int am = topoMesh(); if (am < 0 || tKey() != 'f' || tsel_.id != am || tsel_.f.empty()) { ui::toast(tr("Select polygons of the shells to stack (Polygon mode) — the last picked one is where they go")); return "!"; }
  const auto fs = tSelFaces(am); const int ref = tsel_.last >= 0 && tsel_.f.count(tsel_.last) ? tsel_.last : fs.front(); const uv::PackOpts po = uvPackOpts(am);
  return uvEdit([&, ref](PolyMesh& m, const topo::TTopo& T) -> QString {
    const int n = uv::stackOnto(m, T, fs, ref, &po); if (!n) return tr("!Select two shells or more");
    return tr("%1 shell%2 stacked over the last picked one").arg(n).arg(n == 1 ? "" : "s");
  });
}

QString Viewport::uvUnstack() {
  const int am = topoMesh(); if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons of the stacked shells")); return "!"; }
  const auto fs = tSelFaces(am); const UvTex t = uvTex(am); const double gap = t.pad / t.w;
  return uvEdit([&, gap](PolyMesh& m, const topo::TTopo& T) -> QString {
    const int n = uv::unstack(m, T, fs, gap); if (!n) return tr("!None of them lie over another");
    return tr("%1 shell%2 set apart").arg(n).arg(n == 1 ? "" : "s");
  });
}

QString Viewport::uvSnapTogether() {
  const int am = topoMesh(); if (am < 0 || tKey() != 'e' || tsel_.id != am || tsel_.e.empty()) { ui::toast(tr("Select the seam's edges between two shells (Edge mode)")); return "!"; }
  const std::vector<int> es(tsel_.e.begin(), tsel_.e.end()); const uv::PackOpts po = uvPackOpts(am);
  return uvEdit([&](PolyMesh& m, const topo::TTopo& T) -> QString {
    const int n = uv::snapTogether(m, T, es, &po); if (!n) return tr("!These edges don't join two shells — select a seam between two");
    return tr("Snapped together and sewn along %1 edge%2").arg(n).arg(n == 1 ? "" : "s");
  });
}

// ---------- the texel density ----------
double Viewport::uvDensityOfSel() {
  const int am = topoMesh(); const Item* it = am >= 0 ? scene_->item(am) : nullptr; if (!it || topoMode == "object" || tsel_.id != am) return -1;
  const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) return -1; const UvCache* c = uvCache(); if (!c) return -1;
  const auto fs = tSelFaces(am); if (fs.empty()) return -1;
  std::set<int> sh; for (int f : fs) if (f < c->T->nf) sh.insert(c->S.of[size_t(f)]);
  const UvTex t = uvTex(am); double au = 0, a3 = 0;
  for (int f = 0; f < c->T->nf; f++) if (sh.count(c->S.of[size_t(f)])) { au += std::abs(uv::faceArea(B, f)); a3 += uv::faceArea3(B, f); }
  return au > 0 && a3 > 0 ? std::sqrt(au * t.w * t.h / a3) : -1;
}
QString Viewport::uvDensityGet() {
  const int am = topoMesh(); const double d = uvDensityOfSel(); if (d <= 0) { ui::toast(tr("Select polygons of a shell — its texel density becomes the wanted one")); return "!"; }
  UvTex t = uvTex(am); t.td = d; return uvSetTex(am, t);
}
QString Viewport::uvDensitySet() {
  const int am = topoMesh(); const UvTex t = uvTex(am); const auto attrs = uvShellAttrs(am);
  if (am < 0 || t.td <= 0) { ui::toast(tr("Set the wanted texel density first (type it, or take it from a shell)")); return "!"; }
  if (topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons of the shells to bring to the wanted density")); return "!"; }
  const auto fs = tSelFaces(am);
  return uvEdit([&, t](PolyMesh& m, const topo::TTopo& T) -> QString {
    const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P); std::set<int> want; for (int f : fs) if (f < T.nf) want.insert(S.of[size_t(f)]);
    int n = 0;
    for (int s : want) { std::vector<int> ks; double au = 0, a3 = 0;
      for (int f = 0; f < T.nf; f++) if (S.of[size_t(f)] == s) { au += std::abs(uv::faceArea(m, f)); a3 += uv::faceArea3(m, f); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) ks.push_back(k); }
      int f0 = -1; for (int f = 0; f < T.nf && f0 < 0; f++) if (S.of[size_t(f)] == s) f0 = f; const double mult = f0 >= 0 && size_t(f0) < attrs.size() ? attrs[size_t(f0)].scale : 1;
      if (!(au > 0) || !(a3 > 0)) continue; const double cur = std::sqrt(au * t.w * t.h / a3), k = t.td * mult / cur;   // (a shell set to ×N: N times the density)
      const auto b = uv::box(m, ks); const double cu = (b[0] + b[2]) / 2, cv = (b[1] + b[3]) / 2;
      uv::transform(m, ks, [&](const uv::UV& p) { return uv::UV{float(cu + (p[0] - cu) * k), float(cv + (p[1] - cv) * k)}; }); n++; }
    if (!n) return tr("!Nothing to scale");
    return tr("%1 shell%2 at %3 px per cm, each in its place").arg(n).arg(n == 1 ? "" : "s").arg(t.td, 0, 'f', 2);
  });
}

}  // namespace hm

namespace hm {
// ---------- the transform frame (one tool for E / R / T): round the selection's box — its corners scale both ways (Shift:
// evenly), its sides' middles one way (the other side held); inside it or by the U / V arrows — moved; the knob over its
// top — turned (Shift: 15° steps). In screen pixels ----------
namespace {
const double HS = 5, KNOB = 26, AR = 46, AS = 7;                 // (handle half size, the knob's height over the top, the arrows' length, the middle square)
}
bool Viewport::uvGizmoOn() const {
  if (room() != "topo" || !tset.uv || cam_.view != "uv" || topoMode == "object" || uvTf_.on || uvBrush) return false;
  if (tool_ != "move" && tool_ != "rotate" && tool_ != "gizmo") return false;
  const int am = topoMesh(); return am >= 0 && tsel_.id == am && !const_cast<Viewport*>(this)->uvMoving().empty();
}
std::array<double, 2> Viewport::uvGizmoPivot() const {
  const auto b = const_cast<Viewport*>(this)->uvSelBox(); if (!b) return {0.5, 0.5};
  return {((*b)[0] + (*b)[2]) / 2, ((*b)[1] + (*b)[3]) / 2};
}
namespace {
struct FrameGeo { double x0, x1, y0, y1, cx, cy; };              // (screen: x0 left, x1 right, y0 top, y1 bottom)
}
static FrameGeo frameGeo(const Viewport& vp, const std::array<double, 4>& b, const std::function<QPointF(double, double)>& toPx) {
  const QPointF a = toPx(b[0], b[3]), c = toPx(b[2], b[1]); FrameGeo g{a.x(), c.x(), a.y(), c.y(), (a.x() + c.x()) / 2, (a.y() + c.y()) / 2};
  if (g.x1 - g.x0 < 16) { g.x0 = g.cx - 8; g.x1 = g.cx + 8; } if (g.y1 - g.y0 < 16) { g.y0 = g.cy - 8; g.y1 = g.cy + 8; }   // (a line, a point: a frame still)
  (void)vp; return g;
}
// 0 none; 1 / 2 the U / V arrow; 3 the middle square; 4 the knob; 5 inside; 10…13 the corners (left-bottom, right-bottom,
// right-top, left-top); 20…23 the sides (left, right, bottom, top)
int Viewport::uvGizmoHit(const QPointF& mp) const {
  const auto b = const_cast<Viewport*>(this)->uvSelBox(); if (!b) return 0;
  const FrameGeo g = frameGeo(*this, *b, [this](double u, double v) { return uvToPx(u, v); }); const double x = mp.x(), y = mp.y(), r = HS + 3;
  if (std::hypot(x - g.cx, y - (g.y0 - KNOB)) <= 8) return 4;
  const QPointF cs[4] = {{g.x0, g.y1}, {g.x1, g.y1}, {g.x1, g.y0}, {g.x0, g.y0}};
  for (int i = 0; i < 4; i++) if (std::abs(x - cs[i].x()) <= r && std::abs(y - cs[i].y()) <= r) return 10 + i;
  const QPointF sd[4] = {{g.x0, g.cy}, {g.x1, g.cy}, {g.cx, g.y1}, {g.cx, g.y0}};
  for (int i = 0; i < 4; i++) if (std::abs(x - sd[i].x()) <= r && std::abs(y - sd[i].y()) <= r) return 20 + i;
  if (std::abs(x - g.cx) <= AS + 2 && std::abs(y - g.cy) <= AS + 2) return 3;
  if (x - g.cx > AS && x - g.cx < AR + 10 && std::abs(y - g.cy) < 6) return 1;
  if (g.cy - y > AS && g.cy - y < AR + 10 && std::abs(x - g.cx) < 6) return 2;
  if (x > g.x0 && x < g.x1 && y > g.y0 && y < g.y1) return 5;
  return 0;
}
void Viewport::uvGizmoOverlay(std::vector<Overlay>& out) const {
  if (!uvGizmoOn()) return;
  const auto b = const_cast<Viewport*>(this)->uvSelBox(); if (!b) return;
  const FrameGeo g = frameGeo(*this, *b, [this](double u, double v) { return uvToPx(u, v); });
  const QColor frame(0xe8, 0xe8, 0xf0), red(0xf0, 0x4a, 0x4a), green(0x5a, 0xd0, 0x5a), yellow(0xf2, 0xd2, 0x3c), blue(0x4c, 0x9c, 0xf0), dark(0x14, 0x14, 0x18);
  auto rect = [&](double x0, double y0, double x1, double y1, const QColor& col) { out.push_back({{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, col, true}); };
  auto box = [&](double cx, double cy, double r, const QColor& col) { for (double q = r; q > 0.5; q -= 1.0) rect(cx - q, cy - q, cx + q, cy + q, col); rect(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, dark); };
  rect(g.x0 - 1, g.y0 - 1, g.x1 + 1, g.y1 + 1, dark); rect(g.x0, g.y0, g.x1, g.y1, frame);                     // (the frame)
  out.push_back({{{g.cx, g.y0}, {g.cx, g.y0 - KNOB + 6}}, frame, false});                                      // (the knob's stem)
  { std::vector<std::pair<double, double>> P; for (int i = 0; i < 24; i++) { const double a = 2 * M_PI * i / 24; P.push_back({g.cx + 6 * std::cos(a), g.y0 - KNOB + 6 * std::sin(a)}); }
    out.push_back({P, blue, true}); for (double rr : {5.0, 4.0, 3.0, 2.0}) { std::vector<std::pair<double, double>> Q; for (int i = 0; i < 24; i++) { const double a = 2 * M_PI * i / 24; Q.push_back({g.cx + rr * std::cos(a), g.y0 - KNOB + rr * std::sin(a)}); } out.push_back({Q, blue, true}); } }
  for (auto [x, y] : {std::pair<double, double>{g.x0, g.y1}, {g.x1, g.y1}, {g.x1, g.y0}, {g.x0, g.y0}}) box(x, y, HS, frame);   // (corners: both ways)
  for (auto [x, y] : {std::pair<double, double>{g.x0, g.cy}, {g.x1, g.cy}, {g.cx, g.y1}, {g.cx, g.y0}}) box(x, y, HS - 1, frame);   // (sides: one way)
  out.push_back({{{g.cx + AS, g.cy}, {g.cx + AR, g.cy}}, red, false}); out.push_back({{{g.cx, g.cy - AS}, {g.cx, g.cy - AR}}, green, false});   // (the arrows)
  for (double k = 0; k <= 6; k += 1) { out.push_back({{{g.cx + AR + 9, g.cy}, {g.cx + AR, g.cy - k}}, red, false}); out.push_back({{{g.cx + AR + 9, g.cy}, {g.cx + AR, g.cy + k}}, red, false});
    out.push_back({{{g.cx, g.cy - AR - 9}, {g.cx - k, g.cy - AR}}, green, false}); out.push_back({{{g.cx, g.cy - AR - 9}, {g.cx + k, g.cy - AR}}, green, false}); }
  box(g.cx, g.cy, AS - 2, yellow);
}
}  // namespace hm

namespace hm {
// ---------- the checks ----------
const uv::CheckReport& Viewport::uvCheck() {
  static const uv::CheckReport none; const int am = topoMesh(); const Item* it = am >= 0 ? scene_->item(am) : nullptr; if (!it) return none;
  const PolyMesh* B = baseOf(*it); if (!uv::has(*B)) return none;
  const UvTex t = uvTex(am); const QByteArray sig = QByteArray::number(am) + "|" + QByteArray::number(t.w) + "x" + QByteArray::number(t.h) + "p" + QByteArray::number(t.pad) + "d" + QByteArray::number(t.td)
                                                    + "|" + QByteArray::number(uvPicked.set) + "," + QByteArray::number(uvPicked.mat);
  if (uvCheck_.B == B && uvCheck_.rev == uvStamp_ && uvCheck_.sig == sig) return uvCheck_.r;
  uvCheck_ = {}; uvCheck_.B = B; uvCheck_.rev = uvStamp_; uvCheck_.sig = sig;
  const UvCache* c = uvCache(); if (!c) return uvCheck_.r;
  const auto mc = uv::mirrorCorners(*B, *c->T); uv::CheckOpts o; o.w = t.w; o.h = t.h; o.pad = t.pad; o.td = t.td; o.mc = &mc;
  for (auto& g : uvMatGroups(am)) {                               // (each UV Mat its own tile: checked apart)
    const auto r = uv::check(*B, *c->T, g, o); auto& R = uvCheck_.r;
    for (auto [to, from] : {std::pair<std::vector<int>*, const std::vector<int>*>{&R.flipped, &r.flipped}, {&R.overlap, &r.overlap}, {&R.outside, &r.outside}, {&R.padding, &r.padding}, {&R.density, &r.density}, {&R.asym, &r.asym}})
      to->insert(to->end(), from->begin(), from->end());
    R.stacks += r.stacks; R.tdRef = r.tdRef;
  }
  return uvCheck_.r;
}
QString Viewport::uvCheckSelect(const QString& kind) {
  const int am = topoMesh(); if (am < 0) return "!"; const auto& r = uvCheck();
  const std::vector<int>& fs = kind == "flipped" ? r.flipped : kind == "overlap" ? r.overlap : kind == "outside" ? r.outside : kind == "padding" ? r.padding : kind == "density" ? r.density : r.asym;
  if (fs.empty()) { ui::toast(tr("Nothing of that kind")); return "!"; }
  if (topoMode != "poly") setTopoMode("poly");
  const int n = tSelectFaces(am, fs); ui::hud(tr("%1 polygon%2 selected").arg(n).arg(n == 1 ? "" : "s"), 1200); return {};
}
}  // namespace hm

namespace hm {
// ---------- each shell's own settings ----------
namespace {
QString polyKey(const topo::TTopo& T, int f) { std::vector<int> v(T.fv.begin() + T.fo[size_t(f)], T.fv.begin() + T.fo[size_t(f) + 1]); std::sort(v.begin(), v.end()); QStringList l; for (int x : v) l << QString::number(x); return l.join(','); }
}
std::vector<uv::ShellAttr> Viewport::uvShellAttrs(int m) const {
  const Item* it = scene_->item(m); if (!it) return {}; const PolyMesh* B = baseOf(*it); const auto T = topo::ttopo(it->base ? it->base : it->poly);
  std::vector<uv::ShellAttr> out(size_t(B->faceCount())); const QJsonArray arr = it->raw.value("uvShellAttr").toArray(); if (arr.isEmpty() || !T) return out;
  std::map<QString, uv::ShellAttr> byKey;
  for (const QJsonValue& x : arr) { const QJsonObject o = x.toObject(); QStringList l; for (const QJsonValue& v : o.value("k").toArray()) l << QString::number(v.toInt());
    uv::ShellAttr a; a.lock = o.value("lock").toBool(); a.noTurn = o.value("noTurn").toBool(); a.scale = o.value("scale").toDouble(1); a.group = o.value("group").toInt(); byKey[l.join(',')] = a; }
  const uv::Points P = uv::points(*B, *T); const uv::Shells S = uv::shells(*B, *T, P); std::map<int, uv::ShellAttr> bySh;
  for (int f = 0; f < T->nf; f++) { auto g = byKey.find(polyKey(*T, f)); if (g != byKey.end()) bySh[S.of[size_t(f)]] = g->second; }
  for (int f = 0; f < T->nf; f++) { auto g = bySh.find(S.of[size_t(f)]); if (g != bySh.end()) out[size_t(f)] = g->second; }
  return out;
}
QString Viewport::uvShellSet(const QString& what, double v) {
  const int am = topoMesh(); Item* it = am >= 0 ? scene_->item(am) : nullptr;
  if (!it || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons of the shells (a polygon of each is enough)")); return "!"; }
  const auto fs = tSelFaces(am); const UvCache* c = uvCache(); if (fs.empty() || !c) { ui::toast(tr("Select polygons of the shells")); return "!"; }
  const topo::TTopo& T = *c->T; auto attrs = uvShellAttrs(am);
  std::set<int> sh; for (int f : fs) if (f < T.nf) sh.insert(c->S.of[size_t(f)]);
  std::map<int, int> anchor; std::set<QString> drop;                 // (each shell: its first polygon the key; any key of it dropped)
  for (int f = 0; f < T.nf; f++) { const int s = c->S.of[size_t(f)]; if (!sh.count(s)) continue; if (!anchor.count(s)) anchor[s] = f; drop.insert(polyKey(T, f)); }
  bool anyOff = false; for (auto& [s, f] : anchor) { const auto& a = attrs[size_t(f)]; if (what == "lock" ? !a.lock : what == "noturn" ? !a.noTurn : false) anyOff = true; }
  int nextGroup = 1; for (auto& a : attrs) nextGroup = std::max(nextGroup, a.group + 1);
  QJsonArray arr; for (const QJsonValue& x : it->raw.value("uvShellAttr").toArray()) { QStringList l; for (const QJsonValue& q : x.toObject().value("k").toArray()) l << QString::number(q.toInt()); if (!drop.count(l.join(','))) arr.append(x); }
  for (auto& [s, f] : anchor) {
    uv::ShellAttr a = attrs[size_t(f)];
    if (what == "lock") a.lock = anyOff; else if (what == "noturn") a.noTurn = anyOff; else if (what == "scale") a.scale = std::clamp(v, 0.05, 20.0);
    else if (what == "group") a.group = nextGroup; else if (what == "ungroup") a.group = 0; else if (what == "clear") a = {};
    if (!a.lock && !a.noTurn && a.scale == 1 && a.group == 0) continue;
    QJsonArray k; std::vector<int> vs(T.fv.begin() + T.fo[size_t(f)], T.fv.begin() + T.fo[size_t(f) + 1]); std::sort(vs.begin(), vs.end()); for (int x : vs) k.append(x);
    arr.append(QJsonObject{{"k", k}, {"lock", a.lock}, {"noTurn", a.noTurn}, {"scale", a.scale}, {"group", a.group}});
  }
  if (arr.isEmpty()) it->raw.remove("uvShellAttr"); else it->raw.insert("uvShellAttr", arr);
  const int n = int(anchor.size()); const QString pl = n == 1 ? "" : "s";
  const QString msg = what == "lock" ? (anyOff ? tr("%1 shell%2 locked — Pack and Unfold all leave them, Pack packs round them") : tr("%1 shell%2 unlocked")).arg(n).arg(pl)
                    : what == "noturn" ? (anyOff ? tr("%1 shell%2 — Pack won't turn them") : tr("%1 shell%2 — Pack may turn them again")).arg(n).arg(pl)
                    : what == "scale" ? tr("%1 shell%2 at ×%3 the texel density of the rest (Pack, Set)").arg(n).arg(pl).arg(v, 0, 'f', 2)
                    : what == "group" ? tr("%1 shell%2 — group %3: Pack lays them together").arg(n).arg(pl).arg(nextGroup)
                    : what == "ungroup" ? tr("%1 shell%2 out of their groups").arg(n).arg(pl) : tr("%1 shell%2: their own settings cleared").arg(n).arg(pl);
  stepLabel_ = msg; scene_->revision++; commit(false); uvChanged(); emit topoState(); ui::toast(msg); return msg;
}
}  // namespace hm
