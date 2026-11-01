// Picking in the viewport, as the HTML version's helpers: toScreen (CSS pixels of the viewport), pickAnchor (nearest
// within 10 px), pickCurve (nearest within 7 px of a shown curve's samples), rays against the shown surfaces.
#pragma once
#include "core/Math.h"
#include <QMatrix4x4>
#include <optional>
#include <vector>

namespace hm {

class Camera;

struct ScreenPt { double x, y, z; };                    // z: NDC depth; > 1 — behind the camera / past the far plane

struct Projector {
  QMatrix4x4 vp;                                         // GL clip space (no Vulkan correction)
  double w = 1, h = 1;                                   // the viewport in logical pixels
  Projector(const Camera& cam, double logicalW, double logicalH);
  ScreenPt toScreen(const Vec3& p) const;
};

struct Ray { Vec3 o, d; };
Ray rayAt(const Camera& cam, double x, double y, double logicalW, double logicalH);
std::optional<Vec3> planeHit(const Ray& r, const Vec3& point, const Vec3& normal);

// a triangle soup to cast rays at (one per pickable item)
struct PickMesh { int itemId = 0; const std::vector<Vec3>* pos = nullptr; std::vector<uint32_t> tris; Box3 box; const std::vector<uint32_t>* trisRef = nullptr; };   // (trisRef: kept elsewhere, not copied)
struct RayHit { int itemId; double t; Vec3 point; Vec3 normal; int tri = -1; };   // (normal: the triangle's, unit, as wound; tri — its index in the item's tris)
std::optional<RayHit> raycast(const Ray& r, const std::vector<PickMesh>& meshes);

// the nearest point of a polyline on screen: { distance px, segment index, t along it }
struct CurveHit { double d; int i; double t; };
std::optional<CurveHit> nearestOnScreen(const Projector& P, const std::vector<Vec3>& samples, double x, double y);

bool insidePolygon(const std::vector<std::pair<double, double>>& P, double x, double y);   // even-odd, closes by itself

}  // namespace hm
