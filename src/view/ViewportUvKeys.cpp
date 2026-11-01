// The UV Editor's hotkeys mode (the ⌨ button on its top) — the Topology room's selection, view and gizmo keys untouched
// (Ctrl+A / D / I, F, H, B, E / R / T, Q, X / Y / Z, Shift+X). Seams: C (polygons: cut off into a shell), Shift+C off, W
// sew, Shift+W snap together, Alt+C auto seams (closed shells). Brushes: Alt+R Relax, Alt+P Pin. Unfold: U, Shift+U all, Alt+U symmetric, O Optimize. Face: L open, Shift+L shut, Alt+L
// off. Shape: J grid, N line, I by edge, Shift+I upright, M / Shift+M mirror U / V, [ / ] turn, K / Shift+K stack /
// unstack, arrows nudge (Alt align, Alt+Shift space out). Symmetry: Shift+Y Symmetrize, Alt+Y its axis. Density: Alt+D
// take, Shift+D give. Pack: Shift+B all, Alt+B the selected, Alt+H lock, G / Shift+G group / ungroup. V: the view in
// turn. Pins: Alt+V / Alt+T straightened and pinned, Alt+Shift+C off — the item's raw "uvPins" [[a, b, 'v' | 'h'], …].
// And the preview: the shell under the pointer in 3D shown unwrapped in the editor.
#include "scene/Uv.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
std::pair<int, int> ekey(int a, int b) { return {std::min(a, b), std::max(a, b)}; }
// the edges the selection means (Edge mode: them; polygons: their outline; vertices: edges between two of them)
std::vector<int> selEdgesK(const topo::TTopo& T, char key, const std::set<int>& f, const std::set<int>& e, const std::set<int>& v) {
  std::vector<int> o;
  if (key == 'e') { for (int x : e) if (x >= 0 && x < T.ne) o.push_back(x); }
  else if (key == 'f') { for (int x = 0; x < T.ne; x++) { bool a = f.count(T.ef0[size_t(x)]) > 0, b = T.ef1[size_t(x)] >= 0 && f.count(T.ef1[size_t(x)]) > 0; if (a != b) o.push_back(x); } }
  else for (int x = 0; x < T.ne; x++) if (v.count(T.ea[size_t(x)]) && v.count(T.eb[size_t(x)])) o.push_back(x);
  return o;
}
}  // namespace

void Viewport::setUvKeys(bool on) { uvKeys = on; QSettings().setValue("uvKeys", on); ui::hud(on ? tr("UV hotkeys: C seam · U Unfold · Alt+U Unfold symmetric · O Optimize · W sew · Shift+Y Symmetrize · Alt+Y axis · L / Shift+L / Alt+L lips and lids · arrows nudge (Alt: align, Alt+Shift: space) · J grid · N line · I by edge (Shift: upright) · Shift+W snap together · Alt+V / Alt+T straighten and pin · Alt+Shift+C unpin") : tr("UV hotkeys off"), 2500); emit topoState(); }

uv::Pins Viewport::uvPinsOf(int m) const {
  uv::Pins P; const Item* it = scene_->item(m); if (!it) return P; const PolyMesh* B = baseOf(*it); const int n = B ? int(B->pos.size()) : 0;
  for (const QJsonValue& x : it->raw.value("uvPins").toArray()) { const QJsonArray a = x.toArray(); if (a.size() < 2) continue; const int p = a[0].toInt(), q = a[1].toInt(); if (p >= 0 && q >= 0 && p < n && q < n) P.insert(ekey(p, q)); }   // (p == q: a pinned point)
  return P;
}

bool Viewport::uvKey(int key, Qt::KeyboardModifiers mods) {
  if (uvBrush && room() == "topo" && tset.uv && !(mods & Qt::ControlModifier)) {   // (a brush on: [ ] its size, Shift+[ ] its strength, Esc off — the keys mode or not)
    if (key == Qt::Key_Escape) { setUvBrush(uvBrush); return true; }
    if (key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight || key == Qt::Key_BraceLeft || key == Qt::Key_BraceRight) { const bool up = key == Qt::Key_BracketRight || key == Qt::Key_BraceRight;
      if (mods & Qt::ShiftModifier) { uvBrushK = std::clamp(uvBrushK + (up ? 0.1 : -0.1), 0.1, 1.0); ui::hud(tr("Brush strength %1").arg(uvBrushK, 0, 'f', 1), 800); }
      else { uvBrushR = std::clamp(uvBrushR * (up ? 1.2 : 1 / 1.2), 6.0, 600.0); ui::hud(tr("Brush %1 px").arg(int(uvBrushR)), 800); }
      overlayRev_++; requestUpdate(); emit topoState(); return true; } }
  if (room() != "topo" || !uvKeys || !tset.uv) return false;
  const bool alt = mods & Qt::AltModifier, shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
  if (ctrl) return false;
  if (key == Qt::Key_C && alt && shift) { uvUnpin(); return true; }
  if (key == Qt::Key_V && alt && !shift) { uvPinStraight(true); return true; }
  if (key == Qt::Key_T && alt && !shift) { uvPinStraight(false); return true; }
  if (key == Qt::Key_U && alt && !shift) { uvUnfold(false, true); return true; }
  if (key == Qt::Key_Y && shift && !alt) { uvSymmetrize(); return true; }
  if (key == Qt::Key_Y && alt && !shift) { uvSymAxisFromSel(); emit topoState(); return true; }
  if (key == Qt::Key_L) { uvFeatureKey(alt ? 2 : shift ? 1 : 0); return true; }   // (the face: lips, lids — open / shut / off)
  // lining up: arrows — nudged by a texel (Shift: 10); Alt — to the selection's edge; Alt+Shift — spaced evenly
  if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down) {
    const bool h = key == Qt::Key_Left || key == Qt::Key_Right;
    if (alt && shift) uvDistribute(h);
    else if (alt) uvAlign(key == Qt::Key_Left ? "left" : key == Qt::Key_Right ? "right" : key == Qt::Key_Up ? "top" : "bottom");
    else { const int st = shift ? 10 : 1; uvNudge(key == Qt::Key_Left ? -st : key == Qt::Key_Right ? st : 0, key == Qt::Key_Up ? st : key == Qt::Key_Down ? -st : 0); }
    return true;
  }
  if (key == Qt::Key_J && !alt && !shift) { uvGrid(); return true; }
  if (key == Qt::Key_N && !alt && !shift) { uvStraightenLoop(); return true; }
  if (key == Qt::Key_I && !alt) { if (shift) uvOrientUpright(); else uvOrientEdge(); return true; }
  if (key == Qt::Key_W && shift && !alt) { uvSnapTogether(); return true; }
  // (stage 8: the rest of the plan's keys — none of the Topology room's selection / view / gizmo keys taken)
  if (key == Qt::Key_U && shift && !alt) { uvUnfold(true); return true; }                          // Shift+U: Unfold all
  if (key == Qt::Key_C && shift && !alt) { uvMarkSeams(false); return true; }                      // Shift+C: the seams off
  if (key == Qt::Key_C && alt && !shift) { uvAutoSeams(); return true; }                             // Alt+C: Auto seams for the closed shells
  if (key == Qt::Key_M && !alt) { uvFlip(!shift); return true; }                                   // M / Shift+M: mirrored in U / in V
  if (key == Qt::Key_K && !alt) { if (shift) uvUnstack(); else uvStack(); return true; }           // K / Shift+K: stack / unstack
  if (key == Qt::Key_D && alt && !shift) { uvDensityGet(); return true; }                          // Alt+D: the texel density taken
  if (key == Qt::Key_D && shift && !alt) { uvDensitySet(); return true; }                          // Shift+D: … given to the selected
  if (key == Qt::Key_H && alt && !shift) { uvShellSet("lock"); return true; }                      // Alt+H: locked for Pack / unlocked
  if (key == Qt::Key_G && !alt) { uvShellSet(shift ? "ungroup" : "group"); return true; }          // G / Shift+G: a pack group / out of it
  if (key == Qt::Key_B && shift && !alt) { uvPack(true); return true; }                            // Shift+B: Pack all
  if (key == Qt::Key_R && alt && !shift) { setUvBrush('r'); return true; }                       // Alt+R: the Relax brush on / off
  if (key == Qt::Key_P && alt && !shift) { setUvBrush('p'); return true; }                       // Alt+P: the Pin brush on / off
  if (key == Qt::Key_B && alt && !shift) { uvPack(false); return true; }                           // Alt+B: Pack the selected
  if ((key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight) && !alt && !shift && !isModelTool()) { uvTurn(key == Qt::Key_BracketRight ? -1 : 1); return true; }   // [ / ]: +90° / −90° (no modelling tool on — its own [ ])
  if (key == Qt::Key_V && !alt && !shift) {                                                        // V: the view of the UV in turn — plain → area stretch → angles → the problems
    const int st = uvo.diag ? 3 : uvo.stretch ? (uvo.stretchAngle ? 2 : 1) : 0, nx = (st + 1) % 4;
    uvo.stretch = nx == 1 || nx == 2; uvo.stretchAngle = nx == 2; uvo.diag = nx == 3; saveUvPrefs(); uvChanged(); emit topoState();
    ui::hud(nx == 0 ? tr("UV: plain") : nx == 1 ? tr("UV: stretch by area") : nx == 2 ? tr("UV: stretch by angles") : tr("UV: the problems (Check)"), 1100); return true; }
  if (alt || shift) return false;
  if (key == Qt::Key_C) { uvSeamKey(); return true; }
  if (key == Qt::Key_U) { uvUnfold(false); return true; }
  if (key == Qt::Key_O) { uvOptimize(); return true; }
  if (key == Qt::Key_W) { uvSew(); return true; }
  return false;
}

// C: edges — seams; polygons — their outline a seam and the area unfolded on its own in its place (a shell of its own)
QString Viewport::uvSeamKey() {
  const int am = topoMesh(); if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select edges — or polygons: their area becomes a shell of its own")); return "!"; }
  if (tKey() != 'f') return uvMarkSeams(true);
  std::set<int> F0 = tsel_.f; const uv::Pins pins = uvPinsOf(am); const bool sym = tset.sym, symU = uvo.symUnfold; const uv::PackOpts po = uvPackOpts(am); const auto feats = uvFeaturesOf(am);
  return uvEdit([&, sym, symU](PolyMesh& m, const topo::TTopo& T) -> QString {
    std::set<int> F = F0;
    if (sym) { const auto mc = uv::mirrorCorners(m, T); for (int f : F0) if (f >= 0 && f < T.nf) { const int k = mc[size_t(T.fo[size_t(f)])]; if (k >= 0) F.insert(T.cf[size_t(k)]); } }   // (Symmetry: the mirror polygons cut off too)
    auto es = selEdgesK(T, 'f', F, {}, {}); std::vector<int> fs(F.begin(), F.end());
    if (fs.empty()) return tr("!Select polygons");
    auto fl = uv::seamFlags(m, T); int n = 0; for (int e : es) if (T.ef1[size_t(e)] >= 0 && !fl[size_t(e)]) { fl[size_t(e)] = 1; n++; }
    uv::setSeams(m, T, fl);
    uv::SymUnfold su; if (symU) su.mc = uv::mirrorCorners(m, T);
    const bool had = uv::has(m); auto r = uv::unfold(m, T, fs, true, true, &pins, symU ? &su : nullptr, &po, &feats);
    // (laid in its place it meets the rest where it was cut — moved off it by a small gap: a shell of its own)
    if (r.done && had) {
      std::set<int> in(fs.begin(), fs.end()); const auto ks = uv::faceCorners(T, fs); std::vector<int> rest; for (int f = 0; f < T.nf; f++) if (!in.count(f)) rest.push_back(f);
      const auto b = uv::box(m, ks); const auto ko = uv::faceCorners(T, rest);
      if (!ko.empty()) { const auto o = uv::box(m, ko); double dx = (b[0] + b[2]) / 2 - (o[0] + o[2]) / 2, dy = (b[1] + b[3]) / 2 - (o[1] + o[3]) / 2; const double L = std::hypot(dx, dy);
        if (L < 1e-9) { dx = 1; dy = 0; } else { dx /= L; dy /= L; }
        const double gap = 0.03 * std::max(b[2] - b[0], b[3] - b[1]) + 0.004;
        uv::transform(m, ks, [&](const uv::UV& p) { return uv::UV{float(p[0] + dx * gap), float(p[1] + dy * gap)}; }); } }
    if (!r.done) return n ? tr("%1 seam%2 marked").arg(n).arg(n == 1 ? "" : "s") : tr("!Nothing to cut — the area is a shell already");
    return had ? tr("Cut off: %1 polygon%2 — a UV shell of its own (%3 seam%4)").arg(fs.size()).arg(fs.size() == 1 ? "" : "s").arg(n).arg(n == 1 ? "" : "s")
               : tr("Cut off and unfolded: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s");
  });
}

// Alt+V / Alt+T: the selected edges — each run of them (its UV points joined by them) laid straight up (one U) / across
// (one V), its points spread along it by their 3D lengths over the span it had; then pinned
QString Viewport::uvPinStraight(bool vertical) {
  const int am = topoMesh(); if (am < 0 || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select the edges to straighten and pin (Edge mode)")); return "!"; }
  const char key = tKey(); const std::set<int> F = tsel_.f, E = tsel_.e, V = tsel_.v; std::vector<std::pair<int, int>> made;
  const QString msg = uvEdit([&](PolyMesh& m, const topo::TTopo& T) -> QString {
    if (!uv::has(m)) return tr("!This mesh has no UV — Unfold first");
    auto es = selEdgesK(T, key, F, E, V); if (es.empty()) return tr("!Select the edges to straighten and pin (Edge mode)");
    const uv::Points P = uv::points(m, T);
    // (the UV points the edges join, on each side of them: a graph; its runs)
    std::map<int, std::set<int>> nb; std::map<int, int> vert;
    for (int e : es) for (int f : {T.ef0[size_t(e)], T.ef1[size_t(e)]}) { if (f < 0) continue;
      const int ka = uv::cornerAt(T, f, T.ea[size_t(e)]), kb = uv::cornerAt(T, f, T.eb[size_t(e)]); if (ka < 0 || kb < 0) continue;
      const int pa = P.of[size_t(ka)], pb = P.of[size_t(kb)]; if (pa == pb) continue; nb[pa].insert(pb); nb[pb].insert(pa); vert[pa] = T.ea[size_t(e)]; vert[pb] = T.eb[size_t(e)]; }
    std::set<int> seen; int runs = 0; const int ax = vertical ? 0 : 1, al = 1 - ax;   // (ax: the coordinate made one; al: along)
    auto setPt = [&](int q, double u, double v) { for (int i = P.off[size_t(q)]; i < P.off[size_t(q) + 1]; i++) m.cornerUV[size_t(P.list[size_t(i)])] = {float(u), float(v)}; };
    auto at = [&](int q) { return m.cornerUV[size_t(P.list[size_t(P.off[size_t(q)])])]; };
    for (auto& [q0, _] : nb) {
      if (seen.count(q0)) continue;
      std::vector<int> comp, st{q0}; seen.insert(q0);
      while (!st.empty()) { int q = st.back(); st.pop_back(); comp.push_back(q); for (int w : nb[q]) if (!seen.count(w)) { seen.insert(w); st.push_back(w); } }
      double mean = 0; for (int q : comp) mean += at(q)[size_t(ax)]; mean /= double(comp.size());
      // (a plain run — two ends: its points spread by their 3D lengths over the length it had in the UV; else in place)
      std::vector<int> ends; for (int q : comp) if (nb[q].size() == 1) ends.push_back(q);
      bool path = ends.size() == 2; for (int q : comp) if (nb[q].size() > 2) path = false;
      if (path) {
        std::vector<int> order{ends[0]}; int prev = -1, cur = ends[0];
        while (true) { int nx = -1; for (int w : nb[cur]) if (w != prev) { nx = w; break; } if (nx < 0) break; prev = cur; cur = nx; order.push_back(cur); }
        std::vector<double> cum{0}; for (size_t i = 1; i < order.size(); i++) cum.push_back(cum.back() + m.pos[size_t(vert[order[i]])].distanceTo(m.pos[size_t(vert[order[i - 1]])]));
        // (its length in the UV kept, round the middle of where it lay along; the way its ends ran — else up / right)
        double luv = 0, mid = 0; for (size_t i = 0; i < order.size(); i++) { mid += at(order[i])[size_t(al)]; if (i) luv += std::hypot(double(at(order[i])[0]) - at(order[i - 1])[0], double(at(order[i])[1]) - at(order[i - 1])[1]); }
        mid /= double(order.size()); const double sgn = at(order.back())[size_t(al)] < at(order.front())[size_t(al)] ? -1 : 1, tot = std::max(1e-12, cum.back());
        for (size_t i = 0; i < order.size(); i++) { const double along = mid + sgn * luv * (cum[i] / tot - 0.5); if (vertical) setPt(order[i], mean, along); else setPt(order[i], along, mean); }
      } else for (int q : comp) { const auto c = at(q); if (vertical) setPt(q, mean, c[1]); else setPt(q, c[0], mean); }
      runs++;
    }
    for (int e : es) made.push_back(ekey(T.ea[size_t(e)], T.eb[size_t(e)]));
    if (Item* it = scene_->item(am)) { const std::set<std::pair<int, int>> mk(made.begin(), made.end()); QJsonArray a;   // (the pins kept with the mesh — in this same step; one pinned again: its way now)
      for (const QJsonValue& x : it->raw.value("uvPins").toArray()) { const QJsonArray p = x.toArray(); if (p.size() >= 2 && mk.count(ekey(p[0].toInt(), p[1].toInt()))) continue; a.append(x); }
      for (auto [p, q] : mk) a.append(QJsonArray{p, q, vertical ? "v" : "h"}); it->raw.insert("uvPins", a); }
    return tr("%1 edge%2 straightened %3 and pinned — Unfold lays the shell round them").arg(es.size()).arg(es.size() == 1 ? "" : "s").arg(vertical ? tr("up (V)") : tr("across (U)"));
  });
  if (!msg.startsWith('!')) uvChanged();
  return msg;
}

QString Viewport::uvUnpin() {
  const int am = topoMesh(); Item* it = am >= 0 ? scene_->item(am) : nullptr; if (!it || topoMode == "object" || tsel_.id != am) { ui::toast(tr("Select the pinned edges (Edge mode)")); return "!"; }
  auto T = topo::ttopo(baseSp(*it)); auto es = selEdgesK(*T, tKey(), tsel_.f, tsel_.e, tsel_.v);
  std::set<std::pair<int, int>> off; for (int e : es) { off.insert(ekey(T->ea[size_t(e)], T->eb[size_t(e)])); off.insert({T->ea[size_t(e)], T->ea[size_t(e)]}); off.insert({T->eb[size_t(e)], T->eb[size_t(e)]}); }
  if (tKey() == 'v') for (int v : tsel_.v) off.insert({v, v});   // (the pinned points among them too)
  QJsonArray a, keep; int n = 0; for (const QJsonValue& x : it->raw.value("uvPins").toArray()) { const QJsonArray p = x.toArray(); if (p.size() >= 2 && off.count(ekey(p[0].toInt(), p[1].toInt()))) { n++; continue; } keep.append(x); }
  if (!n) { ui::toast(tr("None of these edges is pinned")); return "!"; }
  if (keep.isEmpty()) it->raw.remove("uvPins"); else it->raw.insert("uvPins", keep);
  stepLabel_ = tr("UV pins off"); scene_->revision++; commit(false); uvChanged(); emit topoState();
  ui::toast(tr("%1 pin%2 off").arg(n).arg(n == 1 ? "" : "s")); return {};
}

// the preview: the shell under the pointer in 3D (cut along the seams) as Unwrap would lay it — in its place
const std::vector<std::array<float, 2>>& Viewport::uvPreviewLines() {
  static const std::vector<std::array<float, 2>> none;
  const int am = topoMesh();
  if (am < 0 || thov_.m != am || room() != "topo" || !tset.uv || !uvPaneShown()) return none;
  const Item* it = scene_->item(am); auto B = baseSp(*it); auto T = topo::ttopo(B); if (!T) return none;
  int f0 = thov_.f; if (f0 < 0 && thov_.e >= 0 && thov_.e < T->ne) f0 = T->ef0[size_t(thov_.e)];
  if (f0 < 0 && thov_.v >= 0 && thov_.v < T->n && T->vfo[size_t(thov_.v)] < T->vfo[size_t(thov_.v) + 1]) f0 = T->vF[size_t(T->vfo[size_t(thov_.v)])];
  if (f0 < 0 || f0 >= T->nf) return none;
  // (the shells along the seams: once per mesh / seams)
  if (uvPrev_.B != B.get() || uvPrev_.stamp != uvStamp_) {
    uvPrev_ = {}; uvPrev_.B = B.get(); uvPrev_.stamp = uvStamp_; const auto seam = uv::seamFlags(*B, *T);
    uvPrev_.shell.assign(size_t(T->nf), -1); int s = 0;
    for (int f = 0; f < T->nf; f++) { if (uvPrev_.shell[size_t(f)] >= 0) continue; std::vector<int> st{f}; uvPrev_.shell[size_t(f)] = s;
      while (!st.empty()) { int x = st.back(); st.pop_back(); for (int k = T->fo[size_t(x)]; k < T->fo[size_t(x) + 1]; k++) { const int e = T->ce[size_t(k)]; if (seam[size_t(e)]) continue; const int g = T->ef0[size_t(e)] == x ? T->ef1[size_t(e)] : T->ef0[size_t(e)];
        if (g >= 0 && uvPrev_.shell[size_t(g)] < 0) { uvPrev_.shell[size_t(g)] = s; st.push_back(g); } } }
      s++; }
  }
  const int sh = uvPrev_.shell[size_t(f0)]; if (sh == uvPrev_.cur) return uvPrev_.lines;
  uvPrev_.cur = sh; uvPrev_.lines.clear();
  std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (uvPrev_.shell[size_t(f)] == sh) fs.push_back(f);
  if (fs.size() > 60000) return uvPrev_.lines;                       // (too big to be quick)
  if (uv::has(*B)) if (const UvCache* uc = uvCache(); uc && uc->base.lock() == B) {   // (laid out already as Unfold would lay it — one UV shell, the same polygons: nothing new to show)
    const int s0 = uc->S.of[size_t(fs[0])]; bool same = true; for (int f : fs) if (uc->S.of[size_t(f)] != s0) { same = false; break; }
    if (same) { size_t n = 0; for (int f = 0; f < T->nf; f++) if (uc->S.of[size_t(f)] == s0) n++; if (n == fs.size()) return uvPrev_.lines; } }
  PolyMesh c = *B; const uv::Pins pins = uvPinsOf(am);
  uv::SymUnfold su; if (uvo.symUnfold) { if (uvPrev_.mc.size() != T->fv.size()) uvPrev_.mc = uv::mirrorCorners(*B, *T); su.mc = uvPrev_.mc; }
  const uv::PackOpts po = uvPackOpts(am); const auto feats = uvFeaturesOf(am); uv::unfold(c, *T, fs, true, false, &pins, uvo.symUnfold ? &su : nullptr, &po, &feats);   // (along the seams marked — as Unfold does it, symmetric too)
  if (!uv::has(c)) return uvPrev_.lines;
  for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; uvPrev_.lines.push_back(c.cornerUV[size_t(k)]); uvPrev_.lines.push_back(c.cornerUV[size_t(nk)]); }
  return uvPrev_.lines;
}

}  // namespace hm

namespace hm {
// ---------- the face's features: lips and lids whose points face each other ----------
std::vector<uv::Feature> Viewport::uvFeaturesOf(int m) const {
  std::vector<uv::Feature> out; const Item* it = scene_->item(m); if (!it) return out;
  const int n = baseOf(*it) ? int(baseOf(*it)->pos.size()) : 0;
  for (const QJsonValue& x : it->raw.value("uvFeatures").toArray()) {
    const QJsonObject o = x.toObject(); uv::Feature f; bool ok = true;
    for (auto [key, c] : {std::pair<const char*, std::vector<int>*>{"a", &f.a}, {"b", &f.b}}) for (const QJsonValue& v : o.value(key).toArray()) { const int i = v.toInt(-1); if (i < 0 || i >= n) ok = false; c->push_back(i); }
    f.closed = o.value("c").toBool(); f.straight = o.value("s").toBool(true);
    if (ok && f.a.size() >= 2 && f.b.size() >= 2) out.push_back(f);
  }
  return out;
}
QString Viewport::uvFeatureKey(int mode) {
  const int am = topoMesh(); Item* it = am >= 0 ? scene_->item(am) : nullptr;
  if (!it) { ui::toast(tr("Select the face's mesh — then its lips' or eye's rim (Edge mode), or the face's polygons")); return "!"; }
  auto B = baseSp(*it); auto T = topo::ttopo(B); if (!T) return "!";
  const bool comp = topoMode != "object" && tsel_.id == am; const char key = tKey();
  std::vector<int> faces = comp ? tSelFaces(am) : std::vector<int>{};
  auto key2 = [](const uv::Feature& f) { return std::minmax(f.a.front(), f.a.back()); };   // (a feature: its two corners)
  auto feats = uvFeaturesOf(am);
  std::vector<uv::Feature> found;
  if (mode == 2) {                                               // (off: those in the selection — none selected: all)
    std::set<int> vs; for (int f : faces) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vs.insert(T->fv[size_t(k)]);
    if (comp && key == 'e') for (int e : tsel_.e) if (e >= 0 && e < T->ne) { vs.insert(T->ea[size_t(e)]); vs.insert(T->eb[size_t(e)]); }
    const size_t was = feats.size();
    feats.erase(std::remove_if(feats.begin(), feats.end(), [&](const uv::Feature& f) { if (vs.empty()) return true; for (int v : f.a) if (vs.count(v)) return true; for (int v : f.b) if (vs.count(v)) return true; return false; }), feats.end());
    if (feats.size() == was) { ui::toast(tr("No lips / lids set here")); return "!"; }
  } else {
    if (comp && key == 'e' && !tsel_.e.empty()) { auto f = uv::featureFromEdges(*B, *T, {tsel_.e.begin(), tsel_.e.end()}); if (!f) { ui::toast(tr("Select one closed edge loop — the lips' or the eye's rim (upper and lower together)")); return "!"; } found.push_back(*f); }
    else { found = uv::detectFeatures(*B, *T, faces); if (found.empty()) { ui::toast(tr("No hole in %1 — select the lips' or the eye's rim (Edge mode), or the face's polygons").arg(faces.empty() ? tr("the mesh") : tr("the selected shells"))); return "!"; } }
    if (tset.sym) { const auto pv = topo::symPairs(*B, B->pos); const size_t n0 = found.size();   // (Symmetry: the other eye too)
      for (size_t i = 0; i < n0; i++) if (auto g = uv::mirrorFeature(found[i], pv)) { bool there = false; for (auto& h : found) if (key2(h) == key2(*g)) there = true; if (!there) found.push_back(*g); } }
    for (auto& f : found) { f.closed = mode == 1; feats.erase(std::remove_if(feats.begin(), feats.end(), [&](const uv::Feature& h) { return key2(h) == key2(f); }), feats.end()); feats.push_back(f); }
  }
  QJsonArray arr; for (auto& f : feats) { QJsonArray a, b; for (int v : f.a) a.append(v); for (int v : f.b) b.append(v); arr.append(QJsonObject{{"a", a}, {"b", b}, {"c", f.closed}, {"s", f.straight}}); }
  if (arr.isEmpty()) it->raw.remove("uvFeatures"); else it->raw.insert("uvFeatures", arr);
  stepLabel_ = mode == 2 ? tr("Lips / lids off") : mode == 1 ? tr("Lips / lids shut") : tr("Lips / lids facing");
  scene_->revision++; uvStamp_++; commit(false); uvChanged(); emit topoState();
  if (mode == 2) { const QString msg = tr("Lips / lids taken off — Unfold lays them free again"); ui::toast(msg); return msg; }
  // (their shells unfolded again at once: the polygons round their points)
  std::set<int> fs; for (auto& f : found) for (auto* c : {&f.a, &f.b}) for (int v : *c) for (int i = T->vfo[size_t(v)]; i < T->vfo[size_t(v) + 1]; i++) fs.insert(T->vF[size_t(i)]);
  const uv::Pins pins = uvPinsOf(am); const uv::PackOpts po = uvPackOpts(am); const bool symU = uvo.symUnfold; const auto all = feats;
  const QString r = uvEdit([&](PolyMesh& m, const topo::TTopo& TT) -> QString {
    uv::SymUnfold su; if (symU) su.mc = uv::mirrorCorners(m, TT);
    const auto u = uv::unfold(m, TT, {fs.begin(), fs.end()}, true, true, &pins, symU ? &su : nullptr, &po, &all);
    return u.done ? QString() : tr("!No shell unfolded — closed ones need a seam");
  });
  const QString msg = tr("%1 %2 — %3: their points %4").arg(found.size()).arg(found.size() == 1 ? tr("rim") : tr("rims")).arg(mode == 1 ? tr("shut") : tr("open"))
                        .arg(mode == 1 ? tr("one on another (painted as closed)") : tr("on one vertical each, the corners level"));
  if (!r.startsWith('!')) ui::toast(msg);
  return r.startsWith('!') ? r : msg;
}
}  // namespace hm
