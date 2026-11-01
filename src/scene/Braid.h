// ⌇ Braids (item 'braid'): strands woven along a source curve — classic (the outer strand over to the middle), Dutch
// (under), fishtail (many thin strands, each over half of them), rope (twisted round each other) — made as separate
// tubes. Each strand has its own cross-section (a Tube profile: glued points — hard edges, dents — concave). Along the
// braid: width, weave density and thickness at the start, middle and end (rounded between them); stylized — each lobe
// sharpened into a triangle, the strands squashed where they press into each other. The ends: Separate (a connector
// round each strand's end — move it, the tube's end follows), Shared (all the strands gathered into one tube, one
// connector round it) or Point (no connector, the end rounded or sharp). The cocoon: one round the whole braid (the
// tubes' rings on its shared levels) or one round each tube (each tube's rings across itself). Convert to geometry makes
// it a mesh of its own (welded to nothing); Convert to locks — each strand a curve with a Tube modifier (the usual
// pipeline).
#pragma once
#include "scene/Scene.h"
#include "scene/Tubes.h"
#include "scene/Walls.h"
#include <optional>
#include <vector>

namespace hm::braid {

struct Params {
  int strands = 3; QString pattern = "classic";          // classic / dutch / fishtail / rope
  double cross = 12;                                     // crossings along the length (rope: half-turns)
  double width = 0;                                      // across the braid (0 — automatic)
  double depth = 1, size = 1, tight = 0.3, rot = 0;      // depth of the weave ×, strand thickness ×, overlap 0…1, turn round the curve (°)
  double wk[3] = {1, 1, 1}, dk[3] = {1, 1, 1}, tk[3] = {1, 1, 1};   // width, weave density, thickness × at the start / middle / end
  double round = 1;                                      // between those: 0 — straight, 1 — rounded
  double sharp = 0, squash = 0;                          // lobes sharpened into triangles; squashed where strands meet (0…1)
  QString end0 = "separate", end1 = "point";             // separate / shared / point
  double conv = 0.08, tip0 = 0.5, tip1 = 0.5;            // gathering length (share); a point end: 0 — sharp, 1 — round
  QString cocoon = "shared";                             // shared — one round the braid; each — one round each tube
  int prev = 10;                                         // rings per crossing
  bool caps = false, shared = true;                      // tubes' cut ends closed; one profile for every strand
  int lockAnchors = 3; bool lockTip = true; double lockR = 1;   // Convert to locks: anchors per crossing, the point ends collapsed, radius ×
};
Params paramsOf(const Item& b);
std::vector<tube::Profile> profilesOf(const Item& b, int n);   // one per strand (shared — the same one)
std::vector<int> braids(const Scene& sc);
int braidOfCurve(const Scene& sc, int curve);              // −1
int braidOfItem(const Scene& sc, int id);                  // a braid, its folder, its curve or connector → the braid (−1)

struct Built {
  struct Strand { std::vector<std::vector<Vec3>> rings; std::vector<double> r; std::vector<Vec3> mid; };   // rings[i][j]: profile point j, ring i; mid — its centre line
  std::vector<Strand> strands;
  struct Level { Vec3 p, N, B, T; double r; };          // (a shared cocoon's level: its plane — centre, depth, across, along — and how far the strands reach)
  std::vector<Level> lv;
  double width = 0, len = 0; bool each = false;          // (each: a cocoon round each tube)
};
std::optional<Built> build(const Scene& sc, const CurveCache& cc, const Item& b, const Vec3& center, int ringsPerCross = -1);
PolyMesh tubesMesh(const Scene& sc, const CurveCache& cc, const Item& b, const Vec3& center);

QString createBraids(Scene& sc, const CurveCache& cc, Selection& sel, const Vec3& center);
QString makeMesh(Scene& sc, const CurveCache& cc, Selection& sel, int id, const Vec3& center);
// each strand a curve along its middle with a Tube modifier of its profile and thickness, in a group of its own; the
// braid hidden
QString toLocks(Scene& sc, const CurveCache& cc, Selection& sel, int id, const Vec3& center);
// every frame: the end connectors carried along with the braid's ends → the anchors moved
std::vector<int> syncConnectors(Scene& sc, const CurveCache& cc, const Vec3& center);
// the ends' connectors made / reshaped / removed as the end kinds and the braid's size want (an edit step's part)
QString updateConnectors(Scene& sc, const CurveCache& cc, int id, const Vec3& center, bool reset = false);   // (reset: each put back on its outline, in place)
QString endReport(const Scene& sc, const CurveCache& cc, const Item& b, const Vec3& center);   // (tests)

}  // namespace hm::braid
