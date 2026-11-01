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
  double nearPlane() const { return ortho() ? 0.001 * sceneScale : sceneScale * 0.005; }
  double farPlane() const { return ortho() ? sceneScale * 60 : sceneScale * 500; }

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
