// The Brush (M): the anchors under the circle follow the pointer (by its falloff — hardness: the share of the radius with
// full strength); Screen — inside the circle on the screen, whatever the depth; Surface — around the 3D point touched (the
// front-most hair under the cursor), a sphere of the brush's radius. Shift — smooth (each anchor toward its neighbours'
// middle), Ctrl — inflate the lock (away from its cross-section's centre), Ctrl+Alt — shrink it; the longer the stroke,
// the more. With curves (or a disc) selected only their anchors. Ctrl+RMB drag — radius (left / right) and hardness
// (up / down), Ctrl + wheel — radius.
#include "scene/Edit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <cmath>
#include <map>

namespace hm {

QString Viewport::brushMode() const { return scene_->fileRaw.value("settings").toObject().value("brushMode").toString("screen"); }
void Viewport::setBrushMode(const QString& m) { QJsonObject S = scene_->fileRaw.value("settings").toObject(); S.insert("brushMode", m); scene_->fileRaw.insert("settings", S); }
void Viewport::brushResize(double k) { brushR = clamp(brushR * k, 8, 800); overlayRev_++; requestUpdate(); }
void Viewport::panelsLater() {
  if (!panelsLater_) { panelsLater_ = new QTimer(this); panelsLater_->setSingleShot(true); panelsLater_->setInterval(300); connect(panelsLater_, &QTimer::timeout, this, [this] { emit brushChanged(); }); }
  panelsLater_->start();
}

double Viewport::worldPerPixelAt(const Vec3& p) const {
  Projector P = projector(); ScreenPt a = P.toScreen(p), b = P.toScreen(p + cam_.right() * (cam_.sceneScale * 0.01));
  double px = std::hypot(b.x - a.x, b.y - a.y); return px > 1e-9 ? cam_.sceneScale * 0.01 / px : cam_.sceneScale * 1e-3;
}

// hardness = the share of the radius with full strength; outside it the influence fades smoothly
double Viewport::brushWeight(double d) const {
  double R = brushR, r0 = brushHard * R; if (d <= r0) return 1;
  double t = clamp((d - r0) / std::max(R - r0, 1e-6), 0, 1); return 1 - t * t * (3 - 2 * t);
}

// the anchors the brush works on: the shown ones (not a disc's) — with curves / a disc selected, only theirs
std::vector<int> Viewport::brushAnchors() const {
  std::set<int> set; bool limit = false;
  for (int id : sel_.items) {
    const Item* it = scene_->item(id); if (!it) continue;
    if (it->type == ItemType::Curve && it->ring < 0) { limit = true; set.insert(it->anchors.begin(), it->anchors.end()); }
    if (it->type == ItemType::Guide && it->raw.value("kind").toString() == "disc") { limit = true; for (const QJsonValue& r : it->raw.value("rings").toArray()) if (const Item* c = scene_->item(r.toInt())) set.insert(c->anchors.begin(), c->anchors.end()); }
  }
  std::vector<int> out;
  for (auto& a : anchors_) { if (limit ? set.count(a.id) : !edit::isRingAnchor(*scene_, a.id)) out.push_back(a.id); }
  return out;
}

// Surface mode: the front-most hair surface, curve or anchor under the cursor
std::optional<Vec3> Viewport::brushCenter3D(const QPointF& m, const std::vector<int>& list) const {
  std::optional<Vec3> best; double bd = 1e300;
  if (auto h = pickSurface(m)) { best = h->point; bd = h->t; }
  Projector P = projector(); Vec3 eye = cam_.pos;
  for (const Item* c : scene_->curves()) {
    if (!curveVisible(*c)) continue; auto it = hair_.curves.find(c->id); if (it == hair_.curves.end()) continue;
    for (auto& s : it->second.p) { ScreenPt q = P.toScreen(s); if (q.z > 1 || std::hypot(q.x - m.x(), q.y - m.y()) > 14) continue; double d = s.distanceTo(eye); if (d < bd) { bd = d; best = s; } }
  }
  if (!best) for (int a : list) { const Vec3& p = scene_->anchor(a)->pos; ScreenPt q = P.toScreen(p); if (q.z > 1 || std::hypot(q.x - m.x(), q.y - m.y()) > brushR) continue; double d = p.distanceTo(eye); if (d < bd) { bd = d; best = p; } }
  return best;
}

std::vector<std::pair<int, double>> Viewport::brushCapture(const QPointF& m) const {
  auto list = brushAnchors(); std::vector<std::pair<int, double>> out; Projector P = projector();
  if (brushMode() != "surface") {
    for (int a : list) { ScreenPt s = P.toScreen(scene_->anchor(a)->pos); double d = std::hypot(s.x - m.x(), s.y - m.y()); if (d < brushR && s.z < 1) out.push_back({a, brushWeight(d)}); }
    return out;
  }
  auto c = brushCenter3D(m, list); if (!c) return out;
  double R = brushR * worldPerPixelAt(*c);
  for (int a : list) { double d = scene_->anchor(a)->pos.distanceTo(*c) / R * brushR; if (d < brushR) out.push_back({a, brushWeight(d)}); }
  return out;
}

void Viewport::startBrush(const QPointF& m, bool smooth) {
  bd_ = {}; bd_.last = m;
  if (smooth) { drag_ = Drag::BrushSmooth; ui::hud(tr("Smooth brush")); smoothStep(m); return; }
  bd_.n = cam_.dir();
  for (auto [a, w] : brushCapture(m)) { Vec3 p = scene_->anchor(a)->pos; bd_.cap.push_back({a, w, p, planeHit(ray(m), p, bd_.n), {}}); }
  { std::vector<int> l; for (auto& c : bd_.cap) l.push_back(c.a); hsym_ = hairSymTwins(l); }   // (Symmetry: the brushed anchors' twins follow mirrored)
  // (both sides under the brush — the crown brushed across its middle: each side moved by the stroke and by its mirror,
  // not both the same way)
  for (size_t i = 0; i < bd_.cap.size(); i++) for (size_t k = 0; k < hsym_.T.size(); k++) { const Vec3 m = hsym_.T[k].apply(bd_.cap[i].start); int best = -1; double bdist = symo.tol;
    for (size_t j = 0; j < bd_.cap.size(); j++) { const double d = bd_.cap[j].start.distanceTo(m);   // (one on the plane: its own mirror — kept on it)
      if (d <= bdist) { bdist = d; best = int(j); } }
    if (best >= 0) bd_.mir.push_back({int(i), best, int(k)}); }
  drag_ = Drag::Brush;
}

// Shift: every anchor under the brush toward the middle of its neighbours on each curve through it (ends stay)
void Viewport::smoothStep(const QPointF& m) {
  std::vector<std::tuple<int, Vec3, double>> moves;
  for (auto [a, w] : brushCapture(m)) {
    Vec3 t; int n = 0;
    for (int cid : edit::curvesWith(*scene_, a)) {
      const Item* c = scene_->item(cid); const auto& A = c->anchors; int L = int(A.size()), k = int(std::find(A.begin(), A.end(), a) - A.begin());
      if (!c->closed && (k <= 0 || k >= L - 1)) continue;
      t += scene_->anchor(A[size_t((k + L - 1) % L)])->pos; t += scene_->anchor(A[size_t((k + 1) % L)])->pos; n += 2;
    }
    if (n) moves.push_back({a, t / double(n), w * 0.15});
  }
  HairSym H; { std::vector<int> l; for (auto& x : moves) l.push_back(std::get<0>(x)); H = hairSymTwins(l); }   // (Symmetry: each step's twins)
  for (auto& [a, t, w] : moves) { Anchor* an = scene_->anchor(a); an->pos = an->pos.lerp(t, w); }
  if (!H.start.empty()) hairSymApply(H);
  if (!moves.empty()) { std::vector<int> l; for (auto& x : moves) l.push_back(std::get<0>(x)); const auto tw = H.twinIds(); l.insert(l.end(), tw.begin(), tw.end()); markCurves(curvesTouching(*scene_, l)); bd_.moved = true; scene_->revision++; }
}

// Ctrl: inflate (sign 1), Ctrl+Alt: shrink (−1) — away from / toward the middle of the lock's cross-section there (the
// mean of the nearest points of all curves of its lock); curves outside locks stay
void Viewport::startInflate(const QPointF& m, int sign) {
  bd_ = {}; bd_.last = m; bd_.sign = sign;
  // the locks: curves joined by walls
  std::map<int, int> par; std::function<int(int)> f = [&](int x) { auto it = par.find(x); if (it == par.end() || it->second == x) return x; return par[x] = f(it->second); };
  for (auto& [id, s] : scene_->items) if (s.type == ItemType::Surface && scene_->item(s.a) && scene_->item(s.b)) { par.try_emplace(s.a, s.a); par.try_emplace(s.b, s.b); par[f(s.a)] = f(s.b); }
  std::map<int, std::vector<int>> lockOf; for (auto& [id, _] : par) lockOf[f(id)].push_back(id);
  for (auto [a, w] : brushCapture(m)) {
    int lock = -1; for (int cid : edit::curvesWith(*scene_, a)) if (par.count(cid)) { lock = f(cid); break; }
    if (lock < 0) continue;
    Vec3 p = scene_->anchor(a)->pos, cen; int n = 0;
    for (int id : lockOf[lock]) {
      auto it = hair_.curves.find(id); if (it == hair_.curves.end() || it->second.p.empty()) continue;
      double best = 1e300; Vec3 q; for (auto& s : it->second.p) { double d = (s - p).lengthSq(); if (d < best) { best = d; q = s; } }
      cen += q; n++;
    }
    if (n >= 2) bd_.cap.push_back({a, w, p, std::nullopt, cen / double(n)});
  }
  { std::vector<int> l; for (auto& c : bd_.cap) l.push_back(c.a); hsym_ = hairSymTwins(l); }   // (Symmetry: their twins too)
  drag_ = Drag::Inflate;
  ui::hud(!bd_.cap.empty() ? (sign > 0 ? tr("Inflate: drag — fatter") : tr("Shrink: drag — thinner")) : tr("No lock anchors under the brush"));
}

void Viewport::brushMove(const QPointF& m) {
  switch (drag_) {
    case Drag::Brush: {
      { std::vector<Vec3> D(bd_.cap.size());
        for (size_t i = 0; i < bd_.cap.size(); i++) { auto& c = bd_.cap[i]; auto h = planeHit(ray(m), c.start, bd_.n); if (h && c.hit0) D[i] = (*h - *c.hit0) * c.w; }
        std::vector<Vec3> T = D;
        for (auto& [i, j, k] : bd_.mir) { const Affine& A = hsym_.T[size_t(k)]; T[size_t(j)] = T[size_t(j)] + (A.apply(D[size_t(i)]) - A.apply(Vec3())); }   // (the mirror's stroke: its direction mirrored)
        for (size_t i = 0; i < bd_.cap.size(); i++) scene_->anchor(bd_.cap[i].a)->pos = bd_.cap[i].start + T[i]; }
      if (!hsym_.start.empty()) hairSymApply(hsym_);
      if (!bd_.cap.empty()) { std::vector<int> l; for (auto& c : bd_.cap) l.push_back(c.a); const auto tw = hsym_.twinIds(); l.insert(l.end(), tw.begin(), tw.end()); markCurves(curvesTouching(*scene_, l)); bd_.moved = true; scene_->revision++; }
      break;
    }
    case Drag::BrushSmooth: smoothStep(m); break;
    case Drag::Inflate: {
      bd_.dist += std::hypot(m.x() - bd_.last.x(), m.y() - bd_.last.y()); bd_.last = m;
      double s = bd_.sign > 0 ? 1 + bd_.dist / 250 : std::max(0.0, 1 - bd_.dist / 250);
      for (auto& c : bd_.cap) scene_->anchor(c.a)->pos = c.cen + (c.start - c.cen) * (1 + (s - 1) * c.w);
      if (!hsym_.start.empty()) hairSymApply(hsym_);
      if (!bd_.cap.empty()) { std::vector<int> l; for (auto& c : bd_.cap) l.push_back(c.a); const auto tw = hsym_.twinIds(); l.insert(l.end(), tw.begin(), tw.end()); markCurves(curvesTouching(*scene_, l)); bd_.moved = true; scene_->revision++; }
      ui::hud(tr("%1: %2%3%").arg(bd_.sign > 0 ? tr("Inflate") : tr("Shrink")).arg(s >= 1 ? "+" : "").arg(int(std::lround((s - 1) * 100))));
      break;
    }
    case Drag::BrushAdjust: {
      if (mergePick) { pickR = clamp(bd_.r0 + (m.x() - bd_.start.x()), 2, 400); overlayRev_++; break; }   // (◎ Pick: its size)
      if (ribDrawOn()) {                                              // (a flat lock: its half width)
        ribR = clamp(bd_.r0 + (m.x() - bd_.start.x()), 3, 400);
        ui::hud(tr("Flat lock: width %1 px").arg(int(std::lround(ribR * 2)))); panelsLater(); break;
      }
      if (tool_ == "cut") {                                           // (Split ends: the spread)
        splitR = clamp(bd_.r0 + (m.x() - bd_.start.x()), 2, 300);
        ui::hud(tr("Split ends: spread %1 px").arg(int(std::lround(splitR)))); panelsLater(); break;
      }
      brushR = clamp(bd_.r0 + (m.x() - bd_.start.x()), 8, 800);
      brushHard = clamp(bd_.h0 - (m.y() - bd_.start.y()) / 200, 0, 1);
      ui::hud(tr("Brush: radius %1 px · hardness %2%").arg(int(std::lround(brushR))).arg(int(std::lround(brushHard * 100))));
      panelsLater();
      break;
    }
    default: break;
  }
  overlayRev_++; requestUpdate();
}

}  // namespace hm
