#include "view/Picking.h"
#include "view/Camera.h"
#include <QVector4D>
#include <cmath>

namespace hm {

Projector::Projector(const Camera& cam, double logicalW, double logicalH) : vp(cam.projMatrix() * cam.viewMatrix()), w(logicalW), h(logicalH) {}

ScreenPt Projector::toScreen(const Vec3& p) const {
  QVector4D c = vp * QVector4D(float(p.x), float(p.y), float(p.z), 1.0f);
  if (c.w() <= 0) return {0, 0, 2};
  double x = c.x() / c.w(), y = c.y() / c.w(), z = c.z() / c.w();
  return {(x + 1) / 2 * w, (1 - y) / 2 * h, z};
}

Ray rayAt(const Camera& cam, double x, double y, double W, double H) {
  double nx = x / W * 2 - 1, ny = 1 - y / H * 2, aspect = W / H;
  if (!cam.ortho()) return {cam.pos, cam.rayDir(x / W * cam.width, y / H * cam.height)};
  double hh = cam.orthoHalf / cam.zoom;
  return {cam.pos + cam.right() * (nx * hh * aspect) + cam.upVec() * (ny * hh), cam.dir()};
}

std::optional<Vec3> planeHit(const Ray& r, const Vec3& point, const Vec3& normal) {
  double den = normal.dot(r.d);
  if (std::abs(den) < 1e-12) return std::nullopt;
  double t = (point - r.o).dot(normal) / den;
  if (t < 0) return std::nullopt;
  return r.o + r.d * t;
}

namespace {
bool rayBox(const Ray& r, const Box3& b) {
  double t0 = 0, t1 = 1e300;
  for (int k = 0; k < 3; k++) {
    double o = r.o[k], d = r.d[k], lo = b.min[k], hi = b.max[k];
    if (std::abs(d) < 1e-15) { if (o < lo || o > hi) return false; continue; }
    double a = (lo - o) / d, c = (hi - o) / d; if (a > c) std::swap(a, c);
    t0 = std::max(t0, a); t1 = std::min(t1, c); if (t0 > t1) return false;
  }
  return true;
}
}  // namespace

// Möller–Trumbore, both sides (DoubleSide), the nearest hit of all
std::optional<RayHit> raycast(const Ray& r, const std::vector<PickMesh>& meshes) {
  std::optional<RayHit> best;
  for (const PickMesh& m : meshes) {
    const auto& T = m.trisRef ? *m.trisRef : m.tris;
    if (!m.pos || T.empty() || !rayBox(r, m.box)) continue;
    const auto& P = *m.pos;
    for (size_t i = 0; i + 2 < T.size(); i += 3) {
      const Vec3 &a = P[T[i]], &b = P[T[i + 1]], &c = P[T[i + 2]];
      Vec3 e1 = b - a, e2 = c - a, pv = r.d.cross(e2); double det = e1.dot(pv);
      if (std::abs(det) < 1e-18) continue;
      double inv = 1 / det; Vec3 tv = r.o - a; double u = tv.dot(pv) * inv; if (u < 0 || u > 1) continue;
      Vec3 qv = tv.cross(e1); double v = r.d.dot(qv) * inv; if (v < 0 || u + v > 1) continue;
      double t = e2.dot(qv) * inv; if (t <= 0) continue;
      if (!best || t < best->t) best = RayHit{m.itemId, t, r.o + r.d * t, e1.cross(e2).normalized(), int(i / 3)};
    }
  }
  return best;
}

std::optional<CurveHit> nearestOnScreen(const Projector& P, const std::vector<Vec3>& samples, double x, double y) {
  std::optional<CurveHit> best;
  std::vector<ScreenPt> sp; sp.reserve(samples.size());
  for (auto& p : samples) sp.push_back(P.toScreen(p));
  for (size_t i = 0; i + 1 < sp.size(); i++) {
    const ScreenPt &a = sp[i], &b = sp[i + 1]; if (a.z > 1 || b.z > 1) continue;
    double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
    double t = l2 > 0 ? clamp(((x - a.x) * vx + (y - a.y) * vy) / l2, 0, 1) : 0;
    double d = std::hypot(a.x + vx * t - x, a.y + vy * t - y);
    if (!best || d < best->d) best = CurveHit{d, int(i), t};
  }
  return best;
}

bool insidePolygon(const std::vector<std::pair<double, double>>& P, double x, double y) {
  bool c = false;
  for (size_t i = 0, j = P.size() - 1; i < P.size(); j = i++) {
    auto [xi, yi] = P[i]; auto [xj, yj] = P[j];
    if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) c = !c;
  }
  return c;
}

}  // namespace hm
