// The viewport camera, as in the HTML version: a perspective (fov 40) or orthographic camera with a pivot; Alt+LMB orbits
// around the pivot with no roll, MMB pans, Alt+RMB dollies, the wheel zooms toward the cursor; fixed ortho views.
#pragma once
#include "core/Math.h"
#include <QMatrix4x4>
#include <algorithm>
#include <QString>
#include <optional>

namespace hm {

class Camera {
public:
  QString view = "persp";               // persp | ortho | front | right | left | back | top | uv (the UV Editor: no 3D)
  Vec3 pos{0, 0, 5};
  Quat quat;
  Vec3 up{0, 1, 0};
  Vec3 pivot;
  double fov = 40, zoom = 1;
  bool freeRoll = false;                 // (the view rolled by hand — the Paint room's ↺ ↻: an orbit keeps the roll until the view is made upright again)
  bool trackball = false;                // (an orbit turns round the view's own axes — free, it can roll; off: turntable)
  double sceneScale = 1, orthoHalf = 0.75;
  Vec3 sceneCenter;
  int width = 1, height = 1;
  // the UV Editor's view: the UV at the window's middle, logical px per UV unit (0 — the tile fitted)
  double uvCx = 0.5, uvCy = 0.5, uvPpu = 0, uvAsp = 1;   // (uvAsp: the texture's height to width — V drawn that much shorter, a texel square on screen)
  double uvScale(double paneW, double paneH) const { return uvPpu > 0 ? uvPpu : std::max(8.0, std::min(paneW, paneH) * 0.8); }

  bool ortho() const { return view != "persp"; }
  Vec3 dir() const { return quat.rotate({0, 0, -1}); }
  Vec3 right() const { return quat.rotate({1, 0, 0}); }
  Vec3 upVec() const { return quat.rotate({0, 1, 0}); }
  // (the clip planes: by the scene's size, and by how far the camera is from what it looks at — 20.11: hair brought from a
  // project of another scale lay thousands of units off a head of 1; framed, it was cut away by a far plane made for the head)
  double pivotDist() const { return (pos - pivot).length(); }
  // (everything the scene holds — the views stand outside it and see all of it: 20.11, a project whose head is 1 unit across and
  // its model thousands: the flat views stood 20 units off the head, inside the model, and cut it in half)
  Box3 world;
  double worldReach(const Vec3& c) const {                  // (the farthest corner of what the scene holds from c)
    if (world.empty()) return 0; double r = 0;
    for (int k = 0; k < 8; k++) r = std::max(r, (Vec3(k & 1 ? world.max.x : world.min.x, k & 2 ? world.max.y : world.min.y, k & 4 ? world.max.z : world.min.z) - c).length());
    return r; }
  double nearPlane() const { return ortho() ? 0.001 * sceneScale : std::max(sceneScale * 0.005, pivotDist() * 0.0005); }
  double farPlane() const { return ortho() ? std::max({sceneScale * 60, pivotDist() * 4, worldReach(pos) * 1.2}) : std::max({sceneScale * 500, pivotDist() * 50, worldReach(pos) * 1.2}); }

  QMatrix4x4 viewMatrix() const;
  QMatrix4x4 projMatrix() const;         // GL convention; the renderer applies Vulkan's clip correction

  void fitToBox(const Box3& box);        // frame a new model (sets scale, centre, resets the view)
  void reset();
  void setView(const QString& mode);
  void lookAt(const Vec3& target);
  void level();
  void orbit(double dx, double dy);
  void pan(double dx, double dy);
  void dolly(double amount);
  // steps > 0: toward the cursor (hitDist: what's under it); a fraction of a notch — a smooth / high-resolution wheel
  void wheel(double x, double y, double steps, std::optional<double> hitDist = std::nullopt);
  double lastWheelDist = 0;                              // (nothing under the cursor: the zoom goes on at the rate it had)
  void frame(const Box3& box, bool small);
  double worldPerPixel() const;
  Vec3 rayDir(double x, double y) const;       // the view ray through a viewport pixel
};

}  // namespace hm
