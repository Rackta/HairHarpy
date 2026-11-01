#include "view/Gizmo.h"
#include <cmath>

namespace hm {

namespace {
const double PI = 3.14159265358979323846;
const QColor AXC[3] = {QColor(0xff, 0x3b, 0x3b), QColor(0x4c, 0xd9, 0x4c), QColor(0x4c, 0x7d, 0xff)};
const QColor HOT(0xff, 0xd2, 0x3f), GREY(0xc8, 0xc8, 0xc8);
const char* AXN[3] = {"X", "Y", "Z"};
double segDist(double px, double py, double ax, double ay, double bx, double by) {
  double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy, t = l2 > 0 ? clamp(((px - ax) * vx + (py - ay) * vy) / l2, 0, 1) : 0;
  return std::hypot(ax + vx * t - px, ay + vy * t - py);
}
}  // namespace

Vec3 Gizmo::axisVec(int k) const { return quat.rotate({k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0}); }

// the handles' length: ~90 px on the screen wherever the gizmo is
double Gizmo::unit(const Camera& cam, double W, double H) const {
  Projector P(cam, W, H); ScreenPt a = P.toScreen(pivot), b = P.toScreen(pivot + cam.right() * (cam.sceneScale * 0.01));
  double px = std::hypot(b.x - a.x, b.y - a.y);
  return px > 1e-9 ? 90.0 * cam.sceneScale * 0.01 / px : cam.sceneScale * 0.1;
}

std::vector<Gizmo::Line> Gizmo::lines(const Camera& cam, double W, double H) const {
  std::vector<Line> out; if (!visible()) return out;
  double u = unit(cam, W, H), px = u / 90; Vec3 p = pivot, cd = cam.dir(), cr = cam.right(), cu = cam.upVec();
  auto col = [&](const QString& ax, const QColor& c) { return (active.valid() ? active.axis == ax : hover.axis == ax) ? HOT : c; };
  auto screenSquare = [&](const Vec3& c, double r, const QColor& k, float w) {
    Vec3 A = c + (cr + cu) * (r * px), B = c + (cr - cu) * (r * px), C = c - (cr + cu) * (r * px), D = c + (cu - cr) * (r * px);
    out.push_back({A, B, k, w}); out.push_back({B, C, k, w}); out.push_back({C, D, k, w}); out.push_back({D, A, k, w});
  };
  if (mode == Mode::Move) {
    for (int k = 0; k < 3; k++) {
      if (lock[k]) continue; Vec3 a = axisVec(k), tip = p + a * u; QColor c = col(AXN[k], AXC[k]);
      out.push_back({p + a * (12 * px), tip, c, 3});
      Vec3 side = a.cross(cd); if (side.lengthSq() < 1e-12) side = cr; side = side.normalized() * (0.07 * u);
      Vec3 back = p + a * (0.8 * u);
      out.push_back({tip, back + side, c, 3}); out.push_back({tip, back - side, c, 3}); out.push_back({back + side, back - side, c, 3});
    }
    for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) {
      if (lock[i] || lock[j]) continue; int k = 3 - i - j; QString ax = QString(AXN[i]) + AXN[j];
      Vec3 a = axisVec(i), b = axisVec(j); QColor c = col(ax, AXC[k]);
      Vec3 q0 = p + (a * 0.22 + b * 0.22) * u, q1 = p + (a * 0.42 + b * 0.22) * u, q2 = p + (a * 0.42 + b * 0.42) * u, q3 = p + (a * 0.22 + b * 0.42) * u;
      out.push_back({q0, q1, c, 2}); out.push_back({q1, q2, c, 2}); out.push_back({q2, q3, c, 2}); out.push_back({q3, q0, c, 2});
    }
    screenSquare(p, 6, col("XYZ", GREY), 2);
  } else if (mode == Mode::Rotate) {
    for (int k = 0; k < 3; k++) {
      if (lock[k]) continue; Vec3 a = axisVec(k), e1 = axisVec((k + 1) % 3), e2 = axisVec((k + 2) % 3); QColor c = col(AXN[k], AXC[k]);
      const int N = 72;
      for (int i = 0; i < N; i++) {
        double t0 = 2 * PI * i / N, t1 = 2 * PI * (i + 1) / N;
        Vec3 A = p + (e1 * std::cos(t0) + e2 * std::sin(t0)) * u, B = p + (e1 * std::cos(t1) + e2 * std::sin(t1)) * u;
        bool back = ((A + B) * 0.5 - p).dot(cd) > 0.05 * u;          // (the far half dimmed)
        QColor cc = c; if (back) cc.setAlphaF(0.35);
        out.push_back({A, B, cc, 3}); (void)a;
      }
    }
    QColor c = col("E", GREY); const int N = 72;
    for (int i = 0; i < N; i++) { double t0 = 2 * PI * i / N, t1 = 2 * PI * (i + 1) / N;
      out.push_back({p + (cr * std::cos(t0) + cu * std::sin(t0)) * (1.15 * u), p + (cr * std::cos(t1) + cu * std::sin(t1)) * (1.15 * u), c, 2}); }
  } else if (mode == Mode::Scale) {
    for (int k = 0; k < 3; k++) {
      if (lock[k]) continue; Vec3 a = axisVec(k), tip = p + a * (0.9 * u); QColor c = col(AXN[k], AXC[k]);
      out.push_back({p + a * (16 * px), tip, c, 3}); screenSquare(tip, 5, c, 3);
    }
    screenSquare(p, 8, col("XYZ", GREY), 2);
  }
  return out;
}

Gizmo::Handle Gizmo::pick(const Camera& cam, double W, double H, double mx, double my) const {
  if (!visible()) return {};
  Projector P(cam, W, H); double u = unit(cam, W, H); ScreenPt c = P.toScreen(pivot);
  if (c.z >= 1) return {};
  int freeN = 0, freeK = -1; for (int k = 0; k < 3; k++) if (!lock[k]) { freeN++; freeK = k; }
  auto facing = [&](int k) { return std::abs(axisVec(k).dot(cam.dir())) > 0.94; };
  if (mode == Mode::Move) {
    if (std::hypot(mx - c.x, my - c.y) < 10) {
      if (freeN == 1 && facing(freeK)) return {QString("D") + AXN[freeK]};   // (the only axis faces the camera: up / down along it)
      return freeN == 3 ? Handle{"XYZ"} : freeN == 1 ? Handle{AXN[freeK]} : Handle{"XYZ"};
    }
    double bd = 8; QString best;
    for (int k = 0; k < 3; k++) {
      if (lock[k]) continue; ScreenPt t = P.toScreen(pivot + axisVec(k) * u);
      double d = segDist(mx, my, c.x + (t.x - c.x) * 0.1, c.y + (t.y - c.y) * 0.1, t.x, t.y); if (d < bd) { bd = d; best = AXN[k]; }
    }
    if (!best.isEmpty()) return {best};
    for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) {
      if (lock[i] || lock[j]) continue; Vec3 a = axisVec(i), b = axisVec(j);
      std::vector<std::pair<double, double>> q;
      for (auto [s, t] : {std::pair{0.22, 0.22}, {0.42, 0.22}, {0.42, 0.42}, {0.22, 0.42}}) { ScreenPt v = P.toScreen(pivot + (a * s + b * t) * u); q.push_back({v.x, v.y}); }
      if (insidePolygon(q, mx, my)) return {QString(AXN[i]) + AXN[j]};
    }
    return {};
  }
  if (mode == Mode::Rotate) {
    double bd = 7; QString best; const int N = 72;
    for (int k = 0; k < 3; k++) {
      if (lock[k]) continue; Vec3 e1 = axisVec((k + 1) % 3), e2 = axisVec((k + 2) % 3);
      ScreenPt prev = P.toScreen(pivot + e1 * u);
      for (int i = 1; i <= N; i++) { double t = 2 * PI * i / N; ScreenPt s = P.toScreen(pivot + (e1 * std::cos(t) + e2 * std::sin(t)) * u);
        double d = segDist(mx, my, prev.x, prev.y, s.x, s.y); if (d < bd) { bd = d; best = AXN[k]; } prev = s; }
    }
    if (!best.isEmpty()) return {best};
    ScreenPt r = P.toScreen(pivot + cam.right() * (1.15 * u)); double R = std::hypot(r.x - c.x, r.y - c.y);
    if (std::abs(std::hypot(mx - c.x, my - c.y) - R) < 7 && freeN == 3) return {"E"};
    return {};
  }
  if (mode == Mode::Scale) {
    if (std::hypot(mx - c.x, my - c.y) < 16) return {"XYZ"};
    double bd = 9; QString best;
    for (int k = 0; k < 3; k++) {
      if (lock[k]) continue; ScreenPt t = P.toScreen(pivot + axisVec(k) * (0.9 * u));
      double d = segDist(mx, my, c.x + (t.x - c.x) * 0.15, c.y + (t.y - c.y) * 0.15, t.x, t.y); if (d < bd) { bd = d; best = AXN[k]; }
    }
    return best.isEmpty() ? Handle{} : Handle{best};
  }
  return {};
}

void Gizmo::begin(const Handle& h, const Camera& cam, double W, double H, double mx, double my) {
  active = h; p0_ = pivot; q0_ = quat; rot_ = Quat{}; mx0_ = mx; my0_ = my; axisK_ = -1;
  Ray r = rayAt(cam, mx, my, W, H);
  QString ax = h.axis;
  if (mode == Mode::Move) {
    if (ax.startsWith('D')) { axisK_ = QString("XYZ").indexOf(ax[1]); return; }
    if (ax.size() == 1) {
      axisK_ = QString("XYZ").indexOf(ax[0]); Vec3 a = axisVec(axisK_);
      // the axis' parameter nearest the pointer's ray
      Vec3 w0 = p0_ - r.o; double b = a.dot(r.d), d = a.dot(w0), e = r.d.dot(w0), den = 1 - b * b;
      t0_ = std::abs(den) > 1e-9 ? (b * e - d) / den : 0;
    } else if (ax.size() == 2) { int k = 3 - QString("XYZ").indexOf(ax[0]) - QString("XYZ").indexOf(ax[1]); hit0_ = planeHit(r, p0_, axisVec(k)).value_or(p0_); }
    else hit0_ = planeHit(r, p0_, cam.dir()).value_or(p0_);
  } else if (mode == Mode::Rotate) {
    rotAxis_ = ax == "E" ? -cam.dir() : axisVec(QString("XYZ").indexOf(ax[0]));
    Vec3 hp = planeHit(r, p0_, rotAxis_).value_or(p0_ + cam.right());
    u0_ = (hp - p0_).normalized();
  }
}

Affine Gizmo::drag(const Camera& cam, double W, double H, double mx, double my, bool shift, bool ctrl) {
  Affine M; Ray r = rayAt(cam, mx, my, W, H); QString ax = active.axis;
  auto lockRemove = [&](Vec3 d) {                         // (the locked axes' motion removed, in the gizmo's frame)
    Vec3 l = q0_.conj().rotate(d); if (lock[0]) l.x = 0; if (lock[1]) l.y = 0; if (lock[2]) l.z = 0; return q0_.rotate(l);
  };
  if (mode == Mode::Move) {
    Vec3 d;
    if (ax.startsWith('D')) {
      Vec3 a = axisVec(axisK_); if (a.dot(cam.dir()) < 0) a = -a;   // (up = away from the camera)
      Projector P(cam, W, H); ScreenPt s0 = P.toScreen(p0_), s1 = P.toScreen(p0_ + cam.upVec() * (cam.sceneScale * 0.01));
      double upp = cam.sceneScale * 0.01 / std::max(1e-9, std::hypot(s1.x - s0.x, s1.y - s0.y));
      d = a * ((my0_ - my) * upp);
    } else if (ax.size() == 1) {
      Vec3 a = axisVec(axisK_), w0 = p0_ - r.o; double b = a.dot(r.d), dd = a.dot(w0), e = r.d.dot(w0), den = 1 - b * b;
      double t = std::abs(den) > 1e-9 ? (b * e - dd) / den : t0_;
      d = a * (t - t0_);
    } else if (ax.size() == 2) {
      int k = 3 - QString("XYZ").indexOf(ax[0]) - QString("XYZ").indexOf(ax[1]);
      if (auto h = planeHit(r, p0_, axisVec(k))) d = *h - hit0_;
    } else if (auto h = planeHit(r, p0_, cam.dir())) d = *h - hit0_;
    d = lockRemove(d);
    if (shift && moveStep > 0) { Vec3 l = q0_.conj().rotate(d); l.x = std::round(l.x / moveStep) * moveStep; l.y = std::round(l.y / moveStep) * moveStep; l.z = std::round(l.z / moveStep) * moveStep; d = q0_.rotate(l); }   // (Shift — steps)
    M.t = d; pivot = p0_ + d;
    return M;
  }
  if (mode == Mode::Rotate) {
    Vec3 hp = planeHit(r, p0_, rotAxis_).value_or(p0_ + u0_); Vec3 u1 = (hp - p0_).normalized();
    double ang = std::atan2(rotAxis_.dot(u0_.cross(u1)), u0_.dot(u1));
    if (shift) ang = std::round(ang / (PI / 4)) * (PI / 4);          // (Shift — 45° steps)
    rot_ = Quat::fromAxisAngle(rotAxis_, ang);
    M = Affine::fromQuat(rot_); M.t = p0_ - M.lin(p0_);
    quat = (rot_ * q0_).normalized();
    return M;
  }
  // Scale: from the pointer on the screen (the centre cube: right / up — bigger, ×2 per ~170 px; an axis: its tip follows
  // the pointer, the lever at least 60 px)
  double s[3] = {1, 1, 1}, dx = mx - mx0_, dy = my - my0_;
  if (ax == "XYZ") { double k = std::exp((dx - dy) / 250); s[0] = s[1] = s[2] = k; }
  else {
    Projector P(cam, W, H); ScreenPt c = P.toScreen(p0_);
    for (int k = 0; k < 3; k++) {
      if (!ax.contains(AXN[k])) continue;
      ScreenPt tip = P.toScreen(p0_ + axisVec(k) * (cam.sceneScale * 0.1));
      double sx = tip.x - c.x, sy = tip.y - c.y, l = std::hypot(sx, sy); if (l < 1e-3) continue;
      sx /= l; sy /= l;
      double d0 = (mx0_ - c.x) * sx + (my0_ - c.y) * sy, lever = (d0 < 0 ? -1 : 1) * std::max(std::abs(d0), 60.0);
      s[k] = std::max(0.01, 1 + (dx * sx + dy * sy) / lever);
    }
  }
  for (int k = 0; k < 3; k++) if (lock[k]) s[k] = 1;
  if (ctrl) { int m = 0; for (int k = 1; k < 3; k++) if (std::abs(s[k] - 1) > std::abs(s[m] - 1)) m = k; s[0] = s[1] = s[2] = s[m]; }   // (Ctrl — uniform)
  if (shift) for (double& v : s) v = std::round(v * 10) / 10;                                                                           // (Shift — 0.1 steps)
  for (double& v : s) if (std::abs(v) < 1e-6) v = 1e-6; for (int k = 0; k < 3; k++) scaleNow[k] = s[k];
  Affine R0 = Affine::fromQuat(q0_), S; for (int k = 0; k < 3; k++) S.L[k][k] = s[k];
  M = Affine::mul(Affine::mul(R0, S), R0.inverse()); M.t = p0_ - M.lin(p0_);
  return M;
}

}  // namespace hm
