// ▬ Flat lock (Curve tool → Lock): the curve is the middle line of a flat ribbon. raw `rib` = { k: { anchorId: { w, r } }
// — half width and rotation (°) at that anchor; smooth — width / rotation evened out along the lock; tip — share of the
// length narrowing to a point; view — the camera direction when drawn (the ribbon faces it); ends — started from anchors
// of other curves (2 — the root spans them, 3+ — a volume branch through them); ext / offL / offR / glue — painted
// weights (Extrude, Offset of each extruded edge, Glue the top face) with extInv, extH, extFade, offFade }.
// Ported from the HTML version (ribFrame, ribRings, ribExtShape, paintRib, ribConvert …).
#pragma once
#include "scene/Display.h"
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <QString>
#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace hm::rib {

const int MASK_N = 96;
bool isRib(const Item& c);
std::vector<int> ribEnds(const Scene& sc, const Item& c);           // (empty: none)
bool isVolume(const Scene& sc, const Item& c);                       // started from 3+ anchors
int ribHost(const Scene& sc, int a);                                 // the curve an end anchor sits on (its trunk)
std::optional<Vec3> trunkDir(const Scene& sc, const std::vector<int>& pair);
struct Root { Vec3 mid, A0, B0; struct P { double x, y, t; }; std::vector<P> prof; double w0 = 0; };
Root ribRoot(const Scene& sc, const std::vector<int>& ends, const std::optional<Vec3>& T0);
std::vector<int> ringOrder(const Scene& sc, const std::vector<int>& list, const std::optional<Vec3>& T0);

struct Frame { Vec3 p, A, T; double w, s; };
struct Shape {
  std::vector<Frame> F; std::optional<Root> root;
  std::vector<std::vector<Vec3>> rings;      // a volume branch / an extruded lock: rings of M points; a flat ribbon: [L, R]
  bool tube = false, extruded = false;       // tube: the rings are closed around (a volume branch, an extruded lock)
  std::vector<double> e, g;                  // (extruded: the extrusion and the glue along the lock)
};
// the camera's direction now (a ribbon without its own `view` faces it); scale — S.scale
std::optional<Shape> ribShape(const Scene& sc, const CurveCache& cc, const Item& c, const Vec3& camDir, double scale);
// the vertex tint of the lock: the active brush's weights (kind: ext / off / glue, "" — none)
std::vector<std::array<float, 3>> ribColors(const Item& c, const Shape& s, const QString& brushKind);

// the root of a lock started from anchors follows their centre → the anchor moved
bool followEnds(Scene& sc, Item& c);
// the brush: paints the weights of the given flat locks under the circle (weightAt: screen weight of a 3D point, -1 —
// out); op paint / invert / erase / smooth → changed
bool paintRib(Scene& sc, const CurveCache& cc, const std::vector<int>& locks, const std::vector<Shape*>& shapes, const QString& kind, const QString& op,
              double amt, double strength, double intensity, const std::function<double(const Vec3&)>& weightAt);
QString convert(Scene& sc, const CurveCache& cc, Selection& sel, int c, const Vec3& camDir, double scale);

}  // namespace hm::rib
