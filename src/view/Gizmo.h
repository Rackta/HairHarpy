// The transform gizmo (Move E / Rotate R / Scale T), as the HTML version's three TransformControls on one proxy: Move —
// axis arrows, plane squares, the centre (free, in the view plane; an axis facing the camera — up / down along it);
// Rotate — a ring per axis and the outer view ring (Shift — 45° steps); Scale — the axes' cubes, the centre cube
// (uniform: right / up — bigger), Shift — 0.1 steps down to 0 (flat), Ctrl — uniform. World / Local axes, X / Y / Z
// locked (their handles gone, their motion removed). The whole change of a drag is one affine map about the start pivot.
#pragma once
#include "core/Math.h"
#include "view/Camera.h"
#include "view/Picking.h"
#include <QColor>
#include <QString>
#include <optional>
#include <vector>

namespace hm {

class Gizmo {
public:
  enum class Mode { None, Move, Rotate, Scale };
  // a handle: axes "X", "XY", "XYZ" (move: free; scale: uniform), "E" (rotate: the view ring), "D…" (move along a facing axis)
  struct Handle { QString axis; bool valid() const { return !axis.isEmpty(); } };
  struct Line { Vec3 a, b; QColor c; float width; };

  Mode mode = Mode::None;
  Vec3 pivot;
  Quat quat;                                     // the gizmo's axes (world: identity)
  bool lock[3] = {false, false, false};
  double scaleNow[3] = {1, 1, 1};                       // (Scale: the factors of the drag so far, along the gizmo's axes)
  double moveStep = 0;                                   // (Shift while moving: the move in steps of this length along the gizmo's axes; 0 — none)
  Handle hover, active;

  bool visible() const { return mode != Mode::None; }
  double unit(const Camera& cam, double W, double H) const;   // world length of the handles (a constant size on screen)
  std::vector<Line> lines(const Camera& cam, double W, double H) const;
  Handle pick(const Camera& cam, double W, double H, double mx, double my) const;

  // a drag: begin at the pointer, then the change so far (shift / ctrl — the keys now)
  void begin(const Handle& h, const Camera& cam, double W, double H, double mx, double my);
  Affine drag(const Camera& cam, double W, double H, double mx, double my, bool shift, bool ctrl);
  Quat rotationSoFar() const { return rot_; }    // (rotate: the world rotation of the drag)

private:
  Vec3 axisVec(int k) const;
  Vec3 p0_, hit0_; double t0_ = 0, ang0_ = 0, my0_ = 0, mx0_ = 0; Vec3 rotAxis_; Vec3 u0_;
  Quat q0_, rot_;
  int axisK_ = -1;
};

}  // namespace hm
