// 〰 Curve modifiers in the viewport: the built modifiers kept in step with their curves and settings, the Extra tab's
// weight brush (Mask / Thickness / Twist painted along the source curve — it stays put while the weights change the shape)
#include "scene/Braid.h"
#include "scene/Tubes.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <cmath>
#include <cstring>

namespace hm {

namespace {
// pointOnCurve: the point at fraction t of the arc length
std::optional<Vec3> pointOnCurve(const CurveSampling& c, double t) {
  if (c.p.size() < 2 || c.arc.size() != c.p.size()) return std::nullopt;
  const auto& L = c.arc; double d = clamp(t, 0, 1) * L.back();
  size_t i = 0; while (i + 2 < L.size() && L[i + 1] < d) i++;
  double s = L[i + 1] - L[i];
  return c.p[i].lerp(c.p[i + 1], s > 0 ? clamp((d - L[i]) / s, 0, 1) : 0);
}
bool signedKind(const QString& k) { return k == "thick" || k == "twist"; }
}  // namespace

bool Viewport::weightBrushOn() const {
  if (tool_ != "modifier" || modTab != "extra" || !maskBrush.on) return false;
  for (auto& [id, it] : scene_->items) if (it.type == ItemType::XMod) {
    QString k = it.raw.value("kind").toString();
    if (k == maskBrush.kind || (maskBrush.kind == "mask" && it.raw.value("mesh").isDouble() && k == "thick")) return true;
  }
  return false;
}

bool Viewport::modPress(QMouseEvent* e) {
  bool alt = e->modifiers() & Qt::AltModifier, ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  if (e->button() != Qt::LeftButton || !weightBrushOn() || (alt && !(ctrl && signedKind(maskBrush.kind)))) return false;
  QString op = shift ? "smooth" : maskBrush.kind == "mask" ? (ctrl ? "erase" : "paint") : ctrl ? (alt ? "erase" : "invert") : "paint";
  QPointF m = e->position();
  md_ = {}; md_.op = op; md_.last = m; drag_ = Drag::Mask;
  paintWeights(m, op, 0.2);
  return true;
}

bool Viewport::paintWeights(const QPointF& m, const QString& op, double amt) {
  const WeightBrush& mb = maskBrush; const QString kind = mb.kind; double lo = signedKind(kind) ? -1 : 0;
  auto sel = tube::modTubes(*scene_, sel_);
  double target = op == "erase" ? 0 : kind == "mask" ? mb.strength : (op == "invert" ? -1 : 1) * (kind == "twist" ? mb.twist : mb.intensity);
  Projector P = projector(); bool changed = false;
  // imported meshes: per vertex, into their Thickness modifier — the Mask brush paints its mask (hold)
  if (kind == "mask" || kind == "thick") {
    auto selM = modMeshes();
    std::vector<int> xs; for (auto& [xid, x] : scene_->items) if (x.type == ItemType::XMod && x.raw.value("mesh").isDouble() && x.raw.value("kind").toString() == "thick" && x.visible && x.raw.value("mask").isArray()) xs.push_back(xid);
    for (int xid : xs) {
      int mesh = scene_->item(xid)->raw.value("mesh").toInt();
      if ((!sel.empty() || !selM.empty()) && std::find(selM.begin(), selM.end(), mesh) == selM.end()) continue;
      auto screen = [&](const Vec3& p, double& d) { ScreenPt s = P.toScreen(p); if (s.z > 1) return false; d = std::hypot(s.x - m.x(), s.y - m.y()); return true; };
      if (meshmods::paintWeights(*scene_, xid, kind == "mask", op, target, lo, amt, mb.mode == "abs", brushR, [this](double d) { return brushWeight(d); }, screen, cam_.pos, cam_.ortho(), camDir())) {
        md_.touched.insert(mesh); changed = true; }
    }
    if (changed) { staticRev_++; requestUpdate(); }
  }
  for (auto& [xid, x] : scene_->items) {
    if (x.type != ItemType::XMod || x.raw.value("kind").toString() != kind || !x.visible || !x.raw.value("mask").isArray() || !x.raw.value("tube").isDouble()) continue;
    int tid = x.raw.value("tube").toInt(); const Item* t = scene_->item(tid); if (!t || !tubes_.count(tid)) continue;
    auto ci = hair_.curves.find(t->raw.value("src").toInt(-1)); if (ci == hair_.curves.end()) continue;
    if (!sel.empty() && std::find(sel.begin(), sel.end(), tid) == sel.end()) continue;   // (with modifiers selected only they are painted)
    std::vector<double> W = tube::maskOf(x.raw.value("mask")); const std::vector<double> src = W; bool ch = false;
    const int N = int(W.size());
    for (int k = 0; k < N; k++) {
      auto q = pointOnCurve(ci->second, double(k) / (N - 1)); if (!q) continue;
      ScreenPt s = P.toScreen(*q); if (s.z > 1) continue;
      double d = std::hypot(s.x - m.x(), s.y - m.y()); if (d >= brushR) continue;
      double bw = brushWeight(d), old = W[size_t(k)];
      if (op == "smooth") { double sum = 0; int n = 0; for (int j = k - 3; j <= k + 3; j++) if (j >= 0 && j < N) { sum += src[size_t(j)]; n++; } W[size_t(k)] = old + (sum / n - old) * bw * 0.5; }
      else if (mb.mode == "abs") W[size_t(k)] = old + (target - old) * bw;
      else if (op == "erase") W[size_t(k)] = old > 0 ? std::max(0.0, old - bw * amt) : std::min(0.0, old + bw * amt);
      else W[size_t(k)] = clamp(old + target * bw * amt, lo, 1);
      if (W[size_t(k)] != old) ch = true;
    }
    if (!ch) continue;
    QJsonArray a; for (double v : W) a.append(v); x.raw.insert("mask", a);
    md_.touched.insert(tid); changed = true;
  }
  if (changed) liveTubes();
  return changed;
}

// the modifiers rebuilt where their curve, their settings or their extras changed (a signature of all that)
void Viewport::syncTubes() {
  if (overlayRev_ == tubesSeen_) return;
  tubesSeen_ = overlayRev_;
  bool changed = false; std::set<int> alive;
  const Vec3 cen = cam_.sceneCenter;
  for (auto& [id, t] : scene_->items) {
    if (t.type != ItemType::Tube || !scene_->isVisible(t)) continue;
    auto ci = hair_.curves.find(t.raw.value("src").toInt(-1)); if (ci == hair_.curves.end()) continue;
    alive.insert(id);
    QJsonObject sig = t.raw; QJsonArray ex;
    for (int x : tube::extrasOf(*scene_, id)) { const Item* xi = scene_->item(x); QJsonObject o = xi->raw; o.insert("visible", xi->visible); ex.append(o); }
    sig.insert("extras", ex);
    QByteArray s = QJsonDocument(sig).toJson(QJsonDocument::Compact);
    s.append(reinterpret_cast<const char*>(ci->second.p.data()), qsizetype(ci->second.p.size() * sizeof(Vec3)));
    s.append(reinterpret_cast<const char*>(&cen), sizeof cen);
    auto f = tubeSig_.find(id);
    if (f != tubeSig_.end() && f->second == s && tubes_.count(id)) continue;
    tubeSig_[id] = s; changed = true;
    auto m = tube::buildTube(*scene_, hair_.curves, t, cen);
    if (!m) { tubes_.erase(id); continue; }
    TubeDraw& d = tubes_[id]; d.n = m->n; d.NR = m->NR; d.pos = std::move(m->pos); d.col = std::move(m->col);
  }
  // the braids: each strand (and band) a surface of its own, keyed below zero, picked / lit as the braid
  for (auto& [id, b] : scene_->items) {
    if (b.type != ItemType::Braid || !scene_->isVisible(b)) continue;
    auto ci = hair_.curves.find(b.raw.value("src").toInt(-1)); if (ci == hair_.curves.end()) continue;
    QByteArray s = QJsonDocument(b.raw).toJson(QJsonDocument::Compact);
    s.append(reinterpret_cast<const char*>(ci->second.p.data()), qsizetype(ci->second.p.size() * sizeof(Vec3)));
    s.append(reinterpret_cast<const char*>(&cen), sizeof cen);
    for (const QJsonValue& x : b.raw.value("conns").toArray()) { auto k = hair_.curves.find(x.toInt(-1)); if (k != hair_.curves.end()) s.append(reinterpret_cast<const char*>(k->second.p.data()), qsizetype(k->second.p.size() * sizeof(Vec3))); }   // (an end follows its connector: reshaped — rebuilt)
    const int key0 = -(id * 64) - 1; std::vector<int> keys; for (auto& [k, t] : tubes_) if (t.item == id) keys.push_back(k);
    auto f = tubeSig_.find(key0);
    if (f != tubeSig_.end() && f->second == s && !keys.empty()) { for (int k : keys) alive.insert(k); continue; }
    tubeSig_[key0] = s; changed = true; for (int k : keys) tubes_.erase(k);
    auto B = braid::build(*scene_, hair_.curves, b, cen);
    if (!B) continue;
    static const QColor SC[] = {QColor(0xb8, 0x8c, 0xe8), QColor(0xe8, 0x9c, 0xc4), QColor(0x8c, 0xb4, 0xe8), QColor(0xe8, 0xc4, 0x8c), QColor(0x9c, 0xe0, 0xb8), QColor(0xd0, 0xa0, 0xf0)};
    int k = 0;
    // (nearly solid — the strands one over another stay apart; their mesh drawn over them)
    auto put = [&](const std::vector<std::vector<Vec3>>& rings, const QColor& col) { if (rings.size() < 2) return; TubeDraw& d = tubes_[key0 - k]; d = {}; d.item = id; d.guides = false; d.wire = true; d.opacity = 1; d.color = col;
      d.NR = int(rings.size()); d.n = int(rings[0].size()); for (auto& R : rings) d.pos.insert(d.pos.end(), R.begin(), R.end()); alive.insert(key0 - k); k++; };
    if (B->each) for (auto& S : B->strands) {   // (a cocoon round each tube: see-through, a little wider than it, its rings drawn)
      const int n = int(S.rings.size()); if (n < 2 || S.rings[0].size() < 3) continue;
      TubeDraw& d = tubes_[key0 - k]; d = {}; d.item = id; d.guides = false; d.ringLines = true; d.opacity = 0.08; d.color = QColor(0xc8, 0xb8, 0xf0); d.n = int(S.rings[0].size()); d.NR = n;
      for (int i = 0; i < n; i++) { const double grow = S.r[size_t(i)] * 0.18 + B->width * 0.004; for (const Vec3& p : S.rings[size_t(i)]) { const Vec3 o = p - S.mid[size_t(i)]; const double l = o.length(); d.pos.push_back(l > 1e-12 ? S.mid[size_t(i)] + o * ((l + grow) / l) : p); } }
      alive.insert(key0 - k); k++; }
    else { // (the cocoon round the braid: see-through, its levels drawn — the planes every ring lies on)
      const auto& LV = B->lv; const int M = 32, n = int(LV.size());
      if (n >= 2) { TubeDraw& d = tubes_[key0 - k]; d = {}; d.item = id; d.guides = false; d.ringLines = true; d.opacity = 0.08; d.color = QColor(0xc8, 0xb8, 0xf0); d.n = M; d.NR = n;
        for (int i = 0; i < n; i++) { double R = 0; int c = 0; for (int q = std::max(0, i - 3); q <= std::min(n - 1, i + 3); q++) { R = std::max(R, LV[size_t(q)].r); c++; } R = R * 1.06 + B->width * 0.02;
          for (int m = 0; m < M; m++) { const double th = 2 * 3.14159265358979323846 * m / M; d.pos.push_back(LV[size_t(i)].p + LV[size_t(i)].B * (R * std::cos(th)) + LV[size_t(i)].N * (R * std::sin(th))); } }
        alive.insert(key0 - k); k++; } }
    for (size_t i = 0; i < B->strands.size(); i++) put(B->strands[i].rings, SC[i % 6]);
  }
  for (auto it = tubes_.begin(); it != tubes_.end();) { if (!alive.count(it->first)) { tubeSig_.erase(it->first); it = tubes_.erase(it); changed = true; } else ++it; }
  if (changed) tubesRev_++;
}

}  // namespace hm
