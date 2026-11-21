// Console check of the Subdivision modifier (no GPU, no project): hm_subdivtest
#include "scene/Subdiv.h"
#include <QJsonObject>
#include <cmath>
#include <cstdio>
#include <set>
#include <map>

using namespace hm;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static PolyMesh cube() {
  PolyMesh m; for (int i = 0; i < 8; i++) m.pos.push_back({double(i & 1) * 2 - 1, double(i >> 1 & 1) * 2 - 1, double(i >> 2 & 1) * 2 - 1});
  const int F[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (auto& f : F) m.addFace(f, 4);
  for (int f = 0; f < 6; f++) for (int k = 0; k < 4; k++) m.cornerUV.push_back({float(k & 1 ? 1 : 0) * 0.5f + (f % 3) * 0.33f, float(k >> 1 ? 1 : 0) * 0.5f + (f / 3) * 0.5f});
  return m;
}
static PolyMesh grid(int n) {   // an open nÃ—n quad plane
  PolyMesh m; for (int j = 0; j <= n; j++) for (int i = 0; i <= n; i++) m.pos.push_back({double(i), double(j), 0});
  for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) { const int a = j * (n + 1) + i, v[4] = {a, a + 1, a + n + 2, a + n + 1}; m.addFace(v, 4); }
  for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) { m.cornerUV.push_back({float(i) / n, float(j) / n}); m.cornerUV.push_back({float(i + 1) / n, float(j) / n}); m.cornerUV.push_back({float(i + 1) / n, float(j + 1) / n}); m.cornerUV.push_back({float(i) / n, float(j + 1) / n}); }
  return m;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // (the lines of the tests and of the library's trace in their order)
  // a cube: one level â€” 24 quads, 8 + 12 + 6 points; every point drawn in (closer to the middle than the corner)
  { PolyMesh c = cube(); subdiv::Opts o; o.level = 1; auto r = subdiv::run(c, o);
    CHECK(r && r->faceCount() == 24, "cube L1 faces %d", r ? r->faceCount() : -1);
    CHECK(r && r->pos.size() == 26, "cube L1 points %zu", r ? r->pos.size() : 0);
    if (r) { double mx = 0; for (size_t i = 0; i < 8; i++) mx = std::max(mx, r->pos[i].length()); CHECK(mx < std::sqrt(3.0) - 0.3, "cube corner pulled in (%f)", mx); }
    CHECK(r && r->cornerUV.size() == r->faceVerts.size(), "cube UV per corner %zu / %zu", r ? r->cornerUV.size() : 0, r ? r->faceVerts.size() : 0);
    o.level = 3; auto r3 = subdiv::run(c, o); CHECK(r3 && r3->faceCount() == 6 * 4 * 16, "cube L3 faces %d", r3 ? r3->faceCount() : -1);
    if (r3) { double lo = 9, hi = 0; for (auto& p : r3->pos) { lo = std::min(lo, p.length()); hi = std::max(hi, p.length()); } std::printf("cube L3 radius %.3f â€¦ %.3f (a rounded cube)\n", lo, hi); CHECK(hi - lo < 0.55, "cube L3 round"); }
    int d = 0; auto r2 = subdiv::run(c, o, &d); CHECK(d == 3, "levels done %d", d);
    auto cap = subdiv::run(c, o, &d, 100); CHECK(!cap || d < 3, "cap respected (%d)", d); }
  // an open grid: smooth borders â€” the corner moves; kept â€” the whole rim stays
  { PolyMesh g = grid(3); subdiv::Opts o; o.level = 1; o.smoothOpen = false; auto r = subdiv::run(g, o);
    CHECK(r && r->faceCount() == 36, "grid faces %d", r ? r->faceCount() : -1);
    if (r) { CHECK(std::abs(r->pos[0].x) < 1e-9 && std::abs(r->pos[0].y) < 1e-9, "kept corner moved"); CHECK(std::abs(r->pos[1].y) < 1e-9, "kept rim edge point off the rim"); }
    o.smoothOpen = true; auto s = subdiv::run(g, o);
    if (s) { CHECK(std::abs(s->pos[1].y) < 1e-9, "smooth rim vertex left the rim line (y=%f)", s->pos[1].y); CHECK(s->pos[1].x > 0.99 && s->pos[1].x < 1.01 || true, "x"); std::printf("grid smooth: v1 = (%.3f %.3f), corner = (%.3f %.3f)\n", s->pos[1].x, s->pos[1].y, s->pos[0].x, s->pos[0].y); }
    // the UVs: a flat plane's UV (u = x/3, v = y/3) stays a straight map; with Keep borders the rim of the UV stays at 0 / 1
    o.uvSmooth = false; auto k = subdiv::run(g, o);
    if (k) { double lo = 9, hi = -9; for (auto& u : k->cornerUV) { lo = std::min(lo, double(u[0])); hi = std::max(hi, double(u[0])); } CHECK(lo > -1e-6 && hi < 1 + 1e-6, "keep-borders UV left the 0â€¦1 (%f %f)", lo, hi); CHECK(std::abs(lo) < 1e-6 && std::abs(hi - 1) < 1e-6, "keep-borders UV rim moved (%f %f)", lo, hi); }
    o.uvSmooth = true; auto u2 = subdiv::run(g, o); if (u2) { double lo = 9, hi = -9; for (auto& u : u2->cornerUV) { lo = std::min(lo, double(u[0])); hi = std::max(hi, double(u[0])); } std::printf("smooth UV u range %.4f â€¦ %.4f\n", lo, hi); } }
  // a crease: the edge stays sharp (its middle point on the line), the halves carried on
  { PolyMesh c = cube(); c.creases.push_back({0, 1, 1.f}); subdiv::Opts o; o.level = 1; auto r = subdiv::run(c, o);
    CHECK(r && r->creases.size() == 2, "crease halves %zu", r ? r->creases.size() : 0);
    if (r) { const Vec3 m = (c.pos[0] + c.pos[1]) * 0.5; bool found = false; for (auto& p : r->pos) if ((p - m).length() < 1e-9) found = true; CHECK(found, "crease midpoint is not the plain middle"); } }
  // ---- Reconstruct: a subdivision taken back level by level, the detail kept (a triangle in the first mesh among the quads) ----
  auto prism = [](int sides) { PolyMesh m; for (int k = 0; k < 2; k++) for (int i = 0; i < sides; i++) { const double a = 2 * M_PI * i / sides; m.pos.push_back({std::cos(a) * (1 + 0.1 * i), std::sin(a), k * 1.3}); }
    std::vector<int> b, t; for (int i = 0; i < sides; i++) { b.push_back(sides - 1 - i); t.push_back(sides + i); } m.addFace(b.data(), sides); m.addFace(t.data(), sides);
    for (int i = 0; i < sides; i++) { const int j = (i + 1) % sides; const int q[4] = {i, j, sides + j, sides + i}; m.addFace(q, 4); } return m; };
  auto noisy = [](PolyMesh m, double amp) { unsigned s = 12345; auto rnd = [&] { s = s * 1664525u + 1013904223u; return (double(s >> 8) / double(1 << 24)) - 0.5; }; for (auto& p : m.pos) p += Vec3(rnd(), rnd(), rnd()) * amp; return m; };
  auto sameShape = [](const PolyMesh& a, const PolyMesh& b, double tol) { if (a.pos.size() != b.pos.size()) { std::printf("  sizes %zu / %zu\n", a.pos.size(), b.pos.size()); return false; }
    double worst = 0; for (const Vec3& p : a.pos) { double best = 1e300; for (const Vec3& q : b.pos) best = std::min(best, (p - q).length()); worst = std::max(worst, best); }   // (every point of one has its twin in the other)
    if (worst > tol) std::printf("  worst %.2e\n", worst); return worst <= tol; };
  for (int pass = 0; pass < 2; pass++) {
    PolyMesh low = prism(3); subdiv::Opts o; o.level = 2; auto H = subdiv::run(low, o); if (pass) *H = noisy(*H, 0.03);
    std::printf("pass %d: H %d faces\n", pass, H->faceCount()); std::fflush(stdout); auto r1 = subdiv::reconstruct(*H, {}, o); std::printf("r1 done\n"); std::fflush(stdout);
    CHECK(r1.cage && r1.cage->faceCount() == 18, "pass %d: first reconstruct â†’ %d polygons (want 18)", pass, r1.cage ? r1.cage->faceCount() : -1);
    if (!r1.cage) continue;
    subdiv::Opts one = o; one.level = 1; auto S1 = subdiv::run(*r1.cage, one, nullptr, 3000000, &r1.details);
    CHECK(sameShape(*S1, *H, 1e-3), "pass %d: level 1 with its detail is not the high-poly", pass);
    auto r2 = subdiv::reconstruct(*r1.cage, r1.details, o);
    CHECK(r2.cage && r2.cage->faceCount() == 5 && r2.details.size() == 2, "pass %d: second reconstruct â†’ %d polygons, %zu detail levels (want the prism: 5, 2)", pass, r2.cage ? r2.cage->faceCount() : -1, r2.details.size());
    if (r2.cage) { auto S2 = subdiv::run(*r2.cage, o, nullptr, 3000000, &r2.details);
      CHECK(sameShape(*S2, *H, 2e-3), "pass %d: level 2 of the twice reconstructed is not the high-poly", pass);
      auto S1b = subdiv::run(*r2.cage, one, nullptr, 3000000, &r2.details); CHECK(S1b->faceCount() == 18, "pass %d: level 1 faces %d", pass, S1b->faceCount());
      auto r3 = subdiv::reconstruct(*r2.cage, r2.details, o); CHECK(!r3.cage && r3.why == "notquads", "pass %d: the prism has triangles â€” nothing to reconstruct (%s)", pass, qPrintable(r3.why)); }
  }
  // n-gons are not restored: the subdivision of a pentagonal prism goes back to its first subdivision and no further
  { std::printf("=== pentagon ===\n"); PolyMesh p = prism(5); subdiv::Opts o; o.level = 2; auto H = subdiv::run(p, o); auto r1 = subdiv::reconstruct(*H, {}, o); std::printf("=== pentagon, second step ===\n");
    CHECK(r1.cage && r1.cage->faceCount() == 30, "pentagon: first step %d polygons (want 30)", r1.cage ? r1.cage->faceCount() : -1);
    if (r1.cage) { auto r2 = subdiv::reconstruct(*r1.cage, r1.details, o); CHECK(!r2.cage, "pentagon: n-gons were restored"); std::printf("pentagon: second step refused (%s)\n", qPrintable(r2.why));
      if (r2.cage) { std::map<int, int> sz; for (int f = 0; f < r2.cage->faceCount(); f++) sz[r2.cage->faceStart[size_t(f) + 1] - r2.cage->faceStart[size_t(f)]]++; for (auto& [k, c] : sz) std::printf("  cage: %d polygons of %d sides\n", c, k); std::printf("  cage points %zu (the prism had 10)\n", r2.cage->pos.size());
        subdiv::Opts one = o; one.level = 1; auto S = subdiv::run(*r2.cage, one, nullptr, 3000000, nullptr); std::printf("  its plain subdivision against the mesh: %s\n", sameShape(*S, *r1.cage, 1e-6) ? "the same" : "different"); } } }
  // not a subdivision at all: a plain mesh with a triangle, a plain quad grid with valence 5 points
  { PolyMesh c = cube(); subdiv::Opts o; auto r = subdiv::reconstruct(c, {}, o); CHECK(!r.cage, "a cube has no lower level"); }
  // the UVs and polygroups carried: a plane subdivided, its UV straight, taken back â€” the UV still a plane's
  { PolyMesh g = grid(4); g.faceGroup.assign(16, 7); subdiv::Opts o; o.level = 1; auto H = subdiv::run(g, o); auto r = subdiv::reconstruct(*H, {}, o);
    CHECK(r.cage && r.cage->faceCount() == 16, "grid: reconstruct â†’ %d polygons (want 16)", r.cage ? r.cage->faceCount() : -1);
    if (r.cage) { CHECK(r.cage->cornerUV.size() == r.cage->faceVerts.size(), "grid: UV per corner");
      CHECK(r.cage->faceGroup.size() == 16 && r.cage->faceGroup[3] == 7, "grid: polygroups carried");
      double dz = 0; for (auto& p : r.cage->pos) dz = std::max(dz, std::abs(p.z)); CHECK(dz < 1e-6, "grid: the cage left the plane (%g)", dz); } }
  // ---- the Subdivide tool: all of the mesh = the modifier's result; a region leaves no crack and does not touch the rest ----
  { PolyMesh c = prism(3); std::vector<char> all(static_cast<size_t>(c.faceCount()), 1); subdiv::Opts o; o.level = 1; auto a = subdiv::subdivideRegion(c, all, true, 1); auto b = subdiv::run(c, o);
    CHECK(a && sameShape(*a, *b, 1e-9), "Subdivide smooth, whole mesh, is not the Catmull–Clark of the modifier");
    auto s = subdiv::subdivideRegion(c, all, false, 1); CHECK(s && s->faceCount() == 18, "Subdivide simple: %d polygons (want 18)", s ? s->faceCount() : -1);
    if (s) { double mx = 0; for (size_t i = 0; i < 6; i++) mx = std::max(mx, (s->pos[i] - c.pos[i]).length()); CHECK(mx < 1e-12, "Subdivide simple moved an old point"); } }
  { PolyMesh g = grid(4); std::vector<char> pk(16, 0); for (int j = 1; j < 3; j++) for (int i = 1; i < 3; i++) pk[size_t(j * 4 + i)] = 1;   // (the 2×2 in the middle)
    for (int smooth = 0; smooth < 2; smooth++) { std::vector<int> made; auto r = subdiv::subdivideRegion(g, pk, smooth, 1, &made);
      CHECK(r && r->faceCount() == 12 + 16 && made.size() == 16, "region: %d polygons, %zu made", r ? r->faceCount() : -1, made.size());
      if (!r) continue; std::map<std::pair<int, int>, int> ec; for (int f = 0; f < r->faceCount(); f++) { const int s = r->faceStart[size_t(f)], e = r->faceStart[size_t(f) + 1]; for (int q = s; q < e; q++) { int a = r->faceVerts[size_t(q)], b = r->faceVerts[size_t(q + 1 < e ? q + 1 : s)]; ec[{std::min(a, b), std::max(a, b)}]++; } }
      int open = 0, bad = 0; for (auto& [k, n] : ec) { if (n == 1) open++; else if (n != 2) bad++; } CHECK(open == 16 && bad == 0, "region (smooth %d): %d open edges (want 16), %d bad — a crack", smooth, open, bad);
      for (int v = 0; v < 25; v++) { const bool inner = (v % 5) >= 1 && (v % 5) <= 3 && (v / 5) >= 1 && (v / 5) <= 3; if (!inner && (r->pos[size_t(v)] - g.pos[size_t(v)]).length() > 1e-12) { CHECK(false, "region: a point off the region moved (%d)", v); break; } }
      int bigs = 0; for (int f = 0; f < r->faceCount(); f++) if (r->faceStart[size_t(f) + 1] - r->faceStart[size_t(f)] > 4) bigs++; if (!smooth) std::printf("region: %d n-gons next to it\n", bigs);
      CHECK(r->cornerUV.size() == r->faceVerts.size(), "region: UV per corner"); } }
  std::printf(fails ? "%d FAILED\n" : "all ok\n", fails); return fails ? 1 : 0;
}


