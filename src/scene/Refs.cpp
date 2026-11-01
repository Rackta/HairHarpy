#include "scene/Refs.h"
#include "scene/Scene.h"
#include "view/Camera.h"
#include <QObject>
#include <algorithm>
#include <cmath>

namespace hm {

M4 m4Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
M4 m4Compose(const Vec3& t, const Quat& q, const Vec3& s) {
  Vec3 cx = q.rotate({1, 0, 0}) * s.x, cy = q.rotate({0, 1, 0}) * s.y, cz = q.rotate({0, 0, 1}) * s.z;
  return {cx.x, cx.y, cx.z, 0, cy.x, cy.y, cy.z, 0, cz.x, cz.y, cz.z, 0, t.x, t.y, t.z, 1};
}
static Quat quatFromRot(double m00, double m01, double m02, double m10, double m11, double m12, double m20, double m21, double m22) {
  double tr = m00 + m11 + m22; Quat q;
  if (tr > 0) { double s = 0.5 / std::sqrt(tr + 1.0); q = {(m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25 / s}; }
  else if (m00 > m11 && m00 > m22) { double s = 2.0 * std::sqrt(1.0 + m00 - m11 - m22); q = {0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s}; }
  else if (m11 > m22) { double s = 2.0 * std::sqrt(1.0 + m11 - m00 - m22); q = {(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s}; }
  else { double s = 2.0 * std::sqrt(1.0 + m22 - m00 - m11); q = {(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s}; }
  return q.normalized();
}
// (three.js's Matrix4.decompose: a negative determinant flips the X scale)
void m4Decompose(const M4& m, Vec3& t, Quat& q, Vec3& s) {
  double sx = Vec3(m[0], m[1], m[2]).length(), sy = Vec3(m[4], m[5], m[6]).length(), sz = Vec3(m[8], m[9], m[10]).length();
  double det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) + m[8] * (m[1] * m[6] - m[2] * m[5]);
  if (det < 0) sx = -sx;
  t = {m[12], m[13], m[14]};
  double ix = sx ? 1 / sx : 0, iy = sy ? 1 / sy : 0, iz = sz ? 1 / sz : 0;
  q = quatFromRot(m[0] * ix, m[4] * iy, m[8] * iz, m[1] * ix, m[5] * iy, m[9] * iz, m[2] * ix, m[6] * iy, m[10] * iz);
  s = {sx, sy, sz};
}
M4 m4Mul(const M4& a, const M4& b) {
  M4 r{}; for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { double v = 0; for (int k = 0; k < 4; k++) v += a[size_t(k * 4 + rr)] * b[size_t(c * 4 + k)]; r[size_t(c * 4 + rr)] = v; }
  return r;
}
Affine m4ToAffine(const M4& m) { Affine a; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) a.L[i][j] = m[size_t(j * 4 + i)]; a.t = {m[12], m[13], m[14]}; return a; }
M4 m4FromAffine(const Affine& a) { M4 m = m4Identity(); for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[size_t(j * 4 + i)] = a.L[i][j]; m[12] = a.t.x; m[13] = a.t.y; m[14] = a.t.z; return m; }
M4 m4Inverse(const M4& m) { return m4FromAffine(m4ToAffine(m).inverse()); }
Vec3 m4Apply(const M4& m, const Vec3& p) { return m4ToAffine(m).apply(p); }

namespace refs {

const char* VIEWS[6] = {"front", "back", "left", "right", "top", "ortho"};
QString viewLabel(const QString& v) {
  return v == "front" ? QObject::tr("Front") : v == "back" ? QObject::tr("Back") : v == "left" ? QObject::tr("Left") : v == "right" ? QObject::tr("Right") : v == "top" ? QObject::tr("Top") : QObject::tr("Orthographic");
}
QString opposite(const QString& v) { return v == "front" ? "back" : v == "back" ? "front" : v == "left" ? "right" : v == "right" ? "left" : QString(); }

static M4 camWorld(const Camera& cam) { return m4Compose(cam.pos, cam.quat, {1, 1, 1}); }
static Vec3 vec(const QJsonValue& v) { QJsonArray a = v.toArray(); return a.size() >= 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3{}; }

M4 frame(const RefPlane& rp, const Camera& cam) {
  if (rp.lock.isObject()) return m4Compose(vec(rp.lock.toObject().value("anchor")), cam.quat, {1, 1, 1});
  return camWorld(cam);
}
void apply(RefPlane& rp, const Camera& cam) { if (!rp.follows()) return; m4Decompose(m4Mul(frame(rp, cam), rp.loc), rp.pos, rp.quat, rp.scl); }
void capture(RefPlane& rp, const Camera& cam) { if (!rp.follows()) return; rp.loc = m4Mul(m4Inverse(frame(rp, cam)), rp.matrix()); rp.hasLoc = true; }

void setLock(RefPlane& rp, const Camera& cam, bool on) {
  M4 before = frame(rp, cam);
  if (on) {
    QJsonObject v{{"pos", QJsonArray{cam.pos.x, cam.pos.y, cam.pos.z}}, {"quat", QJsonArray{cam.quat.x, cam.quat.y, cam.quat.z, cam.quat.w}}, {"zoom", cam.zoom}, {"pivot", QJsonArray{cam.pivot.x, cam.pivot.y, cam.pivot.z}}};
    rp.lock = QJsonObject{{"anchor", QJsonArray{cam.pivot.x, cam.pivot.y, cam.pivot.z}}, {"view", v}};
  } else rp.lock = QJsonValue(QJsonValue::Null);
  rp.loc = m4Mul(m4Mul(m4Inverse(frame(rp, cam)), before), rp.loc);   // (the same world place in the new frame: no jump)
  apply(rp, cam);
}

Box3 modelBox(const Scene& sc, const Camera& cam) {
  Box3 b; for (auto& [id, it] : sc.items) if (it.headTag && it.poly && sc.isVisible(it)) b.unite(it.poly->bounds());
  if (!b.empty()) return b;
  double h = cam.sceneScale / 2; b.expand(cam.sceneCenter - Vec3(h, h, h)); b.expand(cam.sceneCenter + Vec3(h, h, h)); return b;
}

void place(RefPlane& rp, const Scene& sc, const Camera& cam) {
  Box3 box = modelBox(sc, cam); Vec3 c = box.center(), sz = box.size(); double m = std::max({sz.x, sz.y, sz.z}) * 0.06; const QString& v = rp.view;
  if (rp.follows()) {                                         // facing the camera, behind the model, centred on it
    Vec3 q = m4Apply(m4Inverse(camWorld(cam)), c); double h = (sz.y > 0 ? sz.y : cam.sceneScale) * 1.15;
    rp.loc = m4Compose({q.x, q.y, q.z - sz.length() / 2 - m}, Quat{}, {h * rp.aspect, h, 1}); rp.hasLoc = true;
    if (rp.lock.isObject()) rp.loc = m4Mul(m4Mul(m4Inverse(frame(rp, cam)), camWorld(cam)), rp.loc);   // (into the locked frame)
    apply(rp, cam); return;
  }
  // (Euler XYZ of the view: front 0 · back 180° round Y · right +90° · left −90° · top −90° round X)
  rp.quat = v == "back" ? Quat::fromAxisAngle({0, 1, 0}, M_PI) : v == "right" ? Quat::fromAxisAngle({0, 1, 0}, M_PI / 2) : v == "left" ? Quat::fromAxisAngle({0, 1, 0}, -M_PI / 2)
          : v == "top" ? Quat::fromAxisAngle({1, 0, 0}, -M_PI / 2) : Quat{};
  rp.pos = c;
  if (v == "front") rp.pos.z = box.min.z - m; else if (v == "back") rp.pos.z = box.max.z + m;
  else if (v == "right") rp.pos.x = box.min.x - m; else if (v == "left") rp.pos.x = box.max.x + m;
  else rp.pos.y = box.min.y - m;
  double h = ((v == "top" ? sz.z : sz.y) > 0 ? (v == "top" ? sz.z : sz.y) : cam.sceneScale) * 1.15;
  rp.scl = {h * rp.aspect, h, 1};
}

Vec3 pivotWorld(const RefPlane& rp) { return m4Apply(rp.matrix(), {rp.piv[0], rp.piv[1], 0}); }
bool shownIn(const RefPlane& rp, const Camera& cam) {
  if (!rp.visible) return false;
  if (!rp.onlyView || cam.view == rp.view) return true;
  if (cam.view != "ortho" || rp.view == "ortho") return false;
  const Vec3 to = rp.view == "front" ? Vec3(0, 0, -1) : rp.view == "back" ? Vec3(0, 0, 1) : rp.view == "right" ? Vec3(-1, 0, 0) : rp.view == "left" ? Vec3(1, 0, 0) : Vec3(0, -1, 0);   // (where that view looks)
  return cam.dir().dot(to) > 0.990;                          // (≈ 8°)
}

void centre(RefPlane& rp, const Scene& sc, const Camera& cam) {
  Vec3 c = modelBox(sc, cam).center(), n = rp.quat.rotate({0, 0, 1}), P = pivotWorld(rp);
  Vec3 target = c + n * (P - c).dot(n);
  rp.pos += target - P;
  capture(rp, cam);
}

static M4 defFrame(const QString& view, bool onlyView, const Scene& sc, const Camera& cam) {
  RefPlane d; d.view = view; d.onlyView = onlyView; d.lock = QJsonValue(QJsonValue::Null);
  place(d, sc, cam); d.scl = {1, 1, 1}; return d.matrix();
}
RefPlane copyTo(const RefPlane& src0, const QString& view, const Scene& sc, const Camera& cam, int newId, const QString& name) {
  RefPlane src = src0; if (src.follows()) apply(src, cam);
  M4 rel = m4Mul(m4Inverse(defFrame(src.view, src.onlyView, sc, cam)), src.matrix());
  if (opposite(src.view) == view) rel = m4Mul(m4Compose({}, Quat{}, {-1, 1, 1}), rel);
  RefPlane t; t.id = newId; t.view = view; t.name = name; t.opacity = src.opacity; t.over = src.over; t.piv[0] = src.piv[0]; t.piv[1] = src.piv[1];
  t.img = src.img; t.aspect = src.aspect; t.lock = QJsonValue(QJsonValue::Null);
  m4Decompose(m4Mul(defFrame(view, t.onlyView, sc, cam), rel), t.pos, t.quat, t.scl);
  capture(t, cam);
  return t;
}

RefPlane fromJson(const QJsonObject& o, int id) {
  RefPlane rp; rp.id = id; rp.view = o.value("view").toString("front");
  rp.name = o.value("name").toString(); rp.img = o.value("img").toString();
  rp.aspect = o.value("aspect").toDouble(1); if (!(rp.aspect > 0)) rp.aspect = 1;
  rp.opacity = o.value("opacity").toDouble(0.6); rp.visible = o.value("visible").toBool(true);
  rp.onlyView = o.value("ov2").toBool() ? o.value("onlyView").toBool() : true;   // (older saves: "only in its view" was off by default — now on)
  rp.over = o.value("over").toBool();
  QJsonArray pv = o.value("piv").toArray(); if (pv.size() == 2) { rp.piv[0] = pv[0].toDouble(); rp.piv[1] = pv[1].toDouble(); }
  rp.pos = vec(o.value("pos")); QJsonArray q = o.value("quat").toArray(); if (q.size() == 4) rp.quat = Quat{q[0].toDouble(), q[1].toDouble(), q[2].toDouble(), q[3].toDouble()}.normalized();
  QJsonArray s = o.value("scl").toArray(); if (s.size() == 3) rp.scl = {s[0].toDouble(), s[1].toDouble(), s[2].toDouble()};
  QJsonArray l = o.value("loc").toArray(); if (l.size() == 16) { for (int i = 0; i < 16; i++) rp.loc[size_t(i)] = l[i].toDouble(); rp.hasLoc = true; }
  rp.lock = o.value("lock").isObject() ? o.value("lock") : QJsonValue(QJsonValue::Null);
  return rp;
}
QJsonObject toJson(const RefPlane& rp) {
  QJsonObject o{{"view", rp.view}, {"name", rp.name}, {"img", rp.img.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(rp.img)}, {"aspect", rp.aspect}, {"opacity", rp.opacity},
                {"visible", rp.visible}, {"onlyView", rp.onlyView}, {"ov2", true}, {"over", rp.over}, {"piv", QJsonArray{rp.piv[0], rp.piv[1]}},
                {"pos", QJsonArray{rp.pos.x, rp.pos.y, rp.pos.z}}, {"quat", QJsonArray{rp.quat.x, rp.quat.y, rp.quat.z, rp.quat.w}}, {"scl", QJsonArray{rp.scl.x, rp.scl.y, rp.scl.z}}};
  if (rp.follows() && rp.hasLoc) { QJsonArray l; for (double v : rp.loc) l.append(v); o.insert("loc", l); }
  if (rp.lock.isObject()) o.insert("lock", rp.lock);
  return o;
}

}  // namespace refs

}  // namespace hm
