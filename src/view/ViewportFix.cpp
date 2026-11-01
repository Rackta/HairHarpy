// FixMesh: a mesh put right — its collapsed polygons (no area left, or a sliver of no width) and its doubled ones taken
// out; the parts welded together where they should not be (a vertex or an edge shared by parts that only touch there —
// the upper and the lower teeth sewn along their bite) cut apart; each part's polygons turned one way; the separate
// parts made meshes of their own (an option)
#include "scene/MeshMods.h"
#include "scene/TopoEdit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>

namespace hm {

using tedit::MeshEd;

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
const PolyMesh* baseOf(const Item& m) { return baseSp(m).get(); }
std::pair<int, int> ek(int a, int b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }

// the polygons by their edges: each edge's polygons
std::map<std::pair<int, int>, std::vector<int>> edgeFaces(const MeshEd& E) {
  std::map<std::pair<int, int>, std::vector<int>> o;
  for (size_t f = 0; f < E.f.size(); f++) { const auto& v = E.f[f].v; for (size_t k = 0; k < v.size(); k++) o[ek(v[k], v[(k + 1) % v.size()])].push_back(int(f)); }
  return o;
}
}  // namespace

QString Viewport::tFixMesh() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || topoHeavy(m)) { ui::toast(tr("Select a mesh to fix")); return "!"; }
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); if (E.f.empty()) { ui::toast(tr("The mesh has no polygons")); return "!"; }
  double el = 0; int ne = 0; for (auto& F : E.f) for (size_t k = 0; k < F.v.size(); k++) { el += E.pos[size_t(F.v[k])].distanceTo(E.pos[size_t(F.v[(k + 1) % F.v.size()])]); ne++; }
  el = ne ? el / ne : 1;
  // (0) vertices in one place welded (a file's seams left open: its polygons apart though they meet) — the parts then
  // known by how they really join
  int welded = 0;
  if (ttools.fixWeld) { const double q = std::max(1e-12, el * 1e-4); std::map<std::array<long long, 3>, int> at; std::vector<int> to(E.pos.size(), -1); std::vector<char> used(E.pos.size(), 0);
    for (auto& F : E.f) for (int v : F.v) used[size_t(v)] = 1;
    for (size_t v = 0; v < E.pos.size(); v++) { if (!used[v]) continue; const Vec3& p = E.pos[v]; const std::array<long long, 3> k{std::llround(p.x / q), std::llround(p.y / q), std::llround(p.z / q)};
      auto [i, fresh] = at.insert({k, int(v)}); if (!fresh) { to[v] = i->second; welded++; } }
    if (welded) E.weld(to); }
  // (the pieces as they were: polygons joined by any vertex they share — only a piece cut apart here is made meshes; the
  // pieces that were apart already stay in this one)
  std::vector<int> orig(E.f.size(), -1);
  { std::vector<int> up(E.pos.size()); std::iota(up.begin(), up.end(), 0); std::function<int(int)> root = [&](int x) { while (up[size_t(x)] != x) x = up[size_t(x)] = up[size_t(up[size_t(x)])]; return x; };
    for (auto& F : E.f) for (size_t k = 1; k < F.v.size(); k++) up[size_t(root(F.v[k]))] = root(F.v[0]);
    for (size_t f = 0; f < E.f.size(); f++) orig[f] = root(E.f[f].v[0]); }
  auto dropOrig = [&](const std::vector<char>& del) { std::vector<int> o; for (size_t f = 0; f < orig.size(); f++) if (!del[f]) o.push_back(orig[f]); orig = std::move(o); };
  // (1) collapsed polygons: a corner twice, no area, or a sliver — next to no width for an edge of the mesh and for its own length
  int collapsed = 0;
  { std::vector<char> del(E.f.size(), 0);
    for (size_t f = 0; f < E.f.size(); f++) { const auto& v = E.f[f].v; std::set<int> u(v.begin(), v.end());
      double longest = 0; for (size_t k = 0; k < v.size(); k++) longest = std::max(longest, E.pos[size_t(v[k])].distanceTo(E.pos[size_t(v[(k + 1) % v.size()])]));
      const double area = E.normal(int(f)).length() * 0.5, wide = longest > 0 ? area / longest : 0;   // (wide: about half its height across the longest side)
      if (u.size() < 3 || area <= el * el * 1e-6 || (wide < el * 0.03 && wide < longest * 0.02)) { del[f] = 1; collapsed++; } }
    if (collapsed) { E.removeFaces(del); dropOrig(del); } }
  // (2) doubled polygons and walls: the same corners' places (shared vertices or not) — one kept
  int doubled = 0;
  if (ttools.fixDoubles) { const double q = std::max(1e-12, el * 1e-4); std::set<std::vector<std::array<long long, 3>>> seen; std::vector<char> del(E.f.size(), 0);
    for (size_t f = 0; f < E.f.size(); f++) { std::vector<std::array<long long, 3>> k; for (int v : E.f[f].v) { const Vec3& p = E.pos[size_t(v)]; k.push_back({std::llround(p.x / q), std::llround(p.y / q), std::llround(p.z / q)}); }
      if (!seen.insert(tedit::cycleKey(k)).second) { del[f] = 1; doubled++; } }
    if (doubled) { E.removeFaces(del); dropOrig(del); } }
  if (ttools.fixDoubles) { MeshEd E0 = E; const int c = tedit::dropCovered(E);   // (a polygon over its own split — a hidden double wall)
    if (c) { std::vector<char> del(E0.f.size(), 0); size_t j = 0; for (size_t f = 0; f < E0.f.size(); f++) { if (j < E.f.size() && E.f[j].v == E0.f[f].v) j++; else del[f] = 1; } dropOrig(del); doubled += c; } }
  // (2b) non-manifold sides (three polygons or more on one) split into pairs, vertices where parts only touch split —
  // as Clean the geometry does; the pieces then known again by where their vertices lie
  int nonMan = 0;
  if (ttools.fixManifold) { PolyMesh tmp = E.to(B); nonMan = tedit::makeManifold(tmp);
    if (nonMan) { E = MeshEd::from(tmp); const double q = std::max(1e-12, el * 1e-4); std::map<std::array<long long, 3>, int> key; std::vector<int> up(E.pos.size()); std::iota(up.begin(), up.end(), 0);
      std::function<int(int)> root = [&](int x) { while (up[size_t(x)] != x) x = up[size_t(x)] = up[size_t(up[size_t(x)])]; return x; };
      for (size_t v = 0; v < E.pos.size(); v++) { const Vec3& p = E.pos[v]; auto [i, fresh] = key.insert({{std::llround(p.x / q), std::llround(p.y / q), std::llround(p.z / q)}, int(v)}); if (!fresh) up[size_t(root(int(v)))] = root(i->second); }
      for (auto& F : E.f) for (size_t k = 1; k < F.v.size(); k++) up[size_t(root(F.v[k]))] = root(F.v[0]);
      orig.assign(E.f.size(), -1); for (size_t f = 0; f < E.f.size(); f++) orig[f] = root(E.f[f].v[0]); } }
  // (3) parts that only touch: round each vertex its polygons joined across the edges at it that two polygons share (an
  // edge of three or more — not a join); each fan of them beyond the first takes a copy of the vertex
  int unwelded = 0;
  { const auto ef = edgeFaces(E); std::vector<std::vector<int>> vf(E.pos.size()); for (size_t f = 0; f < E.f.size(); f++) for (int v : E.f[f].v) vf[size_t(v)].push_back(int(f));
    const size_t nv0 = E.pos.size(); std::vector<std::vector<int>> F0; for (auto& F : E.f) F0.push_back(F.v);   // (F0: the corners as they were — the sides looked up by them, not by the copies made on the way)
    for (size_t v = 0; v < nv0; v++) { const auto& fs = vf[v]; if (fs.size() < 2) continue;
      std::map<int, int> up; for (int f : fs) up[f] = f; std::function<int(int)> root = [&](int x) { while (up[x] != x) x = up[x] = up[up[x]]; return x; };
      for (int f : fs) { const auto& V = F0[size_t(f)]; const size_t n = V.size(); for (size_t k = 0; k < n; k++) if (V[k] == int(v)) for (int o : {V[(k + 1) % n], V[(k + n - 1) % n]}) {
            auto e = ef.find(ek(int(v), o)); if (e == ef.end() || e->second.size() != 2) continue; up[root(e->second[0])] = root(e->second[1]); } }
      std::map<int, std::vector<int>> fans; for (int f : fs) fans[root(f)].push_back(f);
      if (fans.size() < 2) continue;
      bool first = true;
      for (auto& [r, fl] : fans) { if (first) { first = false; continue; } const int nvx = E.addVert(E.pos[v]); unwelded++;
        for (int f : fl) for (int& x : E.f[size_t(f)].v) if (x == int(v)) x = nvx; } } }
  // (4) the parts: polygons joined across the edges two of them share; each part's polygons turned the way most of them
  // already face
  const auto ef = edgeFaces(E); std::vector<int> part(E.f.size(), -1); int parts = 0, turned = 0;
  for (size_t s = 0; s < E.f.size(); s++) { if (part[s] >= 0) continue;
    std::vector<int> q{int(s)}, order; part[s] = parts; std::vector<char> flip(E.f.size(), 0);
    while (!q.empty()) { const int f = q.back(); q.pop_back(); order.push_back(f); const auto& V = E.f[size_t(f)].v; const size_t n = V.size();
      for (size_t k = 0; k < n; k++) { const int a = V[k], b = V[(k + 1) % n]; auto e = ef.find(ek(a, b)); if (e == ef.end() || e->second.size() != 2) continue;
        const int g = e->second[0] == f ? e->second[1] : e->second[0]; if (part[size_t(g)] >= 0) continue;
        // (the neighbour runs the shared edge the other way when they agree; as this one shows it — flipped or not)
        const auto& W = E.f[size_t(g)].v; bool same = false; for (size_t j = 0; j < W.size(); j++) if (W[j] == a && W[(j + 1) % W.size()] == b) same = true;
        flip[size_t(g)] = char(same != bool(flip[size_t(f)])); part[size_t(g)] = parts; q.push_back(g); } }
    size_t nf = 0; for (int f : order) nf += flip[size_t(f)]; const bool keepFlipped = nf * 2 > order.size();   // (the most of them as they are)
    if (ttools.fixOrient) for (int f : order) if (bool(flip[size_t(f)]) != keepFlipped) { auto& F = E.f[size_t(f)]; std::reverse(F.v.begin(), F.v.end()); std::reverse(F.uv.begin(), F.uv.end()); std::reverse(F.n.begin(), F.n.end()); turned++; }
    parts++; }
  QStringList did; if (welded) did << tr("%1 vert%2 in one place welded").arg(welded).arg(welded == 1 ? "ex" : "ices");
  if (collapsed) did << tr("%1 collapsed polygon%2 out").arg(collapsed).arg(collapsed == 1 ? "" : "s");
  if (doubled) did << tr("%1 doubled out").arg(doubled);
  if (unwelded) did << tr("%1 vert%2 unwelded").arg(unwelded).arg(unwelded == 1 ? "ex" : "ices");
  if (turned) did << tr("%1 polygon%2 turned").arg(turned).arg(turned == 1 ? "" : "s");
  if (nonMan) did << tr("%1 non-manifold fix%2").arg(nonMan).arg(nonMan == 1 ? "" : "es");
  // (5) a piece cut apart here (the teeth sewn along their bite): its largest part stays this mesh's, each other part a
  // mesh of its own beside it (in world space, looking as this one does)
  std::vector<int> size(size_t(parts), 0), pieceOf(size_t(parts), -1); for (size_t f = 0; f < part.size(); f++) { size[size_t(part[f])]++; pieceOf[size_t(part[f])] = orig[f]; }
  std::map<int, int> biggest; for (int p = 0; p < parts; p++) { auto b = biggest.find(pieceOf[size_t(p)]); if (b == biggest.end() || size[size_t(p)] > size[size_t(b->second)]) biggest[pieceOf[size_t(p)]] = p; }
  std::vector<int> out; for (int p = 0; p < parts; p++) if (biggest[pieceOf[size_t(p)]] != p && size[size_t(p)] >= 3) out.push_back(p);   // (a scrap of a polygon or two stays)
  const bool split = ttools.fixSplit && !out.empty();
  if (did.isEmpty() && !split) { const QString msg = tr("FixMesh: nothing to fix (%1 part%2)").arg(parts).arg(parts == 1 ? "" : "s"); ui::toast(msg); return msg; }
  if (!split) return ttDone(m, E, B, tr("FixMesh: ") + did.join(", "));
  const Affine M = meshmods::meshMatrix(*scene_, *it); const QString name = it->name; const int parent = it->parent; const QJsonObject look = it->raw;
  std::vector<PolyMesh> others;
  for (int p : out) { MeshEd Ep = E; std::vector<char> del(Ep.f.size(), 0); for (size_t f = 0; f < Ep.f.size(); f++) del[f] = part[f] != p; Ep.removeFaces(del);
    PolyMesh d = Ep.to(B); for (auto& x : d.pos) x = M.apply(x); others.push_back(std::move(d)); }
  { const std::set<int> gone(out.begin(), out.end()); std::vector<char> del(E.f.size(), 0); for (size_t f = 0; f < E.f.size(); f++) del[f] = gone.count(part[f]) > 0; E.removeFaces(del); }
  did << tr("%1 part%2 cut apart made mesh%3 of %4 own").arg(others.size()).arg(others.size() == 1 ? "" : "s").arg(others.size() == 1 ? "" : "es").arg(others.size() == 1 ? "its" : "their");
  const QString msg = tr("FixMesh: ") + did.join(", ");
  ttDone(m, E, B, {});
  int k = 1; std::vector<int> made;
  for (const PolyMesh& d : others) { const int id = topoNewMesh(scene_->item(parent) ? parent : -1, QString("%1 · %2").arg(name).arg(++k), d); if (id < 0) continue; made.push_back(id);
    if (Item* ni = scene_->item(id)) for (const char* key : {"look", "smoothN", "twire", "mat", "matPreset", "matOff", "unlit", "pgView"}) if (look.contains(key)) ni->raw.insert(key, look.value(key)); }
  scene_->revision++; emit sceneEdited(); commit(true); emit topoState(); ui::toast(msg);
  return msg;
}

}  // namespace hm
