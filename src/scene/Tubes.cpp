#include "scene/Tubes.h"
#include "scene/Edit.h"
#include <QColor>
#include <QJsonDocument>
#include <QObject>
#include <QSettings>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace hm::tube {

namespace {

const double PI = 3.14159265358979323846;
const double TWIST_CW = -1;     // the sign that makes a positive twist clockwise seen from the root along the lock

double ease(double s) { return s * s * (3 - 2 * s); }
double tri(double a, double b, double c, double t) { return t < 0.5 ? a + (b - a) * ease(t * 2) : b + (c - b) * ease(t * 2 - 1); }
double rad(double d) { return d * PI / 180; }
double num(const QJsonObject& p, const char* k, double def) { QJsonValue v = p.value(k); return v.isDouble() ? v.toDouble() : def; }   // (x ?? def)
double orZero(const QJsonObject& p, const char* k) { double v = num(p, k, 0); return std::isfinite(v) ? v : 0; }                     // (x || 0)

std::vector<Vec3> resample(const std::vector<Vec3>& pts, int n) {
  std::vector<double> L{0}; for (size_t i = 1; i < pts.size(); i++) L.push_back(L.back() + pts[i].distanceTo(pts[i - 1]));
  double tot = L.back(); std::vector<Vec3> out; size_t j = 0;
  for (int i = 0; i < n; i++) {
    double d = tot * i / (n - 1);
    while (j + 2 < pts.size() && L[j + 1] < d) j++;
    double sl = L[j + 1] - L[j], t = sl > 0 ? clamp((d - L[j]) / sl, 0, 1) : 0;
    out.push_back(pts[j].lerp(pts[j + 1], t));
  }
  return out;
}

// THREE.Quaternion.setFromUnitVectors(a, b) applied to v
Vec3 rotateFromTo(const Vec3& a, const Vec3& b, const Vec3& v) {
  double r = a.dot(b) + 1; Quat q;
  if (r < 2.220446049250313e-16) { r = 0; q = std::abs(a.x) > std::abs(a.z) ? Quat{-a.y, a.x, 0, r} : Quat{0, -a.z, a.y, r}; }
  else { Vec3 c = a.cross(b); q = Quat{c.x, c.y, c.z, r}; }
  return q.normalized().rotate(v);
}

struct Frame { Vec3 p, T, N, B; };
Vec3 perp(const Vec3& v, const Vec3& t) { return v - t * v.dot(t); }

// Frames along the curve: the start normal points outwards from the model's vertical axis (up if the curve runs along
// that direction), then it is carried along the curve without twisting (parallel transport). B = N × T.
std::vector<Frame> framesAlong(const std::vector<Vec3>& P, const Vec3& center) {
  const size_t NR = P.size(); std::vector<Vec3> T(NR);
  for (size_t i = 0; i < NR; i++) { Vec3 t = P[std::min(i + 1, NR - 1)] - P[i ? i - 1 : 0]; T[i] = t.lengthSq() > 1e-20 ? t.normalized() : Vec3(0, -1, 0); }
  Vec3 h(P[0].x - center.x, 0, P[0].z - center.z), N = perp(h, T[0]);
  if (h.lengthSq() < 1e-12 || N.lengthSq() < 0.09 * h.lengthSq()) N = perp(Vec3(0, 1, 0), T[0]);
  if (N.lengthSq() < 1e-12) N = perp(Vec3(1, 0, 0), T[0]);
  N = N.normalized();
  std::vector<Frame> out; out.reserve(NR);
  for (size_t i = 0; i < NR; i++) {
    if (i) { N = rotateFromTo(T[i - 1], T[i], N); N = perp(N, T[i]); if (N.lengthSq() < 1e-12) N = perp(Vec3(0, 1, 0), T[i]); N = N.normalized(); }
    out.push_back({P[i], T[i], N, N.cross(T[i]).normalized()});
  }
  return out;
}

struct Extra { int id; QString kind; QJsonObject p; std::vector<double> mask; std::vector<int> only; bool ignore; };

double curlTurns(const Extra& x) { double s = 0; for (int k = 0; k < 4; k++) s += num(x.p, ("d" + std::to_string(k)).c_str(), 1); return s; }
int extraRings(const std::vector<Extra>& ex, int NR) {
  double n = NR;
  for (auto& x : ex) { if (x.kind == "curl") n = std::max(n, curlTurns(x) * 24 + 24); if (x.kind == "noise") n = std::max(n, (num(x.p, "freq", 0) ? num(x.p, "freq", 0) : 1) * 16 + 24); }
  return std::min(int(std::floor(n + 0.5)), 900);
}
// how much of extra x works at t: every mask of the same modifier that applies to x lowers it (100% mask → nothing)
double extraWeight(const Extra& x, double t, const std::vector<Extra>& ex) {
  double w = 1;
  for (auto& m : ex) {
    if (m.kind != "mask" || m.id == x.id) continue;
    bool in = std::find(m.only.begin(), m.only.end(), x.id) != m.only.end();
    bool applies = m.only.empty() || (m.ignore ? !in : in);
    if (applies) w *= 1 - maskAt(m.mask, t);
  }
  return w;
}
// smooth value through 4 control values at the root, 1/3, 2/3 and the tip; coil radius 'r' falls back to the single
// radius of older saves
double four(const Extra& x, const char* key, double t) {
  auto v = [&](int k) { k = std::clamp(k, 0, 3); QJsonValue q = x.p.value(QString(key) + QString::number(k)); if (q.isDouble()) return q.toDouble(); return QString(key) == "r" ? orZero(x.p, "radius") : 1.0; };
  double u = clamp(t, 0, 1) * 3; int k = std::min(int(std::floor(u)), 2); double s = ease(u - k);
  return v(k) + (v(k + 1) - v(k)) * s;
}
double rootIn(double t) { return ease(clamp(t / 0.08, 0, 1)); }       // the root stays where it is

double tubeRadius(const QJsonObject& p, double t) {
  double r = tri(num(p, "r0", 0), num(p, "r1", 0), num(p, "r2", 0), t);
  double L = clamp(num(p, "tipLen", 0.25), 0.01, 1);
  if (p.value("collapse").toBool() && t > 1 - L) { double u = (1 - t) / L; r *= u * (2 - u); }
  return std::max(r, 0.0);
}

// anti-clipping 3 ("Thin where crowded"): where other parts of the lock come closer than its thickness, the cross-section
// gets thinner so the surfaces just touch (only the thickness changes)
void thinCrowded(const QJsonObject& tp, const Extra& x, const std::vector<Vec3>& P, std::vector<double>& thick, const std::vector<Extra>& ex) {
  const int n = int(P.size()); std::vector<double> L{0}; for (int i = 1; i < n; i++) L.push_back(L[size_t(i - 1)] + P[size_t(i)].distanceTo(P[size_t(i - 1)]));
  auto r = [&](int i) { return tubeRadius(tp, double(i) / (n - 1)) * thick[size_t(i)]; };
  int step = std::max(1, n / 400); std::vector<double> f(size_t(n), 1.0); double k = clamp(num(x.p, "thin", 0), 0, 1);
  for (int i = 0; i < n; i++) {
    double ri = r(i); if (!(ri > 0)) continue;
    double room = 1e300;
    for (int j = 0; j < n; j += step) {
      double rj = r(j); if (std::abs(L[size_t(j)] - L[size_t(i)]) < 3 * std::max(ri, rj)) continue;
      double d = P[size_t(i)].distanceTo(P[size_t(j)]); room = std::min(room, d * ri / std::max(ri + rj, 1e-12));
    }
    if (room < ri) f[size_t(i)] = 1 - k * (1 - std::max(room / ri, 0.1)) * extraWeight(x, double(i) / (n - 1), ex);
  }
  int h = std::max(1, int(std::floor(n * 0.015 + 0.5))); std::vector<double> pre{0}; for (double v : f) pre.push_back(pre.back() + v);
  for (int i = 0; i < n; i++) { int a = std::max(0, i - h), b = std::min(n - 1, i + h); thick[size_t(i)] *= std::min(f[size_t(i)], (pre[size_t(b + 1)] - pre[size_t(a)]) / (b - a + 1)); }
}

void applyExtras(const QJsonObject& tp, const std::vector<Extra>& ex, std::vector<Frame>& F, std::vector<double>& twist, std::vector<double>& thick, const Vec3& center) {
  const int n = int(F.size()); std::vector<Vec3> off(static_cast<size_t>(n)); twist.assign(size_t(n), 0); thick.assign(size_t(n), 1);
  for (auto& x : ex) {
    const QJsonObject& p = x.p;
    if (x.kind == "noise") {
      double seed = num(p, "seed", 1) * 12.9898;
      auto ph = [&](int k) { return std::fmod(std::sin(seed * (k + 1) * 78.233) * 43758.5453, 2 * PI); };
      for (int i = 0; i < n; i++) {
        double t = double(i) / (n - 1), w = extraWeight(x, t, ex) * rootIn(t) * orZero(p, "amp"); if (!w) continue;
        double a = rad(tri(orZero(p, "rot0"), orZero(p, "rot1"), orZero(p, "rot2"), t)); const Frame& f = F[size_t(i)];
        Vec3 U = f.N * std::cos(a) + f.B * std::sin(a), Vv = f.B * std::cos(a) - f.N * std::sin(a);
        double fr = orZero(p, "freq") ? orZero(p, "freq") : 1, q = 2 * PI * fr * t, k1 = clamp(orZero(p, "pure"), 0, 1);
        double s1 = (0.75 * std::sin(q + ph(0)) + 0.25 * std::sin(2.13 * q + ph(1))) * (1 - k1) + std::sin(q + ph(0)) * k1;
        double s2 = (0.75 * std::sin(1.07 * q + ph(2)) + 0.25 * std::sin(2.71 * q + ph(3))) * (1 - k1) + std::cos(q + ph(0)) * k1;
        off[size_t(i)] += U * (w * num(p, "sx", 1) * s1);
        if (!p.value("plane").toBool()) off[size_t(i)] += Vv * (w * num(p, "sy", 1) * s2);
      }
    }
    if (x.kind == "curl") {
      double th = rad(orZero(p, "phase"));
      // anti-clipping 1 ("Smooth the path"): the spiral winds around a smoothed path (the root stays in place)
      std::vector<Frame> Gs; const std::vector<Frame>* G = &F;
      double sm = clamp(orZero(p, "smooth"), 0, 1);
      if (sm > 0) {
        std::vector<Vec3> P; for (auto& f : F) P.push_back(f.p);
        int h = std::max(1, int(std::floor(sm * n * 0.12 + 0.5)));
        for (int pass = 0; pass < 3; pass++) {
          std::vector<Vec3> pre{Vec3()}; for (auto& q : P) pre.push_back(pre.back() + q);
          std::vector<Vec3> Q(static_cast<size_t>(n)); for (int i = 0; i < n; i++) { int a = std::max(0, i - h), b = std::min(n - 1, i + h); Q[size_t(i)] = (pre[size_t(b + 1)] - pre[size_t(a)]) / double(b - a + 1); }
          P = Q;
        }
        std::vector<Vec3> M; for (int i = 0; i < n; i++) M.push_back(F[size_t(i)].p.lerp(P[size_t(i)], rootIn(double(i) / (n - 1))));
        Gs = framesAlong(M, center); G = &Gs;
      }
      // anti-clipping 2 ("Shrink on bends"): where the path bends tighter than ~3 coil radii, the radius smoothly goes down
      double shr = clamp(orZero(p, "shrink"), 0, 1); std::vector<double> fac(size_t(n), 1.0);
      if (shr > 0) {
        int k = std::max(1, int(std::floor(n * 0.02 + 0.5)));
        for (int i = 0; i < n; i++) {
          int a = std::max(0, i - k), b = std::min(n - 1, i + k); double ang = std::acos(clamp((*G)[size_t(a)].T.dot((*G)[size_t(b)].T), -1, 1));
          double rho = (*G)[size_t(a)].p.distanceTo((*G)[size_t(b)].p) / std::max(ang, 1e-6), R0 = std::max(four(x, "r", double(i) / (n - 1)), 1e-9);
          fac[size_t(i)] = 1 - shr * (1 - clamp((rho - 0.3 * R0) / (R0 * 2.7), 0.05, 1));
        }
        std::vector<double> pre{0}; int h2 = 3 * k; for (double v : fac) pre.push_back(pre.back() + v);
        for (int i = 0; i < n; i++) { int a = std::max(0, i - h2), b = std::min(n - 1, i + h2); fac[size_t(i)] = (pre[size_t(b + 1)] - pre[size_t(a)]) / (b - a + 1); }
      }
      for (int i = 0; i < n; i++) {
        double t = double(i) / (n - 1), w = extraWeight(x, t, ex);
        if (i) th += 2 * PI * four(x, "d", t) * 4 / (n - 1) * w * (p.value("rev").toBool() ? -1 : 1);   // (rev: the spiral winds the other way)
        const Frame& f = (*G)[size_t(i)]; double R = four(x, "r", t) * fac[size_t(i)] * w * rootIn(t);
        if (G != &F) off[size_t(i)] += ((*G)[size_t(i)].p - F[size_t(i)].p) * w;
        off[size_t(i)] += f.N * (R * std::cos(th)) + f.B * (R * std::sin(th));
        twist[size_t(i)] += (p.value("follow").toBool() ? th : 0) + rad(orZero(p, "spin")) * w;
        thick[size_t(i)] *= 1 + (num(p, "thick", 1) - 1) * w;
      }
    }
    if (x.kind == "thick" && !x.mask.empty())                         // painted weights: −1 thinner (to a point) … +1 twice as thick
      for (int i = 0; i < n; i++) { double t = double(i) / (n - 1); thick[size_t(i)] *= std::max(0.02, 1 + maskAt(x.mask, t) * extraWeight(x, t, ex)); }
    if (x.kind == "twist" && !x.mask.empty()) {                       // painted weights: +1 — clockwise by the angle, −1 — anticlockwise
      double a = rad(num(p, "deg", 180)) * TWIST_CW;
      for (int i = 0; i < n; i++) { double t = double(i) / (n - 1); twist[size_t(i)] += a * maskAt(x.mask, t) * extraWeight(x, t, ex); }
    }
  }
  std::vector<Vec3> P; for (int i = 0; i < n; i++) P.push_back(F[size_t(i)].p + off[size_t(i)]);
  for (auto& x : ex) if (x.kind == "curl" && num(x.p, "thin", 0) > 0) thinCrowded(tp, x, P, thick, ex);
  F = framesAlong(P, center);
}

std::vector<Extra> activeExtras(const Scene& sc, int tube) {
  std::vector<Extra> out;
  for (int id : extrasOf(sc, tube)) {
    const Item* x = sc.item(id); if (!x->visible) continue;
    Extra e{id, x->raw.value("kind").toString(), x->raw.value("p").toObject(), maskOf(x->raw.value("mask")), onlyList(sc, *x), x->raw.value("p").toObject().value("ignore").toBool()};
    out.push_back(std::move(e));
  }
  return out;
}

const std::vector<Vec3>* samplesOf(const CurveCache& cc, int id) { auto it = cc.find(id); return it == cc.end() || it->second.p.size() < 2 ? nullptr : &it->second.p; }

std::vector<int> outlinerOrder(const Scene& sc) {
  std::vector<int> out;
  std::function<void(const std::vector<int>&)> walk = [&](const std::vector<int>& ids) {
    for (int id : ids) { const Item* it = sc.item(id); if (!it) continue; out.push_back(id); if (it->type == ItemType::Group || it->type == ItemType::Cocoon) walk(it->children); }
  };
  walk(sc.roots); return out;
}
// (a JS Map's order: the item list in file order)
std::vector<int> mapOrder(const Scene& sc, ItemType t) {
  std::vector<const Item*> v; for (auto& [id, it] : sc.items) if (it.type == t) v.push_back(&it);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; });
  std::vector<int> out; for (auto* it : v) out.push_back(it->id); return out;
}
void placeAfter(Scene& sc, int id, int after) {
  Item* it = sc.item(id); std::vector<int>& arr = it->parent >= 0 && sc.item(it->parent) ? sc.item(it->parent)->children : sc.roots;
  arr.erase(std::remove(arr.begin(), arr.end(), id), arr.end());
  auto at = std::find(arr.begin(), arr.end(), after); arr.insert(at == arr.end() ? arr.end() : at + 1, id);
}

}  // namespace

// ---------- profiles ----------
const std::vector<Preset>& builtinProfiles() {
  static const std::vector<Preset> P = [] {
    std::vector<Preset> v{{"Quad", {{-1, -0.35}, {1, -0.35}, {1, 0.35}, {-1, 0.35}}},
                          {"Trapezoid", {{-1, -0.35}, {1, -0.35}, {0.55, 0.35}, {-0.55, 0.35}}},
                          {"Triangle", {{-1, -0.35}, {1, -0.35}, {0, 0.5}}}};
    Profile c; for (int k = 0; k < 8; k++) c.push_back({std::cos(k * PI / 4 - PI / 2), std::sin(k * PI / 4 - PI / 2)});
    v.push_back({"Circle", c});
    Profile h; for (int k = 0; k < 6; k++) h.push_back({std::cos(k * PI / 5), std::sin(k * PI / 5) * 0.8 - 0.35});
    v.push_back({"Half circle", h});
    return v;
  }();
  return P;
}
std::vector<Preset> userProfiles() {
  std::vector<Preset> out;
  QJsonArray a = QJsonDocument::fromJson(QSettings().value("profiles").toByteArray()).array();
  for (const QJsonValue& v : a) { QJsonObject o = v.toObject(); if (o.value("name").toString().isEmpty() || !o.value("pts").isArray()) continue; out.push_back({o.value("name").toString(), profileOf(o.value("pts"))}); }
  return out;
}
void saveUserProfiles(const std::vector<Preset>& l) {
  QJsonArray a; for (auto& p : l) a.append(QJsonObject{{"name", p.name}, {"pts", profileJson(p.pts)}});
  QSettings().setValue("profiles", QJsonDocument(a).toJson(QJsonDocument::Compact));
}
std::optional<Profile> profileByName(const QString& name) {
  for (auto& p : builtinProfiles()) if (p.name == name) return p.pts;
  for (auto& p : userProfiles()) if (p.name == name) return p.pts;
  return std::nullopt;
}
Profile profileOf(const QJsonValue& v) { Profile P; for (const QJsonValue& q : v.toArray()) { QJsonArray a = q.toArray(); if (a.size() >= 2) P.push_back({a[0].toDouble(), a[1].toDouble()}); } return P; }
QJsonArray profileJson(const Profile& p) { QJsonArray a; for (auto& q : p) a.append(QJsonArray{q[0], q[1]}); return a; }
std::array<Profile, 3> tubeProfs(const Item& t) {
  QJsonArray ps = t.raw.value("profs").toArray();
  if (ps.size() == 3) {
    std::array<Profile, 3> P{profileOf(ps[0]), profileOf(ps[1]), profileOf(ps[2])};
    if (P[1].size() == P[0].size() && P[2].size() == P[0].size()) return P;
  }
  Profile p = profileOf(t.raw.value("profile")); return {p, p, p};
}
bool samePt(const std::array<double, 2>& a, const std::array<double, 2>& b) { return std::abs(a[0] - b[0]) < 1e-9 && std::abs(a[1] - b[1]) < 1e-9; }
std::vector<int> gluedRun(const Profile& pts, int i) {
  int n = int(pts.size()); std::vector<int> run{i};
  for (int k = (i + 1) % n; k != i && samePt(pts[size_t(k)], pts[size_t(i)]); k = (k + 1) % n) run.push_back(k);
  for (int k = (i - 1 + n) % n; k != i && std::find(run.begin(), run.end(), k) == run.end() && samePt(pts[size_t(k)], pts[size_t(i)]); k = (k - 1 + n) % n) run.insert(run.begin(), k);
  return run;
}
QColor profColor(int j) {
  static const unsigned C[] = {0xc77d7d, 0x7dab7d, 0x7d95c7, 0xc7b06d, 0xa67dc7, 0x6db7b7, 0xc78f5e, 0x9aa35e, 0xc77daa, 0x6d8fa3};
  return QColor::fromRgb(C[size_t(j) % 10]);
}

// ---------- extras ----------
XKind xkind(const QString& k) {
  return k == "mask" ? XKind::Mask : k == "thick" ? XKind::Thick : k == "twist" ? XKind::Twist : k == "noise" ? XKind::Noise : k == "curl" ? XKind::Curl : k == "xform" ? XKind::Xform : XKind::Unknown;
}
QString xkindName(const QString& k) {
  static const std::map<QString, QString> N{{"mask", "Mask"}, {"thick", "Thickness"}, {"twist", "Twist"}, {"noise", "Noise"}, {"curl", "Curls"}, {"xform", "Transform"}};
  auto it = N.find(k); return it == N.end() ? k : it->second;
}
QString xkindIcon(const QString& k) {
  static const std::map<QString, QString> N{{"mask", "◐"}, {"thick", "⬤"}, {"twist", "⟳"}, {"noise", "≋"}, {"curl", "➰"}, {"xform", "✥"}};
  auto it = N.find(k); return it == N.end() ? QString("✦") : it->second;
}
bool painted(const QString& k) { return k == "mask" || k == "thick" || k == "twist"; }
std::vector<int> extrasOf(const Scene& sc, int tube) {
  std::vector<int> out;
  for (int id : mapOrder(sc, ItemType::XMod)) { const Item* x = sc.item(id); if (x->raw.value("tube").isDouble() && x->raw.value("tube").toInt() == tube) out.push_back(id); }
  return out;
}
std::vector<int> tubesOf(const Scene& sc) { return mapOrder(sc, ItemType::Tube); }
int tubeOfCurve(const Scene& sc, int curve) { for (int t : tubesOf(sc)) if (sc.item(t)->raw.value("src").toInt(-1) == curve) return t; return -1; }
bool profiledAt(const Scene& sc, int anchor) {
  for (auto& [id, c] : sc.items) if (c.type == ItemType::Curve && std::find(c.anchors.begin(), c.anchors.end(), anchor) != c.anchors.end() && tubeOfCurve(sc, id) >= 0) return true;
  return false;
}
bool onConnector(const Scene& sc, int curve) {
  const Item* c = sc.item(curve); if (!c) return false;
  for (int a : c->anchors) { const Anchor* A = sc.anchor(a); if (A && A->raw.value("at").isObject()) return true;
    for (auto& [id, k] : sc.items) if (id != curve && k.type == ItemType::Curve && !k.conn.isEmpty() && std::find(k.anchors.begin(), k.anchors.end(), a) != k.anchors.end()) return true; }
  return false;
}
std::vector<int> onlyList(const Scene& sc, const Item& m) {
  QJsonValue o = m.raw.value("p").toObject().value("only"); std::vector<int> out;
  if (o.isArray()) { for (const QJsonValue& v : o.toArray()) if (sc.item(v.toInt())) out.push_back(v.toInt()); }
  else if (o.isDouble() && sc.item(o.toInt())) out.push_back(o.toInt());
  return out;
}
double maskAt(const std::vector<double>& m, double t) {
  if (m.size() < 2) return 0;
  double x = clamp(t, 0, 1) * double(m.size() - 1); size_t i = std::min(size_t(std::floor(x)), m.size() - 2);
  return m[i] + (m[i + 1] - m[i]) * (x - double(i));
}
std::vector<double> maskOf(const QJsonValue& v) { std::vector<double> m; for (const QJsonValue& x : v.toArray()) m.push_back(x.toDouble()); return m; }

// Automatic radius: half the typical wall width of the locks already in the scene (the median); without walls — 6% of the
// curve length. Always kept between 2% and 20% of the curve length.
double autoTubeRadius(const Scene& sc, const CurveCache& cc, int curve) {
  auto it = cc.find(curve); double len = it == cc.end() || it->second.arc.empty() ? 0 : it->second.arc.back();
  std::vector<double> ws;
  for (int sid : mapOrder(sc, ItemType::Surface)) {
    const Item* s = sc.item(sid); auto A = samplesOf(cc, s->a), B = samplesOf(cc, s->b); if (!A || !B) continue;
    const Vec3& p = (*A)[A->size() >> 1]; double d = 1e300;
    for (auto& q : *B) d = std::min(d, (q - p).lengthSq());
    ws.push_back(std::sqrt(d));
  }
  std::sort(ws.begin(), ws.end());
  double r = ws.empty() ? len * 0.06 : ws[ws.size() >> 1] * 0.5;
  double v = clamp(r, len * 0.02, len * 0.2);
  if (!(v > 0)) { Box3 b = sc.bounds(); Vec3 s = b.size(); v = std::max({s.x, s.y, s.z, 1e-9}) * 0.01; }
  return v;
}
void ensureTubeRef(Scene& sc, const CurveCache& cc, Item& t) {
  QJsonObject p = t.raw.value("p").toObject(); if (num(p, "rRef", 0)) return;
  if (!samplesOf(cc, t.raw.value("src").toInt(-1))) return;
  double r = autoTubeRadius(sc, cc, t.raw.value("src").toInt());
  p.insert("rRef", r); p.insert("r0", r); p.insert("r1", r); p.insert("r2", r); t.raw.insert("p", p);
}

// ---------- the sweep ----------
// With Extra modifiers the centre line is displaced and the profile twisted / scaled first; the profile is then carried
// along the displaced line. Each profile point goes from the start profile to the middle and end ones like the radius.
std::vector<std::vector<Vec3>> tubeRings(const Scene& sc, const Item& t, const std::vector<Vec3>& curvePts, int NR, const Vec3& center, const QJsonObject* pOverride) {
  QJsonObject p = pOverride ? *pOverride : t.raw.value("p").toObject();
  auto ex = activeExtras(sc, t.id);
  std::vector<Frame> F = framesAlong(resample(curvePts, ex.empty() ? NR : extraRings(ex, NR)), center);
  std::vector<double> twist, thick;
  if (!ex.empty()) applyExtras(p, ex, F, twist, thick, center);
  auto P = tubeProfs(t);
  const int n1 = int(F.size()); std::vector<std::vector<Vec3>> out; out.reserve(size_t(n1));
  for (int i = 0; i < n1; i++) {
    const Frame& f = F[size_t(i)]; double tt = double(i) / (n1 - 1), r = tubeRadius(p, tt) * (thick.empty() ? 1 : thick[size_t(i)]);
    double a = rad(tri(orZero(p, "rot0"), orZero(p, "rot1"), orZero(p, "rot2"), tt)) + (twist.empty() ? 0 : twist[size_t(i)]);
    Vec3 nn = f.N * std::cos(a) + f.B * std::sin(a), b = f.B * std::cos(a) - f.N * std::sin(a);
    std::vector<Vec3> ring;
    for (size_t j = 0; j < P[0].size(); j++) {
      double x = tri(P[0][j][0], P[1][j][0], P[2][j][0], tt), y = tri(P[0][j][1], P[1][j][1], P[2][j][1], tt);
      ring.push_back(f.p + b * (x * r) + nn * (y * r));
    }
    out.push_back(std::move(ring));
  }
  return out;
}

std::optional<TubeMesh> buildTube(const Scene& sc, const CurveCache& cc, const Item& t, const Vec3& center) {
  auto S = samplesOf(cc, t.raw.value("src").toInt(-1));
  Profile prof = profileOf(t.raw.value("profile"));
  if (!S || prof.size() < 3) return std::nullopt;
  QJsonObject p = t.raw.value("p").toObject();
  if (!num(p, "rRef", 0)) { double r = autoTubeRadius(sc, cc, t.raw.value("src").toInt()); p.insert("rRef", r); p.insert("r0", r); p.insert("r1", r); p.insert("r2", r); }   // (ensureTubeRef)
  auto rings = tubeRings(sc, t, *S, TUBE_NR, center, &p);
  TubeMesh m; m.NR = int(rings.size()); m.n = int(rings.empty() ? 0 : rings[0].size());
  for (auto& R : rings) m.pos.insert(m.pos.end(), R.begin(), R.end());
  // painted weights show on the modifier: masks black, thickness orange (fatter) / blue (thinner), twist green (clockwise)
  // / pink (anticlockwise)
  std::vector<std::pair<QString, std::vector<double>>> masks;
  for (int id : extrasOf(sc, t.id)) { const Item* x = sc.item(id); QString k = x->raw.value("kind").toString(); if (x->visible && painted(k) && x->raw.value("mask").isArray()) masks.push_back({k, maskOf(x->raw.value("mask"))}); }
  if (!masks.empty()) {
    auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    const Vec3 base(lin(0x8f / 255.0), lin(0x78 / 255.0), lin(0xe0 / 255.0)), fat(1, 0.55, 0.15), thin(0.2, 0.62, 1), cw(0.3, 0.9, 0.45), ccw(1, 0.35, 0.75);
    for (int i = 0; i < m.NR; i++) {
      double tt = double(i) / (m.NR - 1), mv = 0, w = 0, wt = 0;
      for (auto& [k, M] : masks) { double v = maskAt(M, tt); if (k == "mask") mv = std::max(mv, v); else if (k == "thick") w += v; else wt += v; }
      w = clamp(w, -1, 1); wt = clamp(wt, -1, 1);
      Vec3 c = base.lerp(w >= 0 ? fat : thin, std::min(1.0, std::abs(w) * 1.5)).lerp(wt >= 0 ? cw : ccw, std::min(1.0, std::abs(wt) * 1.5)) * (1 - mv * 0.92);
      for (int j = 0; j < m.n; j++) m.col.push_back({float(c.x), float(c.y), float(c.z)});
    }
  }
  return m;
}

// ---------- apply / paste / convert ----------
std::vector<int> modTargets(const Scene& sc, const Selection& sel, const CurveCache& cc) {
  std::vector<int> out;
  for (int id : sel.items) {
    const Item* it = sc.item(id); if (!it) continue;
    if (it->type == ItemType::Tube) it = sc.item(it->raw.value("src").toInt(-1));
    if (!it || it->type != ItemType::Curve || it->ring >= 0 || !samplesOf(cc, it->id)) continue;
    if (std::find(out.begin(), out.end(), it->id) == out.end()) out.push_back(it->id);
  }
  return out;
}
std::vector<int> modTubes(const Scene& sc, const Selection& sel) {
  std::vector<int> out;
  for (int id : sel.items) {
    const Item* it = sc.item(id); if (!it) continue; int t = -1;
    if (it->type == ItemType::Tube) t = id;
    else if (it->type == ItemType::XMod && it->raw.value("tube").isDouble() && sc.item(it->raw.value("tube").toInt())) t = it->raw.value("tube").toInt();
    else if (it->type == ItemType::Curve) t = tubeOfCurve(sc, id);
    if (t >= 0 && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
  }
  return out;
}

namespace {
int newTube(Scene& sc, int c) {
  int num = edit::nextNumber(sc, ItemType::Tube);
  const Item* cv = sc.item(c);
  Item& t = sc.addItem(ItemType::Tube, cv->parent, QString("Modifier %1 (%2)").arg(num).arg(cv->name)); int id = t.id;
  placeAfter(sc, id, c);
  return id;
}
}  // namespace

QString applyModifier(Scene& sc, const CurveCache& cc, Selection& sel, const Profile& prof) {
  auto cs = modTargets(sc, sel, cc);
  if (cs.empty()) return QObject::tr("!Select a curve in the viewport, then press Apply");
  if (prof.size() < 3) return QObject::tr("!The profile needs at least 3 points");
  { std::vector<int> ok; QStringList banned; for (int c : cs) { if (tubeOfCurve(sc, c) < 0 && onConnector(sc, c)) banned << sc.item(c)->name; else ok.push_back(c); }   // (none on a crown's connector)
    if (ok.empty()) return QObject::tr("!A curve joined to a crown's connector can't take a profile modifier — %1").arg(banned.join(", "));
    cs = ok; }
  std::vector<int> made;
  for (int c : cs) {
    int t = tubeOfCurve(sc, c);
    if (t >= 0) { Item* ti = sc.item(t); ti->raw.insert("profile", profileJson(prof)); ti->raw.remove("profs"); made.push_back(t); continue; }   // re-apply: new profile, same settings
    double r = autoTubeRadius(sc, cc, c);
    t = newTube(sc, c);
    sc.item(t)->raw = QJsonObject{{"src", c}, {"profile", profileJson(prof)},
                                  {"p", QJsonObject{{"r0", r}, {"r1", r}, {"r2", r}, {"rRef", r}, {"rot0", 0}, {"rot1", 0}, {"rot2", 0}, {"collapse", true}, {"tipLen", 0.25}}}};
    made.push_back(t);
  }
  sel.anchors.clear(); sel.items = made;
  return made.size() == 1 ? QObject::tr("Modifier applied — its settings are in the properties panel") : QObject::tr("Modifier applied to %1 curves").arg(made.size());
}

QString pasteModifier(Scene& sc, const CurveCache& cc, Selection& sel, const Clip& clip) {
  if (!clip.profile.isArray()) return QObject::tr("!Copy a modifier first");
  auto cs = modTargets(sc, sel, cc);
  if (cs.empty()) return QObject::tr("!Select the curve(s) to paste the modifier onto");
  std::vector<int> made;
  for (int c : cs) {
    int t = tubeOfCurve(sc, c);
    if (t < 0) { t = newTube(sc, c); sc.item(t)->raw = QJsonObject{{"src", c}}; }
    Item* ti = sc.item(t);
    ti->raw.insert("profile", clip.profile); ti->raw.insert("p", clip.p);
    if (clip.profs.isArray()) { ti->raw.insert("profs", clip.profs); ti->raw.insert("profile", clip.profs.toArray().first()); } else ti->raw.remove("profs");
    made.push_back(t);
  }
  sel.anchors.clear(); sel.items = made;
  return QObject::tr("Modifier pasted onto %1 curve%2").arg(made.size()).arg(made.size() > 1 ? "s" : "");
}

// Convert to the usual workflow: a curve along every profile corner (anchors where the source curve has them, plus a few
// more where it has only a few), closed walls between neighbouring corners; with the tip collapsed all curves end in one
// shared anchor. The modifier and its source curve are removed.
QString convertTube(Scene& sc, const CurveCache& cc, Selection& sel, int tubeId, const Vec3& center) {
  const Item* it = sc.item(tubeId); if (!it) return "!";
  int src = it->raw.value("src").toInt(-1); const Item* c = sc.item(src);
  auto ci = cc.find(src);
  if (!c || ci == cc.end() || ci->second.p.size() < 2 || ci->second.arc.empty()) return QObject::tr("!The source curve is missing");
  const auto& arc = ci->second.arc;
  QJsonObject p = it->raw.value("p").toObject();
  auto rings = tubeRings(sc, *it, ci->second.p, 200, center);
  const int n = int(profileOf(it->raw.value("profile")).size()), NR = int(rings.size()); double tot = arc.back() ? arc.back() : 1;
  std::vector<double> fr; for (size_t k = 0; k < c->anchors.size(); k++) fr.push_back(arc[std::min(k * 24, arc.size() - 1)] / tot);
  while (fr.size() < 6) { std::vector<double> nx{fr[0]}; for (size_t k = 1; k < fr.size(); k++) { nx.push_back((fr[k - 1] + fr[k]) / 2); nx.push_back(fr[k]); } fr = nx; }
  // curls and noise need more anchors to keep their shape: about 8 per turn / wave
  auto ex = activeExtras(sc, tubeId); ex.erase(std::remove_if(ex.begin(), ex.end(), [](const Extra& x) { return x.kind == "mask"; }), ex.end());
  if (!ex.empty()) {
    double s = 0; for (auto& x : ex) s += x.kind == "curl" ? curlTurns(x) * 8 : (orZero(x.p, "freq") ? orZero(x.p, "freq") : 1) * 6;
    int need = std::min(160, int(std::ceil(s)) + 4);
    if (need > int(fr.size())) { fr.clear(); for (int k = 0; k < need; k++) fr.push_back(double(k) / (need - 1)); }
  }
  bool collapse = p.value("collapse").toBool();
  if (collapse) { double ts = 1 - clamp(num(p, "tipLen", 0.25), 0.01, 1); bool far = true; for (double f : fr) if (std::abs(f - ts) <= 0.04) far = false; if (far) { fr.push_back(ts); std::sort(fr.begin(), fr.end()); } }
  auto edgeAt = [&](int j, double f) { double x = clamp(f, 0, 1) * (NR - 1); int i = std::min(int(std::floor(x)), NR - 2); return rings[size_t(i)][size_t(j)].lerp(rings[size_t(i + 1)][size_t(j)], x - i); };
  int parent = c->parent;
  if (parent < 0) {
    // the new group takes the conversion settings of a hair group that has them
    QJsonValue conv; for (int gid : mapOrder(sc, ItemType::Group)) { const Item* g = sc.item(gid); if (g->raw.value("conv").isObject() && !g->raw.value("guidesGroup").toBool() && !g->raw.value("meshesGroup").toBool()) { conv = g->raw.value("conv"); break; } }
    edit::nextNumber(sc, ItemType::Group);
    Item& g = sc.addItem(ItemType::Group, -1, c->name + " lock"); g.expanded = true;
    QJsonObject cv = conv.toObject(); cv.insert("step", projectStep(sc));
    g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "auto"}, {"conv", cv}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                        {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
    parent = g.id; c = sc.item(src);
  }
  int tip = collapse ? sc.addAnchor(ci->second.p.back()).id : -1;
  // the anchors of every edge at every section k: corners glued in the profile there share one anchor (where they come
  // apart along the lock, the edges fork in a smooth Y)
  std::vector<std::vector<int>> A(size_t(n), std::vector<int>(fr.size(), -1));
  auto P3 = tubeProfs(*it);
  for (size_t k = 0; k < fr.size(); k++) {
    if (tip >= 0 && k == fr.size() - 1) { for (int j = 0; j < n; j++) A[size_t(j)][k] = tip; continue; }
    double f = fr[k]; Profile pr;
    for (int j = 0; j < n; j++) pr.push_back({tri(P3[0][size_t(j)][0], P3[1][size_t(j)][0], P3[2][size_t(j)][0], f), tri(P3[0][size_t(j)][1], P3[1][size_t(j)][1], P3[2][size_t(j)][1], f)});
    for (int j = 0; j < n; j++) {
      if (A[size_t(j)][k] >= 0) continue;
      Anchor& a = sc.addAnchor(edgeAt(j, f)); a.raw.insert("sc", int(k));          // cross-section k
      for (int i : gluedRun(pr, j)) A[size_t(i)][k] = a.id;
    }
  }
  // edges glued along the whole lock are one edge
  std::vector<int> keep; for (int j = 0; j < n; j++) if (keep.empty() || A[size_t(j)] != A[size_t(keep.back())]) keep.push_back(j);
  if (keep.size() > 1 && A[size_t(keep.back())] == A[size_t(keep[0])]) keep.pop_back();
  if (tip >= 0) sc.anchor(tip)->raw.insert("sc", int(fr.size() - 1));
  std::vector<int> made;
  QString cname = c->name; double cr = c->roundSet ? c->round : 0.6, cf = c->fairSet ? c->fair : 0.5;
  for (int j : keep) {
    Item& nc = edit::makeUserCurve(sc, parent); nc.name = QString("%1 edge %2").arg(cname).arg(j + 1);
    nc.anchors = A[size_t(j)]; nc.para = true; nc.raw.insert("para", true);   // anchor k of every edge — one cross-section
    nc.round = cr; nc.fair = cf; nc.roundSet = nc.fairSet = true;
    made.push_back(nc.id);
  }
  const size_t m = made.size();
  for (size_t j = 0; j < m; j++) { int s = edit::makeSurface(sc, made[j], made[(j + 1) % m]); if (s >= 0) { QJsonObject sp = sc.item(s)->raw.value("p").toObject(); sp.insert("bulge", 0); sc.item(s)->raw.insert("p", sp); } }
  Selection del; del.items = {tubeId, src};
  for (int x : extrasOf(sc, tubeId)) del.items.push_back(x);
  sc.reindex();
  edit::deleteItems(sc, cc, del);
  bool fork = false;
  for (size_t j = 0; j < m && !fork; j++) for (int a : sc.item(made[j])->anchors) { if (a == tip) continue; for (size_t i = 0; i < m; i++) if (i != j) { auto& an = sc.item(made[i])->anchors; if (std::find(an.begin(), an.end(), a) != an.end()) fork = true; } }
  sel.anchors.clear(); sel.items = made;
  sc.reindex();
  return QObject::tr("Converted: %1 curves and %1 walls (a closed volume lock)").arg(m) + (fork ? QObject::tr(" — glued corners make Y forks") : QString());
}

int newExtra(Scene& sc, const CurveCache& cc, int tubeId, const QString& kind) {
  const Item* t = sc.item(tubeId);
  QJsonObject tp = t->raw.value("p").toObject();
  double r = num(tp, "rRef", 0); if (!r) r = autoTubeRadius(sc, cc, t->raw.value("src").toInt(-1));
  QJsonObject p;
  if (kind == "mask") p = QJsonObject{{"only", QJsonValue()}, {"ignore", false}};
  else if (kind == "twist") p = QJsonObject{{"deg", 180}};
  else if (kind == "noise") p = QJsonObject{{"amp", r}, {"freq", 3}, {"sx", 1}, {"sy", 1}, {"plane", false}, {"rot0", 0}, {"rot1", 0}, {"rot2", 0}, {"seed", 1}, {"ampRef", r}, {"pure", 0}};
  else if (kind == "curl") p = QJsonObject{{"r0", r * 1.5}, {"r1", r * 1.5}, {"r2", r * 1.5}, {"r3", r * 1.5}, {"radRef", r}, {"d0", 1.5}, {"d1", 1.5}, {"d2", 1.5}, {"d3", 1.5},
                                           {"thick", 1}, {"spin", 0}, {"follow", false}, {"phase", 0}, {"smooth", 0}, {"shrink", 0}};
  int num_ = edit::nextNumber(sc, ItemType::XMod);
  QString base = t->name; { auto mm = QRegularExpression("^Modifier \\d+ \\((.*)\\)$").match(base); if (mm.hasMatch()) base = mm.captured(1); }
  int parent = t->parent;
  Item& x = sc.addItem(ItemType::XMod, parent, QString("%1 %2 (%3)").arg(xkindName(kind)).arg(num_).arg(base)); int id = x.id;
  QJsonArray mask; if (painted(kind)) for (int i = 0; i < MASK_N; i++) mask.append(0);
  x.raw = QJsonObject{{"tube", tubeId}, {"kind", kind}, {"p", p}, {"mask", painted(kind) ? QJsonValue(mask) : QJsonValue()}};
  // (after the modifier and its other extras)
  std::vector<int>& arr = parent >= 0 && sc.item(parent) ? sc.item(parent)->children : sc.roots;
  arr.erase(std::remove(arr.begin(), arr.end(), id), arr.end());
  int at = -1; std::vector<int> after{tubeId}; for (int e : extrasOf(sc, tubeId)) if (e != id) after.push_back(e);
  for (int a : after) { auto f = std::find(arr.begin(), arr.end(), a); if (f != arr.end()) at = std::max(at, int(f - arr.begin())); }
  arr.insert(at >= 0 ? arr.begin() + at + 1 : arr.end(), id);
  return id;
}

QString addExtra(Scene& sc, const CurveCache& cc, Selection& sel, const QString& kind) {
  auto ts = modTubes(sc, sel);
  if (ts.empty()) return QObject::tr("!Select a profile modifier (or its curve) — apply a profile in the Profile tab first");
  std::vector<int> made; for (int t : ts) made.push_back(newExtra(sc, cc, t, kind));
  sel.anchors.clear(); sel.items = made;
  if (kind == "mask") return QObject::tr("Mask added — paint it with the mask brush (Ctrl — erase, Shift — smooth)");
  if (kind == "thick") return QObject::tr("Thickness added — set the intensity, then paint (Ctrl — inverted, Ctrl+Alt — erase, Shift — smooth)");
  if (kind == "twist") return QObject::tr("Twist added — paint it: ↻ clockwise, Ctrl — ↺ anticlockwise, Ctrl+Alt — erase, Shift — smooth");
  return QObject::tr("%1 added — its settings are in the properties panel").arg(xkindName(kind));
}

}  // namespace hm::tube
