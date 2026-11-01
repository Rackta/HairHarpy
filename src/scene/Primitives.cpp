#include "scene/Primitives.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>

namespace hm::prim {

namespace {
const double PI2 = 6.283185307179586;
// the mesh as it is made: points welded where they meet (a cylinder's seam, a sphere's poles, a cone's tip), a corner
// repeated round a polygon dropped (a quad at a pole — a triangle)
struct Build {
  std::vector<Vec3> P; std::vector<std::vector<int>> F; std::map<std::array<long long, 3>, int> at; double q = 1e-6;
  int v(const Vec3& p) { const std::array<long long, 3> k{std::llround(p.x / q), std::llround(p.y / q), std::llround(p.z / q)};
    auto f = at.find(k); if (f != at.end()) return f->second; P.push_back(p); return at[k] = int(P.size()) - 1; }
  void face(std::vector<int> f) { std::vector<int> g; for (int x : f) if (g.empty() || g.back() != x) g.push_back(x); while (g.size() > 1 && g.front() == g.back()) g.pop_back();
    if (g.size() >= 3) F.push_back(g); }
  // a grid of points S(i, j), i 0…nu, j 0…nv: quads, turned so their normal agrees with out(i + ½, j + ½)
  void grid(int nu, int nv, const std::function<Vec3(double, double)>& S, const std::function<Vec3(const Vec3&)>& out) {
    std::vector<std::vector<int>> id(size_t(nu) + 1, std::vector<int>(size_t(nv) + 1));
    for (int i = 0; i <= nu; i++) for (int j = 0; j <= nv; j++) id[size_t(i)][size_t(j)] = v(S(double(i) / nu, double(j) / nv));
    for (int i = 0; i < nu; i++) for (int j = 0; j < nv; j++) {
      std::vector<int> f{id[size_t(i)][size_t(j)], id[size_t(i) + 1][size_t(j)], id[size_t(i) + 1][size_t(j) + 1], id[size_t(i)][size_t(j) + 1]};
      const Vec3 c = S((i + 0.5) / nu, (j + 0.5) / nv);
      const Vec3 n = (S((i + 1.0) / nu, (j + 0.5) / nv) - S(double(i) / nu, (j + 0.5) / nv)).cross(S((i + 0.5) / nu, (j + 1.0) / nv) - S((i + 0.5) / nu, double(j) / nv));
      if (n.dot(out(c)) < 0) std::reverse(f.begin(), f.end());
      face(f); } }
  PolyMesh mesh() const {
    PolyMesh m; m.pos = P; m.faceStart = {0};
    for (auto& f : F) { m.faceVerts.insert(m.faceVerts.end(), f.begin(), f.end()); m.faceStart.push_back(int(m.faceVerts.size())); }
    m.faceGroup.assign(F.size(), 0); m.groupNames = {"Default"}; return m; }
};
int I(const QJsonObject& p, const char* k, int d) { return std::max(1, p.value(k).toInt(d)); }
double D(const QJsonObject& p, const char* k, double d) { return p.value(k).toDouble(d); }
}  // namespace

const std::vector<QString>& types() { static const std::vector<QString> t{"cone", "cyl", "pcube", "pcyl", "pplane", "psphere", "ring", "sphere"}; return t; }

QString label(const QString& t) {
  return t == "cone" ? "Cone 3D" : t == "cyl" ? "Cylinder 3D" : t == "pcube" ? "PolyCube" : t == "pcyl" ? "PolyCylinder" : t == "pplane" ? "PolyPlane"
       : t == "psphere" ? "PolySphere" : t == "ring" ? "Ring 3D" : t == "sphere" ? "Sphere 3D" : t;
}

const std::vector<Param>& params(const QString& t) {
  static const std::map<QString, std::vector<Param>> P{
    {"cone", {{"r", "Radius", 0.05, 5, 0.01, 0.5, false}, {"top", "Top radius", 0, 1, 0.01, 0, false}, {"h", "Height", 0.05, 5, 0.01, 1, false}, {"sides", "Sides", 3, 64, 1, 24, true}, {"hseg", "Height segments", 1, 64, 1, 8, true}, {"cap", "Cap rings", 1, 16, 1, 3, true}}},
    {"cyl", {{"r", "Radius", 0.05, 5, 0.01, 0.5, false}, {"h", "Height", 0.05, 5, 0.01, 1, false}, {"sides", "Sides", 3, 64, 1, 24, true}, {"hseg", "Height segments", 1, 64, 1, 8, true}, {"cap", "Cap rings", 1, 16, 1, 3, true}}},
    {"pcube", {{"w", "Width", 0.05, 5, 0.01, 1, false}, {"h", "Height", 0.05, 5, 0.01, 1, false}, {"d", "Depth", 0.05, 5, 0.01, 1, false}, {"sx", "Segments X", 1, 32, 1, 1, true}, {"sy", "Segments Y", 1, 32, 1, 1, true}, {"sz", "Segments Z", 1, 32, 1, 1, true}}},
    {"pcyl", {{"r", "Radius", 0.05, 5, 0.01, 0.5, false}, {"h", "Height", 0.05, 5, 0.01, 1, false}, {"sides", "Sides", 3, 64, 1, 8, true}, {"hseg", "Height segments", 1, 64, 1, 1, true}}},
    {"pplane", {{"w", "Width", 0.05, 5, 0.01, 1, false}, {"d", "Depth", 0.05, 5, 0.01, 1, false}, {"sx", "Segments X", 1, 64, 1, 4, true}, {"sz", "Segments Z", 1, 64, 1, 4, true}}},
    {"psphere", {{"r", "Radius", 0.05, 5, 0.01, 0.5, false}, {"div", "Divisions", 1, 32, 1, 4, true}}},
    {"ring", {{"R", "Radius", 0.05, 5, 0.01, 0.5, false}, {"r", "Thickness", 0.01, 2, 0.01, 0.15, false}, {"sides", "Sides", 3, 96, 1, 24, true}, {"rsides", "Tube sides", 3, 48, 1, 12, true}}},
    {"sphere", {{"r", "Radius", 0.05, 5, 0.01, 0.5, false}, {"sides", "Sides", 3, 96, 1, 24, true}, {"rings", "Rings", 2, 64, 1, 12, true}}},
  };
  static const std::vector<Param> none; auto f = P.find(t); return f == P.end() ? none : f->second;
}

QJsonObject defaults(const QString& t) { QJsonObject o; for (auto& p : params(t)) { if (p.integer) o.insert(p.key, int(p.def)); else o.insert(p.key, p.def); } return o; }

PolyMesh build(const QString& t, const QJsonObject& p, const Vec3& c, double u) {
  Build B; B.q = std::max(1e-9, u * 1e-6);
  auto from = [&](const Vec3& o) { return [o](const Vec3& x) { return x - o; }; };   // (outward: away from a point)
  if (t == "pcube") {
    const double w = D(p, "w", 1) * u / 2, h = D(p, "h", 1) * u / 2, d = D(p, "d", 1) * u / 2; const int sx = I(p, "sx", 1), sy = I(p, "sy", 1), sz = I(p, "sz", 1);
    auto L = [](double a, double b, double s) { return a + (b - a) * s; };
    for (int sg : {-1, 1}) {
      B.grid(sy, sz, [&](double a, double b) { return c + Vec3(sg * w, L(-h, h, a), L(-d, d, b)); }, from(c));
      B.grid(sx, sz, [&](double a, double b) { return c + Vec3(L(-w, w, a), sg * h, L(-d, d, b)); }, from(c));
      B.grid(sx, sy, [&](double a, double b) { return c + Vec3(L(-w, w, a), L(-h, h, b), sg * d); }, from(c)); }
  } else if (t == "pplane") {
    const double w = D(p, "w", 1) * u / 2, d = D(p, "d", 1) * u / 2;
    B.grid(I(p, "sx", 4), I(p, "sz", 4), [&](double a, double b) { return c + Vec3(-w + 2 * w * a, 0, -d + 2 * d * b); }, [](const Vec3&) { return Vec3(0, 1, 0); });
  } else if (t == "psphere") {
    const double r = D(p, "r", 0.5) * u; const int n = I(p, "div", 4);
    auto onS = [&](const Vec3& q) { return c + q.normalized() * r; };
    for (int sg : {-1, 1}) {
      B.grid(n, n, [&](double a, double b) { return onS(Vec3(sg, -1 + 2 * a, -1 + 2 * b)); }, from(c));
      B.grid(n, n, [&](double a, double b) { return onS(Vec3(-1 + 2 * a, sg, -1 + 2 * b)); }, from(c));
      B.grid(n, n, [&](double a, double b) { return onS(Vec3(-1 + 2 * a, -1 + 2 * b, sg)); }, from(c)); }
  } else if (t == "sphere") {
    const double r = D(p, "r", 0.5) * u;
    B.grid(I(p, "sides", 24), I(p, "rings", 12), [&](double a, double b) { const double th = a * PI2, ph = b * PI2 / 2; return c + Vec3(std::sin(ph) * std::cos(th), -std::cos(ph), std::sin(ph) * std::sin(th)) * r; }, from(c));
  } else if (t == "ring") {
    const double R = D(p, "R", 0.5) * u, r = D(p, "r", 0.15) * u;
    B.grid(I(p, "sides", 24), I(p, "rsides", 12), [&](double a, double b) { const double th = a * PI2, ph = b * PI2; const Vec3 ax(std::cos(th), 0, std::sin(th));
      return c + ax * (R + r * std::cos(ph)) + Vec3(0, r * std::sin(ph), 0); },
      [&](const Vec3& x) { Vec3 q = x - c; q.y = 0; return x - (c + (q.lengthSq() > 0 ? q.normalized() : Vec3(1, 0, 0)) * R); });
  } else if (t == "cyl" || t == "pcyl" || t == "cone") {
    const double r0 = D(p, "r", 0.5) * u, h = D(p, "h", 1) * u, r1 = t == "cone" ? D(p, "top", 0) * r0 : r0;
    const int sides = I(p, "sides", t == "pcyl" ? 8 : 24), hs = I(p, "hseg", t == "pcyl" ? 1 : 8), cr = t == "pcyl" ? 1 : I(p, "cap", 3);
    auto rim = [&](double a, double y, double r) { return c + Vec3(std::cos(a * PI2) * r, y, std::sin(a * PI2) * r); };
    B.grid(sides, hs, [&](double a, double b) { return rim(a, -h / 2 + h * b, r0 + (r1 - r0) * b); }, [&](const Vec3& x) { return Vec3(x.x - c.x, 0, x.z - c.z); });
    if (t == "pcyl") {                                                  // (PolyCylinder: each cap one polygon)
      std::vector<int> lo, hi; for (int k = 0; k < sides; k++) { lo.push_back(B.v(rim(double(k) / sides, -h / 2, r0))); hi.push_back(B.v(rim(double(k) / sides, h / 2, r1))); }
      std::reverse(hi.begin(), hi.end()); B.face(lo); B.face(hi);
    } else for (int top = 0; top < 2; top++) { const double y = top ? h / 2 : -h / 2, rr = top ? r1 : r0; if (rr <= 1e-12) continue;   // (the caps: rings to the middle — a fan of triangles there)
      B.grid(sides, cr, [&](double a, double b) { return rim(a, y, rr * (1 - b)); }, [top](const Vec3&) { return Vec3(0, top ? 1 : -1, 0); }); }
  }
  return B.mesh();
}

}  // namespace hm::prim
