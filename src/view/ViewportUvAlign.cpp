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
#include <unordered_map>

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
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvAlign(side); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
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
  const std::vector<int> es(tsel_.e.begin(), tsel_.e.end()); const uv::PackOpts po = uvPackOpts(am); int sideF = -1; if (!tsel_.eSide.empty() && !tsel_.eSide.begin()->second.empty()) sideF = *tsel_.eSide.begin()->second.begin();   // (the shell the chain was picked on)
  return uvEdit([&](PolyMesh& m, const topo::TTopo& T) -> QString {
    const int n = uv::straightenChain(m, T, es, &po, sideF); if (!n) return tr("!Select one chain of edges (no branches)");
    return tr("%1 points on a line, spaced as in 3D").arg(n);
  });
}

QString Viewport::uvOrientEdge() {
  const int am = topoMesh(); if (am < 0 || tKey() != 'e' || tsel_.id != am || tsel_.e.empty()) { ui::toast(tr("Select an edge (Edge mode): its shell turns so it lies level or upright")); return "!"; }
  const int e = tsel_.last >= 0 && tsel_.e.count(tsel_.last) ? tsel_.last : *tsel_.e.begin(); const uv::PackOpts po = uvPackOpts(am); int sideF = -1; if (const auto g = tsel_.eSide.find(e); g != tsel_.eSide.end() && !g->second.empty()) sideF = *g->second.begin();
  return uvEdit([&, e, sideF](PolyMesh& m, const topo::TTopo& T) -> QString {
    const double a = uv::orientByEdge(m, T, e, &po, sideF); return tr("The shell turned by %1° — the edge level / upright").arg(a, 0, 'f', 1);
  });
}

QString Viewport::uvOrientUpright() {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvOrientUpright(); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
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
  if (room() != "topo" || !tset.uv || uvTiles || cam_.view != "uv" || topoMode == "object" || uvTf_.on || uvBrush) return false;
  if (tool_ != "move" && tool_ != "rotate" && tool_ != "gizmo") return false;
  const int am = topoMesh(); if (am < 0) return false;
  if (tsel_.id == am && !const_cast<Viewport*>(this)->uvMoving().empty()) return true;
  return const_cast<Viewport*>(this)->uvSelBox().has_value();   // (picked on another of the selected meshes only — the feet of Legs while Head is the active one: the frame still, round that, 2026-11-10)
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
  if (drag_ == Drag::Uv && uvd_.type == 4 && uvd_.moved && uvCheck_.sig == sig) return uvCheck_.r;   // (a drag moves shells whole: the report is the press's until the release)
  const int uvH = int(qHashBits(B->cornerUV.data(), B->cornerUV.size() * sizeof(B->cornerUV[0])));
  if (uvCheck_.rev == uvH && uvCheck_.sig == sig) return uvCheck_.r;   // (by what the UV is: switching the active mesh back and forth does not change it)
  uvCheck_ = {}; uvCheck_.B = B; uvCheck_.rev = uvH; uvCheck_.sig = sig;
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
  std::vector<uv::ShellAttr> out(size_t(B->faceCount())); if (!T) return out; const QJsonArray arr = it->raw.value("uvShellAttr").toArray();
  std::vector<int> lvl; for (const auto& k : uvPinsOf(m)) if (k.first < 0 && k.second >= uv::kAlignH && -1 - k.first < T->nf) lvl.push_back(-1 - k.first);   // (a shell with h / v constraints: level — Pack turns it by 90° at most, never slanted)
  if (arr.isEmpty() && lvl.empty()) return out;
  auto levelOn = [&](const uv::Shells& S) { std::set<int> ls; for (int f : lvl) ls.insert(S.of[size_t(f)]); for (int f = 0; f < T->nf; f++) if (ls.count(S.of[size_t(f)])) out[size_t(f)].level = true; };
  if (arr.isEmpty()) { const uv::Points P = uv::points(*B, *T); levelOn(uv::shells(*B, *T, P)); return out; }
  // (each setting goes to ONE shell: the one holding most of its key polygons — the anchor and the spread ones ("ks"). A shell cut in two: the
  //  setting stays with the part that has most of it, never with a small piece cut off because the anchor fell in it. With the anchor alone
  //  (older settings), the shell that holds it. Pieces of 2–3 polygons cut off a grouped shell took its group, the shell lost it, 2026-11-11)
  const uv::Points P = uv::points(*B, *T); const uv::Shells S = uv::shells(*B, *T, P); std::map<int, uv::ShellAttr> bySh;
  std::unordered_map<QString, int> faceOf; for (int f = 0; f < T->nf; f++) faceOf.emplace(polyKey(*T, f), f);
  auto keyStr = [](const QJsonValue& v) { QStringList l; for (const QJsonValue& q : v.toArray()) l << QString::number(q.toInt()); return l.join(','); };
  for (const QJsonValue& x : arr) { const QJsonObject o = x.toObject();
    uv::ShellAttr a; a.lock = o.value("lock").toBool(); a.noTurn = o.value("noTurn").toBool(); a.sym = o.value("sym").toBool(); a.scale = o.value("scale").toDouble(1); a.group = o.value("group").toInt(); a.ovl = o.value("ovl").toBool(); a.turn = o.value("turn").toInt(); a.stack = o.value("stack").toInt();
    if (a.ovl && !o.contains("stack")) { a.stack = a.group; a.group = 0; a.ovl = false; }   // (an older Overlap group: an Overlap stack now — apart from the pack groups)
    int anchorSh = -1; if (auto g = faceOf.find(keyStr(o.value("k"))); g != faceOf.end()) anchorSh = S.of[size_t(g->second)];
    std::map<int, int> votes; if (anchorSh >= 0) votes[anchorSh]++;
    for (const QJsonValue& kv : o.value("ks").toArray()) if (auto g = faceOf.find(keyStr(kv)); g != faceOf.end()) votes[S.of[size_t(g->second)]]++;
    int best = -1, bv = 0; for (auto& [s, v] : votes) if (v > bv || (v == bv && s == anchorSh)) { best = s; bv = v; }
    if (best >= 0) bySh[best] = a; }
  for (int f = 0; f < T->nf; f++) { auto g = bySh.find(S.of[size_t(f)]); if (g != bySh.end()) out[size_t(f)] = g->second; }
  levelOn(S); return out;
}
// a mesh's shell settings saved with their anchor polygon alone (before 2026-11-11): given the spread polygons of the shell they hold NOW —
// before an edit cuts it, so a cut leaves the setting with the bigger part, not with the piece the anchor falls in
void Viewport::uvAttrUpgrade(int m) {
  Item* it = scene_->item(m); if (!it) return; QJsonArray arr = it->raw.value("uvShellAttr").toArray(); if (arr.isEmpty()) return;
  bool need = false; for (const QJsonValue& x : arr) if (!x.toObject().contains("ks")) { need = true; break; } if (!need) return;
  const PolyMesh* B = baseOf(*it); const auto T = topo::ttopo(it->base ? it->base : it->poly); if (!B || !T || !uv::has(*B)) return;
  const uv::Points P = uv::points(*B, *T); const uv::Shells S = uv::shells(*B, *T, P);
  std::unordered_map<QString, int> faceOf; for (int f = 0; f < T->nf; f++) faceOf.emplace(polyKey(*T, f), f);
  std::vector<std::vector<int>> byS(static_cast<size_t>(S.count)); for (int f = 0; f < T->nf; f++) byS[size_t(S.of[size_t(f)])].push_back(f);
  auto keyOf = [&](int g) { QJsonArray k; std::vector<int> vs(T->fv.begin() + T->fo[size_t(g)], T->fv.begin() + T->fo[size_t(g) + 1]); std::sort(vs.begin(), vs.end()); for (int x : vs) k.append(x); return k; };
  for (int i = 0; i < arr.size(); i++) { QJsonObject o = arr[i].toObject(); if (o.contains("ks")) continue;
    QStringList l; for (const QJsonValue& q : o.value("k").toArray()) l << QString::number(q.toInt()); auto g = faceOf.find(l.join(',')); if (g == faceOf.end()) continue;
    const auto& mine = byS[size_t(S.of[size_t(g->second)])]; QJsonArray ks; const size_t nk = std::min<size_t>(24, mine.size()); for (size_t j = 0; j < nk; j++) ks.append(keyOf(mine[j * mine.size() / nk]));
    o.insert("ks", ks); arr[i] = o; }
  it->raw.insert("uvShellAttr", arr);
}
QString Viewport::uvShellSet(const QString& what, double v) {
  if (!eachBusy_ && uvMulti()) return tEachMesh([&] { return uvShellSet(what, v); }, true);   // (several objects picked: the same done on each — its own selection, or the whole of it)
  const int am = topoMesh(); Item* it = am >= 0 ? scene_->item(am) : nullptr;
  if (!it || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons of the shells (a polygon of each is enough)")); return "!"; }
  const auto fs = tSelFaces(am); const UvCache* c = uvCache(); if (fs.empty() || !c) { ui::toast(tr("Select polygons of the shells")); return "!"; }
  const topo::TTopo& T = *c->T; auto attrs = uvShellAttrs(am);
  std::set<int> sh; for (int f : fs) if (f < T.nf) sh.insert(c->S.of[size_t(f)]);
  if (tset.sym && uvMode()) for (int f : tSymList(am, fs, 'f')) if (f < T.nf) sh.insert(c->S.of[size_t(f)]);   // (Symmetry: the mirror shells get the same setting)
  std::map<int, int> anchor; std::set<QString> drop;                 // (each shell: its first polygon the key; any key of it dropped)
  for (int f = 0; f < T.nf; f++) { const int s = c->S.of[size_t(f)]; if (!sh.count(s)) continue; if (!anchor.count(s)) anchor[s] = f; drop.insert(polyKey(T, f)); }
  bool anyOff = false; for (auto& [s, f] : anchor) { const auto& a = attrs[size_t(f)]; if (what == "lock" ? !a.lock : what == "noturn" ? !a.noTurn : what == "sym" ? !a.sym : what == "free" ? a.turn != 4 : what == "rot90" ? a.turn != 2 : what == "rot180" ? a.turn != 3 : false) anyOff = true; }
  int nextGroup = 1, nextStack = 1; for (auto& a : attrs) { nextGroup = std::max(nextGroup, a.group + 1); nextStack = std::max(nextStack, a.stack + 1); }
  // (Ungroup: out of their pack groups; none of them in one — out of their Overlap stacks)
  bool anyGroup = false; for (auto& [s, f] : anchor) if (attrs[size_t(f)].group > 0) anyGroup = true;
  std::unordered_map<QString, int> faceOf; for (int g = 0; g < T.nf; g++) faceOf.emplace(polyKey(T, g), g);
  auto keyStr = [](const QJsonValue& v) { QStringList l; for (const QJsonValue& q : v.toArray()) l << QString::number(q.toInt()); return l.join(','); };
  QJsonArray arr; for (const QJsonValue& x : it->raw.value("uvShellAttr").toArray()) { const QJsonObject o = x.toObject(); const QString k0 = keyStr(o.value("k")); if (drop.count(k0)) continue;
    std::map<int, int> votes; int anchorSh = -1; if (auto g = faceOf.find(k0); g != faceOf.end()) { anchorSh = c->S.of[size_t(g->second)]; votes[anchorSh]++; }   // (a setting whose shell — by most of its polygons — is one of these: replaced too)
    for (const QJsonValue& kv : o.value("ks").toArray()) if (auto g = faceOf.find(keyStr(kv)); g != faceOf.end()) votes[c->S.of[size_t(g->second)]]++;
    int best = -1, bv = 0; for (auto& [s2, v] : votes) if (v > bv || (v == bv && s2 == anchorSh)) { best = s2; bv = v; }
    if (best >= 0 && sh.count(best)) continue; arr.append(x); }
  for (auto& [s, f] : anchor) {
    uv::ShellAttr a = attrs[size_t(f)];
    if (what == "lock") a.lock = anyOff; else if (what == "sym") a.sym = anyOff; else if (what == "free") { a.turn = anyOff ? 4 : 0; if (anyOff) a.noTurn = false; } else if (what == "noturn") { a.noTurn = anyOff; if (anyOff) a.turn = 0; } else if (what == "rot90") { a.turn = anyOff ? 2 : 0; if (anyOff) a.noTurn = false; } else if (what == "rot180") { a.turn = anyOff ? 3 : 0; if (anyOff) a.noTurn = false; } else if (what == "scale") a.scale = std::clamp(v, 0.05, 20.0);
    else if (what == "group") { a.group = nextGroup; a.ovl = false; }                      // (a pack group: its Overlap stacks kept — a group may hold stacks)
    else if (what == "overlap") { a.stack = nextStack; a.ovl = false; }                    // (an Overlap stack: its pack group kept)
    else if (what == "ungroup") { if (anyGroup) a.group = 0; else a.stack = 0; a.ovl = false; } else if (what == "unstack") { a.stack = 0; a.ovl = false; } else if (what == "clear") a = {};
    if (!a.lock && !a.noTurn && !a.sym && a.turn == 0 && a.scale == 1 && a.group == 0 && a.stack == 0) continue;
    auto keyOf = [&](int g) { QJsonArray k; std::vector<int> vs(T.fv.begin() + T.fo[size_t(g)], T.fv.begin() + T.fo[size_t(g) + 1]); std::sort(vs.begin(), vs.end()); for (int x : vs) k.append(x); return k; };
    std::vector<int> mine; for (int g = 0; g < T.nf; g++) if (c->S.of[size_t(g)] == s && g != f) mine.push_back(g);   // (up to 24 more polygons spread over the shell: the setting follows its biggest part)
    QJsonArray ks; const size_t nk = std::min<size_t>(24, mine.size()); for (size_t i = 0; i < nk; i++) ks.append(keyOf(mine[i * mine.size() / nk]));
    arr.append(QJsonObject{{"k", keyOf(f)}, {"ks", ks}, {"lock", a.lock}, {"noTurn", a.noTurn}, {"sym", a.sym}, {"scale", a.scale}, {"group", a.group}, {"ovl", false}, {"stack", a.stack}, {"turn", a.turn}});
  }
  if (arr.isEmpty()) it->raw.remove("uvShellAttr"); else it->raw.insert("uvShellAttr", arr);
  const int n = int(anchor.size()); const QString pl = n == 1 ? "" : "s";
  const QString msg = what == "lock" ? (anyOff ? tr("%1 shell%2 locked — Pack and Unfold all leave them, Pack packs round them") : tr("%1 shell%2 unlocked")).arg(n).arg(pl)
                    : what == "free" ? (anyOff ? tr("%1 shell%2 — Pack turns them freely, whatever the rest does") : tr("%1 shell%2 — Pack turns them as the rest")).arg(n).arg(pl)
                    : what == "sym" ? (anyOff ? tr("%1 shell%2 — Pack packs them symmetrically (with their mirror shells, across the axis)") : tr("%1 shell%2 — Pack packs them as the rest")).arg(n).arg(pl)
                    : what == "noturn" ? (anyOff ? tr("%1 shell%2 — Pack won't turn them") : tr("%1 shell%2 — Pack may turn them again")).arg(n).arg(pl)
                    : what == "rot90" ? (anyOff ? tr("%1 shell%2 — Pack turns them only by 90°") : tr("%1 shell%2 — Pack may turn them as before")).arg(n).arg(pl)
                    : what == "rot180" ? (anyOff ? tr("%1 shell%2 — Pack turns them only by 180°") : tr("%1 shell%2 — Pack may turn them as before")).arg(n).arg(pl)
                    : what == "scale" ? tr("%1 shell%2 at ×%3 the texel density of the rest (Pack, Set)").arg(n).arg(pl).arg(v, 0, 'f', 2)
                    : what == "group" ? tr("%1 shell%2 — group %3: Pack lays them together").arg(n).arg(pl).arg(nextGroup)
                    : what == "overlap" ? tr("%1 shell%2 — Overlap stack %3: they lie on each other, Pack keeps them so and moves them as one (a pack group can hold it)").arg(n).arg(pl).arg(nextStack)
                    : what == "ungroup" ? (anyGroup ? tr("%1 shell%2 out of their pack groups (their Overlap stacks kept)") : tr("%1 shell%2 out of their Overlap stacks")).arg(n).arg(pl)
                    : what == "unstack" ? tr("%1 shell%2 out of their Overlap stacks").arg(n).arg(pl) : tr("%1 shell%2: their own settings cleared").arg(n).arg(pl);
  stepLabel_ = msg; scene_->revision++; commit(false); uvChanged(); emit topoState(); ui::toast(msg); return msg;
}

// Overlap: the shells of `src` laid on the shell of `targetFace` (their middles one), and tagged one group that Pack keeps lying so and moves as one
QString Viewport::uvOverlapOnto(const std::vector<int>& src, int targetFace) {
  const int am = topoMesh(); if (am < 0 || topoMode == "object") { ui::toast(tr("Select polygons of the shells (Polygon mode)")); return "!"; }
  const int h0 = histIdx_;                                     // (the shells laid and their stack tag: one undo step)
  const QString r = uvEdit([&](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    if (targetFace < 0 || targetFace >= T.nf) return tr("!Pick the shell the others go on");
    const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P);
    auto facesOf = [&](int s) { std::vector<int> fs; for (int f = 0; f < T.nf; f++) if (S.of[size_t(f)] == s) fs.push_back(f); return fs; };
    (void)P; (void)S; (void)facesOf; const uv::PackOpts po = uvPackOpts(am); const int n = uv::stackOnto(m, T, src, targetFace, &po);   // (the shells brought onto it: the border and the points inside on the reference's, one to one)
    if (!n) return tr("!Nothing to overlap — the other shells and the one they go on must differ");
    return tr("%1 shell%2 laid on the last one").arg(n).arg(n == 1 ? "" : "s"); });
  if (r.startsWith("!")) return r;
  std::vector<int> all = src; all.push_back(targetFace); tselApply(am, all, "set", 'f'); const QString t = uvShellSet("overlap"); histMerge(h0); return t;
}
// the selected shells onto the last picked polygon's shell
QString Viewport::uvOverlapSel() {
  const int am = topoMesh(); if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons of the shells; the last picked one is the shell the others go on")); return "!"; }
  const auto fs = tSelFaces(am); if (fs.size() < 2) { ui::toast(tr("Select polygons of at least two shells")); return "!"; }
  const int last = tsel_.last >= 0 && std::find(fs.begin(), fs.end(), tsel_.last) != fs.end() ? tsel_.last : fs.back();
  return uvOverlapOnto(fs, last);
}
// Ctrl+C: the selected shells remembered; Ctrl+V: with a shell picked — they are laid on it (the group tagged Overlap)
void Viewport::uvCopyShells() {
  const int am = topoMesh(); if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select polygons of the shells to copy")); return; }
  const auto fs = tSelFaces(am); if (fs.empty()) { ui::toast(tr("Select polygons of the shells to copy")); return; }
  uvClip_ = fs; uvClipMesh_ = am; ui::hud(tr("Copied: the shells of %1 polygon%2 — pick another shell and press Ctrl+V to lay them on it").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"), 2200);
}
QString Viewport::uvPasteShells() {
  const int am = topoMesh(); if (uvClip_.empty() || uvClipMesh_ != am) { ui::toast(tr("Nothing copied — Ctrl+C on shells first")); return "!"; }
  if (topoMode == "object" || tsel_.id != am) { ui::toast(tr("Pick the shell they go on")); return "!"; }
  const auto fs = tSelFaces(am); if (fs.empty()) { ui::toast(tr("Pick the shell they go on")); return "!"; }
  return uvOverlapOnto(uvClip_, fs.back());
}
}  // namespace hm

namespace hm {
namespace {
// a small stroke font for the editor's markers (G, S, y, m, the digits): strokes in a cell 1 wide, 1 high (y down; a lowercase one from 0.3, y below 1)
using Stroke = std::vector<std::pair<double, double>>;
std::vector<Stroke> glyph(QChar c) {
  switch (c.unicode()) {
    case 'G': return {{{1, 0.2}, {0.8, 0}, {0.2, 0}, {0, 0.2}, {0, 0.8}, {0.2, 1}, {0.8, 1}, {1, 0.8}, {1, 0.5}, {0.55, 0.5}}};
    case 'S': return {{{1, 0.15}, {0.8, 0}, {0.2, 0}, {0, 0.15}, {0, 0.35}, {0.2, 0.5}, {0.8, 0.5}, {1, 0.65}, {1, 0.85}, {0.8, 1}, {0.2, 1}, {0, 0.85}}};
    case 'y': return {{{0, 0.3}, {0.5, 1}}, {{1, 0.3}, {0.3, 1.3}}};
    case 'm': return {{{0, 1}, {0, 0.3}}, {{0, 0.5}, {0.2, 0.3}, {0.45, 0.3}, {0.5, 0.5}, {0.5, 1}}, {{0.5, 0.5}, {0.7, 0.3}, {0.95, 0.3}, {1, 0.5}, {1, 1}}};
    case 'x': return {{{0, 0.3}, {1, 1}}, {{1, 0.3}, {0, 1}}};
    case '.': return {{{0.4, 0.95}, {0.6, 1}}};
    case 'O': return {{{0.2, 0}, {0.8, 0}, {1, 0.2}, {1, 0.8}, {0.8, 1}, {0.2, 1}, {0, 0.8}, {0, 0.2}, {0.2, 0}}};   // (the Overlap stack's plate)
    default: break;
  }
  if (c >= '0' && c <= '9') {   // (seven segments: a top, b upper right, c lower right, d bottom, e lower left, f upper left, g middle)
    static const char* seg[] = {"abcdef", "bc", "abged", "abgcd", "fgbc", "afgcd", "afgedc", "abc", "abcdefg", "abcdfg"};
    std::vector<Stroke> o; for (const char* p = seg[c.unicode() - '0']; *p; p++) {
      switch (*p) { case 'a': o.push_back({{0, 0}, {1, 0}}); break; case 'b': o.push_back({{1, 0}, {1, 0.5}}); break; case 'c': o.push_back({{1, 0.5}, {1, 1}}); break;
        case 'd': o.push_back({{0, 1}, {1, 1}}); break; case 'e': o.push_back({{0, 0.5}, {0, 1}}); break; case 'f': o.push_back({{0, 0}, {0, 0.5}}); break; default: o.push_back({{0, 0.5}, {1, 0.5}}); break; } }
    return o;
  }
  return {};
}
// the text as strokes at (x, y) (its top-left), `h` px high
void strokeText(std::vector<Viewport::Overlay>& out, const QString& s, double x, double y, double h, const QColor& col) {
  const double w = h * 0.55, gap = h * 0.4;
  for (const QChar ch : s) { for (const auto& st : glyph(ch)) { std::vector<std::pair<double, double>> P; for (const auto& q : st) P.push_back({x + q.first * w, y + q.second * h}); out.push_back({P, col, false}); for (auto& q : P) q.first += 0.8; out.push_back({P, col, false}); }
    x += w + gap; }
}
double textWidth(const QString& s, double h) { return s.size() * (h * 0.95) - h * 0.4; }
}  // namespace


// the pack group whose plate (its letter and number at the frame's top-left) is under the pointer; 0 — none
int Viewport::uvGroupAt(const QPointF& mp) const {
  if (cam_.view != "uv" || room() != "topo" || !tset.uv) return 0;
  const int am = topoMesh(); const Item* it = am >= 0 ? scene_->item(am) : nullptr; if (!it || it->raw.value("uvShellAttr").toArray().isEmpty()) return 0;
  const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) return 0;
  const UvCache* c = uvCache(); if (!c) return 0; const auto attrs = uvShellAttrs(am); if (attrs.size() != size_t(c->T->nf)) return 0;
  const std::vector<char> hidF = uvHidden(*it, B); std::map<int, std::array<double, 4>> g;
  const int inSetOnly = topo::hasUvSets(B) ? std::max(uvWorkSet(am, B), -1) : -1;   // (the set worked in: the tags of its own shells only)
  for (int f = 0; f < c->T->nf; f++) { if (!hidF.empty() && size_t(f) < hidF.size() && hidF[size_t(f)]) continue; if (inSetOnly >= 0 && !(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> inSetOnly & 1u))) continue; const int gid = attrs[size_t(f)].group; if (gid <= 0) continue;
    auto& b = g.emplace(gid, std::array<double, 4>{1e300, 1e300, -1e300, -1e300}).first->second;
    for (int k = c->T->fo[size_t(f)]; k < c->T->fo[size_t(f) + 1]; k++) { const auto& q = B.cornerUV[size_t(k)]; b[0] = std::min(b[0], double(q[0])); b[1] = std::min(b[1], double(q[1])); b[2] = std::max(b[2], double(q[0])); b[3] = std::max(b[3], double(q[1])); } }
  for (const auto& [gid, b] : g) { const QPointF a = uvToPx(b[0], b[3]), d = uvToPx(b[2], b[1]); const double pad = 7, h = 13, x0 = std::min(a.x(), d.x()) - pad, y0 = std::min(a.y(), d.y()) - pad;
    const double tw = (QString::number(gid).size() + 1) * (h * 0.95) - h * 0.4;
    if (mp.x() >= x0 - 2 && mp.x() <= x0 + tw + 12 && mp.y() >= y0 - h - 10 && mp.y() <= y0 + 2) return gid; }
  return 0;
}
// the markers of the shells' own settings in the UV Editor: a pack group — a frame round its shells with its letter G and number; a shell tagged "Pack symmetrically" — Sym beside it
void Viewport::uvMarkOverlay(std::vector<Overlay>& out) const {
  if (cam_.view != "uv" || room() != "topo" || !tset.uv) return;
  const int am = topoMesh(); const Item* it = am >= 0 ? scene_->item(am) : nullptr; if (!it || it->raw.value("uvShellAttr").toArray().isEmpty()) return;
  const PolyMesh& B = *baseOf(*it); if (!uv::has(B)) return;
  const UvCache* c = uvCache(); if (!c) return; const auto attrs = uvShellAttrs(am); if (attrs.size() != size_t(c->T->nf)) return;
  struct Bx { double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300; int group = 0; bool sym = false; uv::ShellAttr attr; };
  std::map<int, Bx> sh; const std::vector<char> hidF = uvHidden(*it, B);
  const int inSetOnly = topo::hasUvSets(B) ? std::max(uvWorkSet(am, B), -1) : -1;   // (the set worked in: the tags of its own shells only)
  for (int f = 0; f < c->T->nf; f++) { if (!hidF.empty() && size_t(f) < hidF.size() && hidF[size_t(f)]) continue; if (inSetOnly >= 0 && !(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> inSetOnly & 1u))) continue; const auto& a = attrs[size_t(f)]; if (a.group <= 0 && a.stack <= 0 && !a.sym && !a.lock && !a.noTurn && a.turn == 0 && a.scale == 1) continue;
    Bx& b = sh[c->S.of[size_t(f)]]; b.group = a.group; b.sym = a.sym; b.attr = a;
    for (int k = c->T->fo[size_t(f)]; k < c->T->fo[size_t(f) + 1]; k++) { const auto& q = B.cornerUV[size_t(k)]; b.u0 = std::min(b.u0, double(q[0])); b.v0 = std::min(b.v0, double(q[1])); b.u1 = std::max(b.u1, double(q[0])); b.v1 = std::max(b.v1, double(q[1])); } }
  static const QColor gc[] = {QColor(0x9c, 0x64, 0xf0), QColor(0xe0, 0x9c, 0x30), QColor(0x3c, 0xb4, 0x6e), QColor(0x28, 0xa0, 0xb4), QColor(0xd2, 0x5a, 0x96), QColor(0x96, 0xb4, 0x28)};
  std::map<int, Bx> groups; for (const auto& [s, b] : sh) if (b.group > 0) { Bx& g = groups[b.group]; g.u0 = std::min(g.u0, b.u0); g.v0 = std::min(g.v0, b.v0); g.u1 = std::max(g.u1, b.u1); g.v1 = std::max(g.v1, b.v1); }
  for (const auto& [gid, g] : groups) { const QColor col = gc[(gid - 1) % 6]; const QPointF a = uvToPx(g.u0, g.v1), d = uvToPx(g.u1, g.v0); const double pad = 7;   // (screen: top-left is the larger V)
    const double x0 = std::min(a.x(), d.x()) - pad, x1 = std::max(a.x(), d.x()) + pad, y0 = std::min(a.y(), d.y()) - pad, y1 = std::max(a.y(), d.y()) + pad;
    out.push_back({{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, QColor(0x14, 0x14, 0x18), true});                 // (dark below, light over: seen on any ground)
    out.push_back({{{x0 + 1, y0 + 1}, {x1 - 1, y0 + 1}, {x1 - 1, y1 - 1}, {x0 + 1, y1 - 1}}, col, true});
    const QString t = "G" + QString::number(gid); const double h = 13, tw = textWidth(t, h);   // (the letter and the number on a plate at the frame's top-left)
    out.push_back({{{x0, y0 - h - 8}, {x0 + tw + 10, y0 - h - 8}, {x0 + tw + 10, y0}, {x0, y0}}, col, true});
    strokeText(out, t, x0 + 5, y0 - h - 4, h - 2, col); }
  // (the Overlap stacks: a thin light frame close round each, its O and number on a plate at the bottom-left — inside a group's frame when the group holds it)
  std::map<int, Bx> stacks; for (const auto& [s, b] : sh) if (b.attr.stack > 0) { Bx& g = stacks[b.attr.stack]; g.u0 = std::min(g.u0, b.u0); g.v0 = std::min(g.v0, b.v0); g.u1 = std::max(g.u1, b.u1); g.v1 = std::max(g.v1, b.v1); }
  for (const auto& [sid, g] : stacks) { const QColor col(0xc8, 0xcc, 0xd8); const QPointF a = uvToPx(g.u0, g.v1), d = uvToPx(g.u1, g.v0); const double pad = 3;
    const double x0 = std::min(a.x(), d.x()) - pad, x1 = std::max(a.x(), d.x()) + pad, y0 = std::min(a.y(), d.y()) - pad, y1 = std::max(a.y(), d.y()) + pad;
    out.push_back({{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, col, true});
    const QString t = "O" + QString::number(sid); const double h = 11, tw = textWidth(t, h);
    out.push_back({{{x0, y1}, {x0 + tw + 8, y1}, {x0 + tw + 8, y1 + h + 6}, {x0, y1 + h + 6}}, col, true});
    strokeText(out, t, x0 + 4, y1 + 3, h - 2, col); }
  // the tags on the shells: a row of small badges above each shell's top-left — a lock, Keep orientation (a ring struck through), 90 / 180, Free turn (a circular arrow), the density ×, Sym
  for (const auto& [s, b] : sh) { const auto& a = b.attr; const bool any = a.lock || a.noTurn || a.turn == 2 || a.turn == 3 || a.turn == 4 || a.scale != 1 || a.sym; if (!any) continue;
    const QPointF pa = uvToPx(b.u0, b.v1), pd = uvToPx(b.u1, b.v0); double x = std::min(pa.x(), pd.x()), y = std::min(pa.y(), pd.y()) - 26; const double S = 16, th = 10;
    const QColor slate(0x9a, 0xaa, 0xd8), amber(0xf0, 0x9a, 0x2e);
    auto box = [&](double w, const QColor& col) { out.push_back({{{x, y}, {x + w, y}, {x + w, y + S}, {x, y + S}}, col, true}); };
    auto stroke = [&](const std::vector<std::pair<double, double>>& P, const QColor& col, bool closed = false) { std::vector<std::pair<double, double>> Q; for (const auto& q : P) Q.push_back({x + q.first, y + q.second}); out.push_back({Q, col, closed}); };
    auto ring = [&](double r, double a0, double a1, const QColor& col) { std::vector<std::pair<double, double>> P; for (int i = 0; i <= 14; i++) { const double t = a0 + (a1 - a0) * i / 14.0; P.push_back({S / 2 + r * std::cos(t), S / 2 + r * std::sin(t)}); } stroke(P, col); };
    auto text = [&](const QString& t, const QColor& col) { const double w = textWidth(t, th) + 8; box(w, col); strokeText(out, t, x + 4, y + 3, th, col); x += w + 4; };
    if (a.lock) { box(S, slate); stroke({{4, 8}, {4, 5.5}, {6, 3.5}, {10, 3.5}, {12, 5.5}, {12, 8}}, slate); stroke({{3, 8}, {13, 8}, {13, 13.5}, {3, 13.5}}, slate, true); x += S + 4; }
    if (a.noTurn) { box(S, amber); ring(4.6, 0, 6.2832, amber); stroke({{3, 13}, {13, 3}}, amber); x += S + 4; }
    if (a.turn == 2) text("90", amber);
    if (a.turn == 3) text("180", amber);
    if (a.turn == 4) { box(S, amber); ring(4.6, 0.5, 5.4, amber); stroke({{12.2, 5.2}, {12.6, 8.4}, {9.6, 7.4}}, amber); x += S + 4; }
    if (a.scale != 1) text("x" + QString::number(a.scale, 'g', 3), amber);
    if (a.sym) text("Sym", amber); }
}
}  // namespace hm
