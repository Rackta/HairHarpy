// Hair flow coordinates (the groundwork of the Paint room's anime hair highlight, after VNShader's Flow Coordinates
// Editor): every hair piece gets a flow — a curve from its root to its tip over a set of its polygons — and from it each
// polygon corner gets two numbers and a direction:
//   u — across the strand, 0…1 (from one edge of the lock to the other, or round it for a tube),
//   v — along it, from the root, in flow units (Settings::unit scene lengths per unit: the same band pattern is the same
//       size on every lock), plus `along` — the same 0…1 from root to tip (tip gradients),
//   tangent — the strand's direction there (what an anisotropic highlight is stretched across).
// The shader reads them as a third UV set (u, v) and a tangent; band phase / scale move and stretch the highlight's band
// along the flow. Flows are made automatically (one per connected piece of a mesh — our locks come from curves, so the
// direction is known) and can be drawn by hand on a piece later (a curve over the picked polygons).
// Core only — no GPU, no viewport: plain data, JSON for the project record, the solver, and the CPU twin of the shading
// (strandSpec / bandMask) the GLSL will mirror.
#pragma once
#include "core/Math.h"
#include "scene/Scene.h"
#include <QJsonObject>
#include <QString>
#include <array>
#include <vector>

namespace hm::flow {

enum class Cross { FitWidth, AroundTube };    // u: across the lock's width (front and back alike) | the angle round it (an automatic flow on a round lock goes round it)

struct Curve {
  std::vector<Vec3> pts;                      // root → tip, on (or near) the polygons
  float weight = 1;                           // (several curves on one flow: how strongly each pulls; kept for the editor)
};

struct Flow {
  QString name;
  bool on = true;
  bool autoMade = false;                      // made by autoFlows (remade with the mesh); a hand-drawn one stays
  std::vector<int> faces;                     // the polygons it covers
  std::vector<Curve> curves;
  Cross cross = Cross::FitWidth;
  float phase = 0, scaleU = 1, scaleV = 1;    // the band: moved along v, stretched across / along
  bool reversed = false;                      // root and tip swapped
};

struct Settings {
  double unit = 0;                            // scene lengths per flow unit; 0 — a quarter of the head's size
  Vec3 seam{0, 0, -1};                        // where u starts round a tube (pointed at the back of the head by default)
  bool autoBridge = true;                     // (VNShader's "auto bridge": gaps between a curve and its polygons closed)
};

struct FlowSet {
  std::vector<Flow> flows;
  Settings s;
  QJsonObject toJson() const;
  static FlowSet fromJson(const QJsonObject& o);
};

// the solved field over a mesh, per polygon corner (parallel to faceVerts)
struct Field {
  std::vector<std::array<float, 2>> uv;       // (u across 0…1, v along in units)
  std::vector<float> along;                   // 0 root … 1 tip
  std::vector<std::array<float, 3>> tangent;  // unit, root → tip
  std::vector<int> faceFlow;                  // per polygon: its flow (−1 — none)
  double unit = 1;                            // the scene length of one unit used
  int covered = 0, tubes = 0;                 // (tubes: flows solved round their lock)
};

// the polygons' connected pieces (polygons sharing a vertex): a piece id per polygon
std::vector<int> pieces(const PolyMesh& m, int* count = nullptr);
// the head's centre the roots are near: the Head item's (or an imported mesh tagged as the head), else the hair's box
// centre raised to its top third
Vec3 headCenter(const Scene& sc);
// a flow curve through a piece: geodesic distance from its tip (the vertex farthest from the root side), its vertices
// binned by it and each bin's centre taken, root → tip; samples — the curve's points
Curve autoCurve(const PolyMesh& m, const std::vector<int>& faces, const Vec3& rootHint, int samples = 24);
// one flow for every piece of the mesh (the hand-drawn flows of `keep` stay, their polygons left out)
FlowSet autoFlows(const PolyMesh& m, const Vec3& rootHint, const FlowSet* keep = nullptr);
// the field: each corner projected onto its polygon's flow curve
Field solve(const PolyMesh& m, const FlowSet& fs, double headSize);

// ---- the shading's CPU twin (the GLSL mirrors these) ----
// Kajiya–Kay: a strand's highlight — bright where the half vector is across the strand; the tangent shifted towards the
// normal moves the band along the strand (shift: −1…1)
Vec3 shiftTangent(const Vec3& t, const Vec3& n, double shift);
double strandSpec(const Vec3& t, const Vec3& v, const Vec3& l, double exponent);
// a stable per-strand jitter from u (the comb-like edge of an anime highlight: each strip of the lock a little off)
double strandJitter(double u, double strips, double seed = 0);
// the band's shape in flow space: a band across the strand at `phase` (v units), `width` wide, its edges cut per strip
double bandMask(double u, double v, double phase, double width, double jag, double strips);

// the project record: an item's flows live in its raw record under "flow"
FlowSet readFlows(const Item& it);
void writeFlows(Item& it, const FlowSet& fs);

}  // namespace hm::flow
