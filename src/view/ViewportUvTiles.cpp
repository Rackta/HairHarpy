// UV Editor — every UV Set of the project at once, each in a tile of its own ("All sets as tiles"). Up to 4 tiles in a row,
// the rows as even as can be: 4 sets — 2 × 2, 5 — 3 over 2, 6 — 3 × 2, 9 — 3 × 3, 10 — 4, 4, 2. To look at only: a click on a
// tile works in that set (the tiles closed); the edits wait until then
#include "scene/Uv.h"
#include "scene/Topo.h"
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace hm {

namespace {
constexpr double kTileGap = 0.14;                                  // (between two tiles: room for the set's name)
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
int tileCols(int n) { return std::clamp(int(std::ceil(std::sqrt(double(std::max(1, n))) - 1e-9)), 1, 4); }
}  // namespace

// (the tiles' sets: the project's, in the UV Sets list's order)
std::vector<QString> Viewport::uvTileNames() const { return uvSetNamesAll(); }

std::array<double, 2> Viewport::uvTileOrigin(int i, int n) const {
  const int c = tileCols(n); const int col = i % c, row = i / c;
  return {col * (1 + kTileGap), -row * (1 + kTileGap)};          // (row 0 the 0…1 tile itself; the next ones below it)
}

int Viewport::uvTileAt(const QPointF& mp) const {
  const auto names = uvTileNames(); const int n = int(names.size()); if (!n) return -1;
  const auto uv = pxToUv(mp);
  for (int i = 0; i < n; i++) { const auto o = uvTileOrigin(i, n); if (uv[0] >= o[0] && uv[0] <= o[0] + 1 && uv[1] >= o[1] && uv[1] <= o[1] + 1) return i; }
  return -1;
}

void Viewport::uvTilesFrame() {
  const auto names = uvTileNames(); const int n = std::max(1, int(names.size()));
  double x0 = 0, y0 = 0, x1 = 1, y1 = 1; for (int i = 0; i < n; i++) { const auto o = uvTileOrigin(i, n); x0 = std::min(x0, o[0]); y0 = std::min(y0, o[1]); x1 = std::max(x1, o[0] + 1); y1 = std::max(y1, o[1] + 1); }
  const double W = paneW(), H = paneH(), w = x1 - x0, h = (y1 - y0) + kTileGap;   // (room above for the names)
  cam_.uvPpu = std::clamp(std::min(W / (w * 1.08), H / (h * cam_.uvAsp * 1.08)), 4.0, 400000.0); cam_.uvCx = (x0 + x1) / 2; cam_.uvCy = (y0 + y1) / 2 + kTileGap / 4;
  requestUpdate();
}

void Viewport::setUvTiles(bool on) {
  if (uvTiles == on) return;
  uvTiles = on; uvChanged();
  if (cam_.view == "uv") { if (on) uvTilesFrame(); else uvFrame(); }
  else for (int i = 0; i < 4; i++) if (paneCamera(i).view == "uv") { const int was = act_; activatePane(i); if (on) uvTilesFrame(); else uvFrame(); activatePane(was); break; }
}

// a click on a tile: work in that set — on the active mesh when it has it, else on the first shown mesh that has it
bool Viewport::uvTilePick(const QPointF& mp) {
  const int i = uvTileAt(mp); const auto names = uvTileNames(); if (i < 0 || size_t(i) >= names.size()) return false;
  const QString& want = names[size_t(i)];
  auto setOf = [&](int id) { const Item* it = scene_->item(id); if (!it || !it->poly) return -1; const auto S = uvSets(id); for (size_t k = 0; k < S.size(); k++) if (S[k].name == want) return int(k); return -1; };
  int m = topoMesh(), s = m >= 0 ? setOf(m) : -1;
  if (s < 0) for (int id : topoMeshes()) if ((s = setOf(id)) >= 0) { m = id; break; }
  if (s < 0) for (const auto& [id, it] : scene_->items) if ((s = setOf(id)) >= 0) { m = id; break; }
  uvTiles = false; emit topoState();
  if (s < 0) { ui::toast(tr("%1 has no polygons yet").arg(want)); uvChanged(); uvFrame(); return true; }
  if (!sel_.hasItem(m)) setSelectedItems({m});
  if (!(uvPicked.id == m && uvPicked.set == s && uvPicked.mat == -2)) uvPickSet(m, s);
  uvChanged(); uvFrame(); return true;
}

// the drawing: each tile's frame and tenths, every shown mesh's polygons in the tile of their set (a mesh with no sets: in its one set's)
void Viewport::uvTilesDraw(UvDraw& D) {
  const auto names = uvTileNames(); const int n = int(names.size()); std::map<QString, int> idx; for (int i = 0; i < n; i++) idx[names[size_t(i)]] = i;
  static const QColor hue[] = {QColor(0x4a, 0x8c, 0xe0), QColor(0x3c, 0xb4, 0x6e), QColor(0xd2, 0x8c, 0x3c), QColor(0x9c, 0x64, 0xf0), QColor(0x28, 0xa0, 0xb4), QColor(0xd2, 0x5a, 0x96), QColor(0x8c, 0xa8, 0x3c), QColor(0xc8, 0x50, 0x50)};
  for (int i = 0; i < n; i++) { const auto o = uvTileOrigin(i, n); const float ox = float(o[0]), oy = float(o[1]);
    UvDraw::Lines g; g.c = QColor(0x36, 0x36, 0x36); for (int k = 1; k < 10; k++) { const float t = k / 10.f; g.p.push_back({ox + t, oy}); g.p.push_back({ox + t, oy + 1}); g.p.push_back({ox, oy + t}); g.p.push_back({ox + 1, oy + t}); } D.grid.push_back(g);
    UvDraw::Lines t; t.c = QColor(0x8a, 0x8a, 0x8a); t.w = 1.5f; t.p = {{ox, oy}, {ox + 1, oy}, {ox + 1, oy}, {ox + 1, oy + 1}, {ox + 1, oy + 1}, {ox, oy + 1}, {ox, oy + 1}, {ox, oy}}; D.grid.push_back(t); }
  if (!n) return;
  const int am = topoMesh();
  for (const auto& [id, it] : scene_->items) {
    if (!it.poly || it.type != ItemType::IMesh || !scene_->isVisible(it) || it.raw.value("tref").toBool()) continue;
    const PolyMesh& B = *baseOf(it); if (B.cornerUV.size() != B.faceVerts.size()) continue;
    const bool sets = topo::hasUvSets(B); const auto S = uvSets(id); const auto hid = meshmods::hiddenFaces(it, B);
    std::vector<int> tileOfSet(sets ? B.uvSetNames.size() : 1, -1);
    if (sets) { for (size_t k = 0; k < B.uvSetNames.size(); k++) if (auto f = idx.find(B.uvSetNames[k]); f != idx.end()) tileOfSet[k] = f->second; }
    else if (!S.empty()) { if (auto f = idx.find(S[0].name); f != idx.end()) tileOfSet[0] = f->second; }
    UvDraw::Lines l; l.c = id == am ? QColor(0xd8, 0xdc, 0xe8) : sel_.hasItem(id) ? QColor(0xa8, 0xb0, 0xc8) : QColor(0x78, 0x7c, 0x88); l.w = id == am ? 1.2f : 1.f;
    UvDraw::Tris fill; fill.op = 0.22f;
    for (int f = 0; f < B.faceCount(); f++) { if (!hid.empty() && size_t(f) < hid.size() && hid[size_t(f)]) continue;
      int s = 0; if (sets) { s = size_t(f) < B.faceLive.size() ? B.faceLive[size_t(f)] : -1;
        if (s < 0 || !(B.faceSets[size_t(f)] >> s & 1u)) { s = -1; for (size_t k = 0; k < B.uvSetNames.size(); k++) if (B.faceSets[size_t(f)] >> k & 1u) { s = int(k); break; } } }
      if (s < 0 || size_t(s) >= tileOfSet.size() || tileOfSet[size_t(s)] < 0) continue;
      const int ti = tileOfSet[size_t(s)]; const auto o = uvTileOrigin(ti, n);
      const bool live = !sets || (size_t(f) < B.faceLive.size() && B.faceLive[size_t(f)] == s);   // (shown now: cornerUV; else the set's own copy)
      const auto& U = live || size_t(s) >= B.setUV.size() || B.setUV[size_t(s)].size() != B.cornerUV.size() ? B.cornerUV : B.setUV[size_t(s)];
      auto at = [&](int k) { const auto& p = U[size_t(k)]; return std::array<float, 2>{float(p[0] + o[0]), float(p[1] + o[1])}; };
      const int a = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
      for (int k = a; k < e; k++) { l.p.push_back(at(k)); l.p.push_back(at(k + 1 < e ? k + 1 : a)); }
      for (int k = a + 1; k + 1 < e; k++) { fill.p.push_back(at(a)); fill.p.push_back(at(k)); fill.p.push_back(at(k + 1)); fill.c.push_back(hue[ti % 8]); } }
    if (!fill.p.empty()) D.tris.push_back(std::move(fill));
    if (!l.p.empty()) D.lines.push_back(std::move(l)); }
}

// the sets' names over their tiles (the UV Editor's window, wherever it is)
void Viewport::uvTileLabels(std::vector<ViewLabel>& out) const {
  if (!uvTiles || room() != "topo" || !tset.uv) return;
  const auto names = uvTileNames(); const int n = int(names.size());
  for (int pi : shownPanes()) { const Camera& c = paneCamera(pi); if (c.view != "uv") continue; const QRect r = paneRect(pi); const double W = r.width(), H = r.height(), s = c.uvScale(W, H);
    for (int i = 0; i < n; i++) { const auto o = uvTileOrigin(i, n); const double u = o[0] + 0.5, v = o[1] + 1 + kTileGap * 0.45;
      const QPointF p(r.x() + W / 2 + (u - c.uvCx) * s, r.y() + H / 2 - (v - c.uvCy) * s * c.uvAsp);
      if (r.contains(p.toPoint())) out.push_back({p, names[size_t(i)], QColor(0xd8, 0xdc, 0xe8)}); } }
}

}  // namespace hm
