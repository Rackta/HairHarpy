// Small double-precision vector math for the scene (the GPU gets floats). Mirrors what the HTML version used from
// THREE.Vector3 / Quaternion, so ported formulas read the same.
#pragma once
#include <algorithm>
#include <cmath>

namespace hm {

inline double clamp(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

struct Vec3 {
  double x = 0, y = 0, z = 0;
  Vec3() = default;
  constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(double k) const { return {x * k, y * k, z * k}; }
  // (as THREE.Vector3.divideScalar: times the reciprocal — the same bits as the HTML version; where the shape of a lock
  // is fitted to nearly coincident points, the last bit decides the oval's axis)
  Vec3 operator/(double k) const { double r = 1 / k; return {x * r, y * r, z * r}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(double k) { x *= k; y *= k; z *= k; return *this; }
  bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
  double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
  double lengthSq() const { return dot(*this); }
  double length() const { return std::sqrt(lengthSq()); }
  double distanceTo(const Vec3& o) const { return (*this - o).length(); }
  Vec3 normalized() const { double l = length(); return *this / (l ? l : 1); }   // (THREE: divideScalar(length() || 1))
  Vec3 lerp(const Vec3& o, double t) const { return *this + (o - *this) * t; }
  double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
inline Vec3 operator*(double k, const Vec3& v) { return v * k; }

struct Quat {
  double x = 0, y = 0, z = 0, w = 1;
  static Quat fromAxisAngle(const Vec3& axis, double a) {
    Vec3 n = axis.normalized(); double s = std::sin(a / 2);
    return {n.x * s, n.y * s, n.z * s, std::cos(a / 2)};
  }
  Quat operator*(const Quat& b) const {   // this * b (b applied first)
    return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
  }
  Vec3 rotate(const Vec3& v) const {
    Vec3 q{x, y, z}; Vec3 t = q.cross(v) * 2.0;
    return v + t * w + q.cross(t);
  }
  Quat normalized() const { double l = std::sqrt(x * x + y * y + z * z + w * w); return l > 0 ? Quat{x / l, y / l, z / l, w / l} : Quat{}; }
  Quat conj() const { return {-x, -y, -z, w}; }   // (the inverse of a unit quaternion)
  // rotation whose -Z looks along `dir` with +Y as close to `up` as possible (THREE's lookAt for cameras)
  static Quat lookRotation(const Vec3& dir, const Vec3& up);
};

inline Quat Quat::lookRotation(const Vec3& dir, const Vec3& up) {
  Vec3 zAxis = (-dir).normalized();
  Vec3 xAxis = up.cross(zAxis);
  if (xAxis.lengthSq() < 1e-20) xAxis = Vec3(std::abs(zAxis.z) > 0.9 ? 1 : 0, 0, std::abs(zAxis.z) > 0.9 ? 0 : 1).cross(zAxis);
  xAxis = xAxis.normalized();
  Vec3 yAxis = zAxis.cross(xAxis);
  double m00 = xAxis.x, m01 = yAxis.x, m02 = zAxis.x, m10 = xAxis.y, m11 = yAxis.y, m12 = zAxis.y, m20 = xAxis.z,
         m21 = yAxis.z, m22 = zAxis.z, tr = m00 + m11 + m22;
  Quat q;
  if (tr > 0) { double s = 0.5 / std::sqrt(tr + 1.0); q = {(m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25 / s}; }
  else if (m00 > m11 && m00 > m22) { double s = 2.0 * std::sqrt(1.0 + m00 - m11 - m22); q = {0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s}; }
  else if (m11 > m22) { double s = 2.0 * std::sqrt(1.0 + m11 - m00 - m22); q = {(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s}; }
  else { double s = 2.0 * std::sqrt(1.0 + m22 - m00 - m11); q = {(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s}; }
  return q.normalized();
}

// an affine map p → L·p + t (the gizmo's change of a drag)
struct Affine {
  double L[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  Vec3 t;
  Vec3 lin(const Vec3& v) const { return {L[0][0] * v.x + L[0][1] * v.y + L[0][2] * v.z, L[1][0] * v.x + L[1][1] * v.y + L[1][2] * v.z, L[2][0] * v.x + L[2][1] * v.y + L[2][2] * v.z}; }
  Vec3 apply(const Vec3& p) const { return lin(p) + t; }
  double det() const { return L[0][0] * (L[1][1] * L[2][2] - L[1][2] * L[2][1]) - L[0][1] * (L[1][0] * L[2][2] - L[1][2] * L[2][0]) + L[0][2] * (L[1][0] * L[2][1] - L[1][1] * L[2][0]); }
  static Affine mul(const Affine& a, const Affine& b) {      // a ∘ b
    Affine r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { r.L[i][j] = 0; for (int k = 0; k < 3; k++) r.L[i][j] += a.L[i][k] * b.L[k][j]; }
    r.t = a.lin(b.t) + a.t; return r;
  }
  Vec3 normal(const Vec3& n) const {                         // a normal carried along: the inverse transpose, unit length
    if (n.lengthSq() == 0) return n; Affine i = inverse();
    return Vec3(i.L[0][0] * n.x + i.L[1][0] * n.y + i.L[2][0] * n.z, i.L[0][1] * n.x + i.L[1][1] * n.y + i.L[2][1] * n.z, i.L[0][2] * n.x + i.L[1][2] * n.y + i.L[2][2] * n.z).normalized();
  }
  Affine inverse() const {
    Affine r; double d = det(); if (std::abs(d) < 1e-300) return r;
    r.L[0][0] = (L[1][1] * L[2][2] - L[1][2] * L[2][1]) / d; r.L[0][1] = (L[0][2] * L[2][1] - L[0][1] * L[2][2]) / d; r.L[0][2] = (L[0][1] * L[1][2] - L[0][2] * L[1][1]) / d;
    r.L[1][0] = (L[1][2] * L[2][0] - L[1][0] * L[2][2]) / d; r.L[1][1] = (L[0][0] * L[2][2] - L[0][2] * L[2][0]) / d; r.L[1][2] = (L[0][2] * L[1][0] - L[0][0] * L[1][2]) / d;
    r.L[2][0] = (L[1][0] * L[2][1] - L[1][1] * L[2][0]) / d; r.L[2][1] = (L[0][1] * L[2][0] - L[0][0] * L[2][1]) / d; r.L[2][2] = (L[0][0] * L[1][1] - L[0][1] * L[1][0]) / d;
    r.t = -r.lin(t); return r;
  }
  static Affine fromQuat(const Quat& q) {
    Affine r; Vec3 cx = q.rotate({1, 0, 0}), cy = q.rotate({0, 1, 0}), cz = q.rotate({0, 0, 1});
    for (int i = 0; i < 3; i++) { r.L[i][0] = cx[i]; r.L[i][1] = cy[i]; r.L[i][2] = cz[i]; }
    return r;
  }
};

struct Box3 {
  Vec3 min{1e300, 1e300, 1e300}, max{-1e300, -1e300, -1e300};
  bool empty() const { return max.x < min.x || max.y < min.y || max.z < min.z; }
  void expand(const Vec3& p) {
    min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
    max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
  }
  void unite(const Box3& b) { if (!b.empty()) { expand(b.min); expand(b.max); } }
  Vec3 center() const { return (min + max) * 0.5; }
  Vec3 size() const { return empty() ? Vec3{} : max - min; }
};

}  // namespace hm
