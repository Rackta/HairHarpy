#include "view/Camera.h"
#include <cmath>

namespace hm {

namespace {
constexpr double DEG = M_PI / 180.0;
bool fixedView(const QString& v) { return v == "front" || v == "right" || v == "left" || v == "back" || v == "top"; }
Vec3 viewDirOf(const QString& v) {
  if (v == "front") return {0, 0, 1};
  if (v == "right") return {1, 0, 0};
  if (v == "left") return {-1, 0, 0};
  if (v == "back") return {0, 0, -1};
  return {0, 1, 0};
}
}  // namespace

QMatrix4x4 Camera::viewMatrix() const {
  QMatrix4x4 m;
  m.lookAt(QVector3D(float(pos.x), float(pos.y), float(pos.z)),
           QVector3D(float(pos.x + dir().x), float(pos.y + dir().y), float(pos.z + dir().z)),
           QVector3D(float(upVec().x), float(upVec().y), float(upVec().z)));
  return m;
}

QMatrix4x4 Camera::projMatrix() const {
  QMatrix4x4 m;
  double aspect = double(width) / std::max(1, height);
  if (!ortho()) m.perspective(float(fov), float(aspect), float(nearPlane()), float(farPlane()));
  else {
    double h = orthoHalf / zoom, w = h * aspect;
    m.ortho(float(-w), float(w), float(-h), float(h), float(nearPlane()), float(farPlane()));
  }
  return m;
}

void Camera::lookAt(const Vec3& target) { quat = Quat::lookRotation(target - pos, up); }

void Camera::level() { up = {0, 1, 0}; quat = Quat::lookRotation(dir(), up); }

void Camera::fitToBox(const Box3& box) {
  Vec3 s = box.size();
  sceneScale = std::max({s.x, s.y, s.z}); if (!(sceneScale > 0)) sceneScale = 1;
  sceneCenter = box.empty() ? Vec3{} : box.center();
  orthoHalf = sceneScale * 0.75;
  reset();
}

// free views: the default perspective framing of the whole model (upright, no roll); fixed views re-centre
void Camera::reset() {
  lastWheelDist = 0;                                         // (the wheel's rate starts over)
  uvCx = uvCy = 0.5; uvPpu = 0;                              // (the UV Editor: the tile fitted)
  pivot = sceneCenter;
  if (!fixedView(view)) {
    QString v = view;
    up = {0, 1, 0}; pos = sceneCenter + Vec3(sceneScale * 0.9, sceneScale * 0.3, sceneScale * 2.3); lookAt(sceneCenter);
    view = "persp"; setView(v);
  } else setView(view);
}

void Camera::setView(const QString& mode) {
  lastWheelDist = 0;                                         // (the wheel's rate starts over)
  if (mode == "uv") { view = mode; return; }                 // (the UV Editor: the 3D camera kept for when it goes back)
  bool wasFree = !fixedView(view);
  if (mode == "persp" || mode == "ortho") {
    Vec3 d = dir();
    double dist = std::max((pivot - pos).dot(d), sceneScale * 0.05);
    double half = ortho() ? orthoHalf / zoom : dist * std::tan(fov / 2 * DEG);
    if (!wasFree && ortho()) pivot = sceneCenter;
    if (mode == "persp") pos = pivot + d * (-half / std::tan(fov / 2 * DEG));
    else { pos = pivot + d * (-sceneScale * 20); zoom = orthoHalf / std::max(half, 1e-6); }
  } else {
    Vec3 d = viewDirOf(mode); double dist = sceneScale * 20;
    pos = sceneCenter + d * dist; up = mode == "top" ? Vec3(0, 0, -1) : Vec3(0, 1, 0); lookAt(sceneCenter);
    zoom = 1; pivot = sceneCenter;
  }
  view = mode;
}

void Camera::orbit(double dx, double dy) {
  lastWheelDist = 0;                                         // (the wheel's rate starts over)
  if (view == "top") {                                       // top view: left / right turns the picture
    Quat q = Quat::fromAxisAngle({0, 1, 0}, -dx * 0.006);
    up = q.rotate(up); quat = (q * quat).normalized(); return;
  }
  if (fixedView(view)) return;
  if (trackball) {                                           // (trackball: round the view's own up and right — no levelling)
    Quat q = Quat::fromAxisAngle(upVec(), -dx * 0.006) * Quat::fromAxisAngle(right(), -dy * 0.006);
    pos = q.rotate(pos - pivot) + pivot; quat = (q * quat).normalized(); up = upVec(); return;
  }
  Quat qy = Quat::fromAxisAngle({0, 1, 0}, -dx * 0.006), qx = Quat::fromAxisAngle(right(), -dy * 0.006);
  Quat q = qy * qx;
  if ((q * quat).rotate({0, 1, 0}).y < 0.02) q = qy;
  pos = q.rotate(pos - pivot) + pivot;
  quat = (q * quat).normalized();
  level();                                                   // rounding errors never accumulate into a roll
}

double Camera::worldPerPixel() const {
  if (ortho()) return 2 * orthoHalf / zoom / std::max(1, height);
  double d = std::max((pivot - pos).dot(dir()), sceneScale * 0.05);
  return 2 * d * std::tan(fov / 2 * DEG) / std::max(1, height);
}

void Camera::pan(double dx, double dy) {
  lastWheelDist = 0;                                         // (the wheel's rate starts over)
  double k = worldPerPixel();
  pos += right() * (-dx * k) + upVec() * (dy * k);           // (the pivot stays, as in the HTML version)
}

void Camera::dolly(double amount) {
  lastWheelDist = 0;                                         // (the wheel's rate starts over)
  if (ortho()) { zoom = clamp(zoom * std::exp(amount * 0.01), 0.02, 500); return; }
  double d = std::max(pivot.distanceTo(pos), sceneScale * 0.05);
  pos += dir() * (amount * d * 0.006);
}

Vec3 Camera::rayDir(double x, double y) const {
  double aspect = double(width) / std::max(1, height);
  double nx = x / std::max(1, width) * 2 - 1, ny = 1 - y / std::max(1, height) * 2;
  if (ortho()) return dir();
  double t = std::tan(fov / 2 * DEG);
  return (dir() + right() * (nx * t * aspect) + upVec() * (ny * t)).normalized();
}

void Camera::wheel(double x, double y, double steps, std::optional<double> hitDist) {
  double s = clamp(steps, -3, 3);
  if (!ortho()) {
    // 15 % of the distance to what is under the cursor per notch; nothing there — the distance the last step went by
    // (no jump when the cursor leaves the model), else to the pivot
    double dist = hitDist ? *hitDist : lastWheelDist > 0 ? lastWheelDist : std::max(pivot.distanceTo(pos), sceneScale * 0.1);
    pos += rayDir(x, y) * (s * dist * 0.15);
    lastWheelDist = std::max(dist * (1 - s * 0.15), sceneScale * 1e-4);
  } else {
    // zoom about the cursor: the world point under it stays put
    double aspect = double(width) / std::max(1, height);
    double nx = x / std::max(1, width) * 2 - 1, ny = 1 - y / std::max(1, height) * 2;
    auto at = [&](double z) { double h = orthoHalf / z; return pos + right() * (nx * h * aspect) + upVec() * (ny * h); };
    Vec3 p0 = at(zoom);
    zoom = clamp(zoom * std::pow(1.15, s), 0.02, 500);
    pos += p0 - at(zoom);
  }
}

void Camera::frame(const Box3& box, bool small) {
  lastWheelDist = 0;                                         // (the wheel's rate starts over)
  if (box.empty()) return;
  Vec3 c = box.center();
  double r = std::max(box.size().length() * 0.6, sceneScale * (small ? 0.02 : 0.1));
  pivot = c;
  if (!ortho()) pos = c + dir() * (-r / std::tan(fov / 2 * DEG));
  else { double d = (pos - c).dot(dir()); pos = c + dir() * d; zoom = orthoHalf / r; }
}

}  // namespace hm
