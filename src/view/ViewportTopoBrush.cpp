// The Topology room's brush, as the Hair room's: its circle (radius) and its hard core (hardness) — Ctrl+RMB drag: ←→
// the radius, ↑↓ the hardness; Ctrl + wheel the radius. RMB — its mode for LMB: Move (what lies within the circle
// dragged — Screen: in the view's plane; Surface: sliding along each mesh's own surface; Topology surface: as Surface,
// only the mesh under the centre), Smooth (each vertex towards its neighbours' middle), Relax (the same, kept on the
// surface: the shape stays, the polygons even out; the stars — vertices of other than four edges — evened too or left),
// Inflate / Shrink (along the surface's normal). Shift — Smooth, Shift+Ctrl — Relax whatever the mode; Ctrl — what lies
// within the circle moved along the surface's normals by the drag. With a
// selection in a component mode only the selected vertices move. Symmetry: only the side the stroke is on is brushed —
// the other side follows mirrored. Snap acts as on any edit; a stroke is one undo step.
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCursor>
#include <cmath>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
// (full strength inside the hard core, then a smooth fall to the rim)
double falloff(double d, double R, double hard) {
  const double r0 = R * std::clamp(hard, 0.0, 0.98); if (d <= r0) return 1;
  double t = std::clamp(1 - (d - r0) / std::max(1e-12, R - r0), 0.0, 1.0); return t * t * (3 - 2 * t);
}
int kindOf(const QString& k) { return k == "smooth" ? 1 : k == "relax" ? 2 : k == "inflate" ? 3 : k == "shrink" ? 4 : 0; }
}  // namespace

// RMB on the brush: its modes (what LMB does)
void Viewport::tbModePie(const QPoint& global) {
  ui::PieSpec s; s.title = tr("Brush");
  auto it = [this](const QString& k, const QString& label, const QString& tip) {
    ui::PieItem i; i.label = (tset.brushKind == k ? "● " : "") + label; i.tip = tip; i.cur = tset.brushKind == k;
    i.act = [this, k, label] { tset.brushKind = k; saveTopoPrefs(); emit topoState(); ui::hud(tr("Brush: %1").arg(label), 1000); }; return i; };
  s.top = {it("move", tr("Move"), tr("Drags what lies within the circle")), it("smooth", tr("Smooth"), tr("Each vertex towards its neighbours' middle (also Shift)")),
           it("relax", tr("Relax"), tr("Smooth kept on the surface — the polygons even out, the shape stays (also Shift+Ctrl)"))};
  s.bottom = {it("inflate", tr("Inflate"), tr("Pushed out along the surface's normal")), it("shrink", tr("Shrink"), tr("Pulled in along the surface's normal"))};
  QWidget* mw = nullptr; for (QWidget* w : QApplication::topLevelWidgets()) if (w->inherits("QMainWindow")) mw = w;
  if (mw) ui::Pie::open(mw, global, s, true);
}

void Viewport::tbPress(const QPointF& mp, bool shift, bool ctrl, int kind, double radPx, int only) {
  const int am = topoMesh();
  const bool selOnly = topoMode != "object" && am >= 0 && tsel_.id == am && (tsel_.f.size() + tsel_.e.size() + tsel_.v.size()) > 0;   // (with the selection: only it)
  std::vector<int> cands;
  if (only >= 0) cands = {only}; else if (selOnly || (!tset.brushAll && am >= 0 && !topoHeavy(am))) cands = {am}; else for (int m : topoMeshes()) if (!topoHeavy(m)) cands.push_back(m);   // (All meshes off: the active one only)
  auto hit = tPick(mp, cands); if (!hit) { ui::hud(tr("Start the stroke on a mesh"), 900); return; }
  if (!selOnly && only < 0 && tset.brushMode == "topo") cands = {hit->m};   // (Topology surface: the mesh under the centre only)
  tb_ = {}; tb_.kind = kind >= 0 ? kind : shift && ctrl ? 2 : shift ? 1 : ctrl ? 6 : kindOf(tset.brushKind); tb_.hit0 = hit->point; tb_.n = cam_.dir(); tb_.p0 = mp;   // (Ctrl: moved along the surface's normals)
  { Projector P = projector(); ScreenPt a = P.toScreen(hit->point); const double u = cam_.sceneScale * 0.01; ScreenPt b = P.toScreen(hit->point + cam_.upVec() * u);   // (the circle's radius in the world, at the centre's depth)
    tb_.Rw = (radPx > 0 ? radPx : brushR) * u / std::max(1e-9, std::hypot(b.x - a.x, b.y - a.y)); }
  for (int m : cands) {
    Item* it = scene_->item(m); if (!it || !baseOf(*it)) continue;
    TBM t; t.M = meshmods::meshMatrix(*scene_, *it); t.Mi = t.M.inverse(); t.base = baseOf(*it)->pos; t.uv0 = baseOf(*it)->cornerUV;
    if (tset.sym) { t.pairs = symFirstPairs(m, t.base); t.side = t.Mi.apply(hit->point).x >= 0 ? 1 : -1; }
    if (tset.brushMode != "screen" || tb_.kind == 2 || tb_.kind == 5) t.surf = bvhOf(m, false);   // (the surface as it is now: slid along / kept on)
    std::vector<int> allow; if (selOnly) allow = tSelVerts(m);
    if (tb_.kind == 0 || tb_.kind == 6) {                         // (the move: who and how much, once — at the press)
      std::vector<char> ok(t.base.size(), allow.empty() ? 1 : 0); for (int v : allow) ok[size_t(v)] = 1;
      for (size_t v = 0; v < t.base.size(); v++) { if (!ok[v] || t.offSide(int(v))) continue; double d = t.M.apply(t.base[v]).distanceTo(hit->point); if (d < tb_.Rw) { t.vs.push_back(int(v)); t.w.push_back(falloff(d, tb_.Rw, brushHard)); } }
      if (t.vs.empty()) continue;
      if (tb_.kind == 6) { const PolyMesh& G = *baseOf(*it); t.nrm.assign(t.vs.size(), Vec3()); std::vector<Vec3> vn(G.pos.size());   // (each one's normal, from its polygons — as at the press)
        for (int fc = 0; fc < G.faceCount(); fc++) { const int s = G.faceStart[size_t(fc)], e = G.faceStart[size_t(fc) + 1]; Vec3 n;
          for (int k = s; k < e; k++) { const Vec3& a = G.pos[size_t(G.faceVerts[size_t(k)])]; const Vec3& b = G.pos[size_t(G.faceVerts[size_t(k + 1 < e ? k + 1 : s)])]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
          for (int k = s; k < e; k++) vn[size_t(G.faceVerts[size_t(k)])] += n; }
        for (size_t i = 0; i < t.vs.size(); i++) { Vec3 n = t.M.lin(vn[size_t(t.vs[i])]); t.nrm[i] = n.lengthSq() > 0 ? n.normalized() : Vec3(); tb_.nC += t.nrm[i] * t.w[i]; } }
    } else t.vs = allow;                                          // (the others: the selection, else all — under the circle)
    tb_.meshes[m] = std::move(t);
  }
  if (tb_.meshes.empty()) { ui::hud(tr("Nothing under the brush"), 900); return; }
  td_ = {}; td_.type = 8; drag_ = Drag::Topo;
  const char* names[] = {"", "Smooth", "Relax", "Inflate", "Shrink", "Pinch", "Along the normals"};
  ui::hud(tb_.kind ? tr(names[tb_.kind]) : tset.brushMode == "screen" ? tr("Move (screen)") : tset.brushMode == "surface" ? tr("Move along the surface") : tr("Move along the surface — this mesh only"));
  if (tb_.kind) tbMove(mp);
}

void Viewport::tbMove(const QPointF& mp) {
  overlayRev_++;                                                // (the brush's circles follow the pointer)
  const bool softWas = tset.soft; tset.soft = false;            // (the brush's own falloff — not the soft selection's)
  if (tb_.kind == 6) {                                           // (Ctrl: the drag along the normal under the centre — each vertex out along its own)
    // (the drag on the screen along where the normal points there; the normal turned to the view — up: out of the surface)
    const Vec3 nc = tb_.nC.lengthSq() > 0 ? tb_.nC.normalized() : Vec3(); Projector Pr = projector(); const ScreenPt s0 = Pr.toScreen(tb_.hit0), s1 = Pr.toScreen(tb_.hit0 + nc * tb_.Rw);
    const double dx = mp.x() - tb_.p0.x(), dy = mp.y() - tb_.p0.y(), sx = s1.x - s0.x, sy = s1.y - s0.y, sl = std::hypot(sx, sy), rpx = std::max(1e-9, tb_.Rw / std::max(1e-12, worldPerPixelAt(tb_.hit0)));
    double amt = 0; if (sl > rpx * 0.35) amt = (dx * sx + dy * sy) / sl / sl * tb_.Rw;
    else { const double toward = nc.dot(cam_.ortho() ? cam_.dir() * -1.0 : (cam_.pos - tb_.hit0)); amt = -dy * worldPerPixelAt(tb_.hit0) * (toward >= 0 ? 1 : -1); }
    for (auto& [m, t] : tb_.meshes) { std::vector<Vec3> P = t.base;
      for (size_t i = 0; i < t.vs.size(); i++) { const int v = t.vs[i]; P[size_t(v)] = t.Mi.apply(t.M.apply(t.base[size_t(v)]) + t.nrm[i] * (amt * t.w[i])); }
      { const bool s0 = tset.snap; tset.snap = false; topoAfterMove(m, P, t.vs, t.base, tset.sym ? &t.pairs : nullptr); tset.snap = s0; } topoLive(m, P); }   // (off the surface on purpose: no snap back onto it)
    tset.soft = softWas; tb_.moved = true; requestUpdate(); ui::hud(tr("Along the normals: %1").arg(amt, 0, 'g', 3)); return; }
  if (tb_.kind == 0) {
    auto h = planeHit(ray(mp), tb_.hit0, tb_.n); if (!h) { tset.soft = softWas; return; }
    const Vec3 delta = *h - tb_.hit0;
    for (auto& [m, t] : tb_.meshes) {
      std::vector<Vec3> P = t.base;
      for (size_t i = 0; i < t.vs.size(); i++) { const int v = t.vs[i]; Vec3 w = t.M.apply(t.base[size_t(v)]) + delta * t.w[i];
        if (t.surf) { double d; if (auto q = t.surf->closest(w, nullptr, &d)) w = *q; }   // (Surface: slid along it)
        P[size_t(v)] = t.Mi.apply(w); }
      topoAfterMove(m, P, t.vs, t.base, tset.sym ? &t.pairs : nullptr);
      if (tset.keepUv && !t.uv0.empty()) { auto U = uvKeep(m, t.base, t.uv0, P); topoLive(m, P, &U); } else topoLive(m, P);
    }
  } else {
    std::vector<int> ids; for (auto& [m, t] : tb_.meshes) ids.push_back(m);
    auto hit = tPick(mp, ids);
    if (hit) for (auto& [m, t] : tb_.meshes) {
      Item* it = scene_->item(m); if (!it || !baseOf(*it)) continue;
      auto T = topo::ttopo(baseSp(*it)); const std::vector<Vec3> Pc = baseOf(*it)->pos; std::vector<Vec3> P = Pc;
      std::vector<char> ok(Pc.size(), t.vs.empty() ? 1 : 0); for (int v : t.vs) ok[size_t(v)] = 1;
      std::vector<Vec3> vn;                                       // (inflate / shrink: the vertices' normals, from their polygons)
      if (tb_.kind >= 2) { vn.assign(Pc.size(), Vec3());
        for (int f = 0; f < T->nf; f++) { const int s = T->fo[size_t(f)], e = T->fo[size_t(f) + 1]; Vec3 n;
          for (int k = s; k < e; k++) { const Vec3& a = Pc[size_t(T->fv[size_t(k)])]; const Vec3& b = Pc[size_t(T->fv[size_t(k + 1 < e ? k + 1 : s)])];
            n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
          for (int k = s; k < e; k++) vn[size_t(T->fv[size_t(k)])] += n; } }
      std::vector<int> moved; const double str = std::clamp(tset.brushStr, 0.02, 1.0);
      for (int v = 0; v < T->n; v++) {
        if (!ok[size_t(v)] || t.offSide(v)) continue;
        const Vec3 wv = t.M.apply(Pc[size_t(v)]); const double d = wv.distanceTo(hit->point); if (d >= tb_.Rw) continue;
        const int s = T->voff[size_t(v)], e = T->voff[size_t(v) + 1]; if (e - s < 2) continue;
        const double f = falloff(d, tb_.Rw, brushHard);
        Vec3 w;
        if (tb_.kind == 3 || tb_.kind == 4) {                      // (along the normal, a little per step)
          Vec3 n = t.M.lin(vn[size_t(v)]); if (n.lengthSq() <= 0) continue; n = n.normalized();
          w = wv + n * ((tb_.kind == 3 ? 1 : -1) * f * str * tb_.Rw * 0.04);
        } else {
          bool rim = false; for (int j = s; j < e; j++) if (T->ef1[size_t(T->vE[size_t(j)])] < 0) rim = true;
          if (rim) continue;                                      // (the mesh's border stays — else it shrinks)
          if (tb_.kind == 2 && !tset.relaxStars && e - s != 4) continue;   // (relax, the stars left in place)
          Vec3 avg; for (int j = s; j < e; j++) avg += t.M.apply(Pc[size_t(T->other(T->vE[size_t(j)], v))]); avg = avg * (1.0 / (e - s));
          Vec3 dl = tb_.kind == 5 ? (hit->point - wv) * 0.15 : avg - wv;   // (pinch: towards the centre)
          if (tb_.kind >= 2) { Vec3 n = t.M.lin(vn[size_t(v)]); if (n.lengthSq() > 0) { n = n.normalized(); dl = dl - n * dl.dot(n); } }   // (relax / pinch: along the surface only — not off it)
          w = wv + dl * (f * str);
          if ((tb_.kind == 2 || tb_.kind == 5) && t.surf) { double dd; if (auto q = t.surf->closest(w, nullptr, &dd)) w = *q; }   // (relax: kept on the surface)
        }
        P[size_t(v)] = t.Mi.apply(w); moved.push_back(v);
      }
      if (moved.empty()) continue;
      topoAfterMove(m, P, moved, Pc, tset.sym ? &t.pairs : nullptr);
      topoLive(m, P);
    }
  }
  tset.soft = softWas;
  tb_.moved = true; requestUpdate();
}

void Viewport::tbRelease() {
  overlayRev_++; ui::hud({});
  const bool moved = tb_.moved; std::vector<int> ids; for (auto& [m, t] : tb_.meshes) ids.push_back(m);
  tb_ = {};
  if (!moved) return;
  for (int m : ids) if (Item* it = scene_->item(m); it && baseOf(*it)) topoWrite(m, baseSp(*it));
  commit(false); emit topoState();
}

}  // namespace hm
