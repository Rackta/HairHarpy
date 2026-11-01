#include "scene/Rib.h"
#include "scene/Edit.h"
#include <QJsonArray>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <functional>

namespace hm::rib {

namespace {
const double PI = 3.14159265358979323846;
double jsRound(double x) { return std::floor(x + 0.5); }
QJsonObject R(const Item& c) { return c.raw.value("rib").toObject(); }
double num(const QJsonObject& o, const char* k, double def) { QJsonValue v = o.value(k); return v.isDouble() ? v.toDouble() : def; }
std::vector<double> maskOf(const QJsonValue& v) { std::vector<double> m; for (const QJsonValue& x : v.toArray()) m.push_back(x.toDouble()); return m; }
double maskAt(const std::vector<double>& m, double t) {
  if (m.size() < 2) return m.empty() ? 0 : m[0];
  double x = clamp(t, 0, 1) * double(m.size() - 1); size_t i = std::min(size_t(std::floor(x)), m.size() - 2);
  return m[i] + (m[i + 1] - m[i]) * (x - double(i));
}
Vec3 perp(const Vec3& v, const Vec3& t) { return v - t * v.dot(t); }
Vec3 rotateFromTo(const Vec3& a, const Vec3& b, const Vec3& v) {   // (THREE.Quaternion.setFromUnitVectors(a, b) applied to v)
  double r = a.dot(b) + 1; Quat q;
  if (r < 2.220446049250313e-16) { r = 0; q = std::abs(a.x) > std::abs(a.z) ? Quat{-a.y, a.x, 0, r} : Quat{0, -a.z, a.y, r}; }
  else { Vec3 c = a.cross(b); q = Quat{c.x, c.y, c.z, r}; }
  return q.normalized().rotate(v);
}
double ribFade(double s, double f) { if (!(f > 0) || s <= 1 - f) return 1; double u = clamp((1 - s) / f, 0, 1); return u * u * (3 - 2 * u); }
std::vector<int> curvesWith(const Scene& sc, int a) { std::vector<int> o; for (const Item* c : sc.curves()) if (std::find(c->anchors.begin(), c->anchors.end(), a) != c->anchors.end()) o.push_back(c->id); return o; }
const CurveSampling* smp(const CurveCache& cc, int id) { auto it = cc.find(id); return it == cc.end() || it->second.p.size() < 2 || it->second.arc.size() != it->second.p.size() ? nullptr : &it->second; }
std::optional<Vec3> pointOnCurve(const CurveSampling* c, double t) {
  if (!c) return std::nullopt; const auto& L = c->arc; double d = clamp(t, 0, 1) * L.back();
  size_t i = 0; while (i + 2 < L.size() && L[i + 1] < d) i++; double s = L[i + 1] - L[i];
  return c->p[i].lerp(c->p[i + 1], s > 0 ? clamp((d - L[i]) / s, 0, 1) : 0);
}
}  // namespace

bool isRib(const Item& c) { return c.raw.value("rib").isObject(); }
std::vector<int> ribEnds(const Scene& sc, const Item& c) {
  QJsonArray e = R(c).value("ends").toArray(); if (e.size() < 2) return {};
  std::vector<int> l; for (const QJsonValue& v : e) { int a = v.toInt(-1); bool found = false; for (const Item* x : sc.curves()) if (std::find(x->anchors.begin(), x->anchors.end(), a) != x->anchors.end()) found = true; if (!found) return {}; l.push_back(a); }
  return l;
}
bool isVolume(const Scene& sc, const Item& c) { return ribEnds(sc, c).size() > 2; }
int ribHost(const Scene& sc, int a) { for (int c : curvesWith(sc, a)) { const Item* k = sc.item(c); if (k->ring < 0 && k->conn.isEmpty() && !k->closed && !k->junc && !isRib(*k)) return c; } return -1; }
std::optional<Vec3> trunkDir(const Scene& sc, const std::vector<int>& pair) {
  Vec3 d;
  for (int x : pair) {
    int h = ribHost(sc, x); if (h < 0) continue; const auto& A = sc.item(h)->anchors; int i = int(std::find(A.begin(), A.end(), x) - A.begin());
    std::optional<Vec3> v; const Vec3& xp = sc.anchor(x)->pos;
    if (i > 0) v = xp - sc.anchor(A[size_t(i - 1)])->pos; else if (A.size() > 1) v = xp - sc.anchor(A[1])->pos;
    if (v && v->lengthSq() > 1e-12) d += v->normalized();
  }
  if (d.lengthSq() > 1e-8) return d.normalized();
  return std::nullopt;
}
Root ribRoot(const Scene& sc, const std::vector<int>& ends, const std::optional<Vec3>& T0) {
  Root r; for (int a : ends) r.mid += sc.anchor(a)->pos; r.mid = r.mid / double(ends.size());
  Vec3 A0 = sc.anchor(ends[1])->pos - sc.anchor(ends[0])->pos;
  if (ends.size() > 2) {
    double bd = -1; for (int a : ends) for (int b : ends) { double d = (sc.anchor(a)->pos - sc.anchor(b)->pos).lengthSq(); if (d > bd) { bd = d; A0 = sc.anchor(b)->pos - sc.anchor(a)->pos; } }
    if (T0) { Vec3 q = A0 - *T0 * A0.dot(*T0); if (q.lengthSq() > 1e-12) A0 = q; }
  }
  r.A0 = A0.normalized(); r.B0 = T0 ? T0->cross(r.A0).normalized() : Vec3();
  for (int a : ends) { Vec3 d = sc.anchor(a)->pos - r.mid; r.prof.push_back({d.dot(r.A0), d.dot(r.B0), T0 ? d.dot(*T0) : 0}); }
  if (ends.size() == 2) r.w0 = sc.anchor(ends[0])->pos.distanceTo(sc.anchor(ends[1])->pos) / 2;
  else { r.w0 = 1e-6; for (auto& q : r.prof) r.w0 = std::max(r.w0, std::hypot(q.x, q.y)); }
  return r;
}
std::vector<int> ringOrder(const Scene& sc, const std::vector<int>& list, const std::optional<Vec3>& T0) {
  Root r = ribRoot(sc, list, T0); std::vector<std::pair<double, int>> o;
  for (size_t i = 0; i < list.size(); i++) o.push_back({std::atan2(r.prof[i].y, r.prof[i].x), list[i]});
  std::stable_sort(o.begin(), o.end(), [](auto& a, auto& b) { return a.first < b.first; });
  std::vector<int> out; for (auto& x : o) out.push_back(x.second); return out;
}

std::optional<Shape> ribShape(const Scene& sc, const CurveCache& cc, const Item& c, const Vec3& camDir, double scale) {
  const CurveSampling* S = smp(cc, c.id); if (!S) return std::nullopt;
  const QJsonObject rib = R(c); const QJsonObject K = rib.value("k").toObject();
  const int N = int(S->p.size()), n = int(c.anchors.size());
  // width / rotation at every anchor (an anchor without its own takes them from its neighbours)
  std::vector<int> known; for (int i = 0; i < n; i++) if (K.contains(QString::number(c.anchors[size_t(i)]))) known.push_back(i);
  auto at = [&](int i, const char* key, double def) {
    if (known.empty()) return def;
    int lo = -1, hi = -1; for (int j : known) { if (j <= i) lo = j; if (j >= i && hi < 0) hi = j; }
    auto v = [&](int j) { return K.value(QString::number(c.anchors[size_t(j)])).toObject().value(key).toDouble(0); };
    if (lo < 0) return v(hi); if (hi < 0 || hi == lo) return v(lo);
    return v(lo) + (v(hi) - v(lo)) * (i - lo) / (hi - lo);
  };
  std::vector<double> AW, AR; for (int i = 0; i < n; i++) { AW.push_back(at(i, "w", scale * 0.02)); AR.push_back(at(i, "r", 0)); }
  auto ends = ribEnds(sc, c);
  const auto& P = S->p; std::vector<Vec3> T(static_cast<size_t>(N));
  for (int i = 0; i < N; i++) { Vec3 t = P[size_t(std::min(i + 1, N - 1))] - P[size_t(std::max(i - 1, 0))]; T[size_t(i)] = t.lengthSq() > 1e-20 ? t.normalized() : Vec3(0, -1, 0); }
  Shape sh; if (!ends.empty()) { sh.root = ribRoot(sc, ends, T[0]); AW[0] = sh.root->w0; AR[0] = 0; }
  const auto& arc = S->arc; double tot = arc.back() ? arc.back() : 1, per = double(N - 1) / std::max(1, n - 1);
  std::vector<double> sA; for (int k = 0; k < n; k++) sA.push_back(arc[size_t(std::min(N - 1, int(jsRound(k * per))))]);
  // between the anchors: a smooth monotone cubic over the arc length; Profile smoothing — a Gaussian along the lock
  auto curve = [&](const std::vector<double>& V) {
    const int m = int(V.size()); std::vector<double> out(static_cast<size_t>(N));
    if (m < 2) { std::fill(out.begin(), out.end(), m ? V[0] : 0); return out; }
    std::vector<double> d, t;
    for (int k = 0; k < m - 1; k++) d.push_back((V[size_t(k + 1)] - V[size_t(k)]) / std::max(sA[size_t(k + 1)] - sA[size_t(k)], 1e-9));
    for (int k = 0; k < m; k++) t.push_back(k == 0 ? d[0] : k == m - 1 ? d[size_t(m - 2)] : d[size_t(k - 1)] * d[size_t(k)] <= 0 ? 0 : (d[size_t(k - 1)] + d[size_t(k)]) / 2);
    for (int k = 0; k < m - 1; k++) { if (d[size_t(k)] == 0) { t[size_t(k)] = t[size_t(k + 1)] = 0; continue; } double a = t[size_t(k)] / d[size_t(k)], b = t[size_t(k + 1)] / d[size_t(k)], h = a * a + b * b; if (h > 9) { double s = 3 / std::sqrt(h); t[size_t(k)] = s * a * d[size_t(k)]; t[size_t(k + 1)] = s * b * d[size_t(k)]; } }
    for (int i = 0; i < N; i++) {
      double x = arc[size_t(i)]; int k = 0; while (k < m - 2 && x > sA[size_t(k + 1)]) k++;
      double hh = std::max(sA[size_t(k + 1)] - sA[size_t(k)], 1e-9), u = clamp((x - sA[size_t(k)]) / hh, 0, 1), u2 = u * u, u3 = u2 * u;
      out[size_t(i)] = (2 * u3 - 3 * u2 + 1) * V[size_t(k)] + (u3 - 2 * u2 + u) * hh * t[size_t(k)] + (-2 * u3 + 3 * u2) * V[size_t(k + 1)] + (u3 - u2) * hh * t[size_t(k + 1)];
    }
    return out;
  };
  auto w = curve(AW), r = curve(AR);
  double sg = num(rib, "smooth", 0.4) * 0.12 * tot;
  if (sg > tot * 1e-4) {
    auto gauss = [&](const std::vector<double>& V) {
      double ds = tot / (N - 1); int ext = int(std::ceil(3 * sg / ds)); std::vector<double> o(static_cast<size_t>(N));
      for (int i = 0; i < N; i++) {
        double s = 0, ws = 0, x = arc[size_t(i)];
        for (int j = 0; j < N; j++) { double q = std::exp(-std::pow(arc[size_t(j)] - x, 2) / (2 * sg * sg)); s += q * V[size_t(j)]; ws += q; }
        for (int e = 1; e <= ext; e++) { double q0 = std::exp(-std::pow(x + e * ds, 2) / (2 * sg * sg)), q1 = std::exp(-std::pow(tot + e * ds - x, 2) / (2 * sg * sg)); s += q0 * V[0] + q1 * V[size_t(N - 1)]; ws += q0 + q1; }
        o[size_t(i)] = s / ws;
      }
      return o;
    };
    double w0 = w[0]; w = gauss(w); r = gauss(r);
    if (!ends.empty()) {                                   // (the root stays on its anchors)
      double dw = w0 - w[0], r0 = r[0];
      for (int i = 0; i < N; i++) { double f = std::exp(-std::pow(arc[size_t(i)] / (sg * 1.5), 2)); w[size_t(i)] += dw * f; r[size_t(i)] -= r0 * f; }
    }
  }
  double tip = num(rib, "tip", 0.25);
  if (tip > 0) for (int i = 0; i < N; i++) { double t = arc[size_t(i)] / tot; if (t <= 1 - tip) continue; double u = (1 - t) / tip; w[size_t(i)] *= u * (2 - u); }
  Vec3 A0;
  if (!ends.empty()) A0 = sh.root->A0;
  else { QJsonArray vv = rib.value("view").toArray(); Vec3 vw = vv.size() == 3 ? Vec3(vv[0].toDouble(), vv[1].toDouble(), vv[2].toDouble()) : camDir;
    A0 = T[0].cross(vw); if (A0.lengthSq() < 1e-10) A0 = perp(Vec3(0, 1, 0), T[0]); A0 = A0.normalized(); }
  Vec3 A = perp(A0, T[0]); if (A.lengthSq() < 1e-12) A = perp(Vec3(1, 0, 0), T[0]); A = A.normalized();
  for (int i = 0; i < N; i++) {
    if (i) { A = rotateFromTo(T[size_t(i - 1)], T[size_t(i)], A); A = perp(A, T[size_t(i)]); if (A.lengthSq() < 1e-12) A = perp(Vec3(0, 1, 0), T[size_t(i)]); A = A.normalized(); }
    Vec3 a = Quat::fromAxisAngle(T[size_t(i)], r[size_t(i)] * PI / 180).rotate(A);
    if (i == 0 && !ends.empty()) a = A0;                   // (the root lies exactly on the anchors)
    sh.F.push_back({P[size_t(i)], a, T[size_t(i)], std::max(0.0, w[size_t(i)]), arc[size_t(i)] / tot});
  }
  // a volume branch: ring j of every sample — the root anchors' places in the root frame, carried along and scaled
  if (sh.root && sh.root->prof.size() >= 3) {
    for (auto& f : sh.F) { Vec3 B = f.T.cross(f.A).normalized(); double k = f.w / sh.root->w0, fade = std::exp(-std::pow(f.s / 0.1, 2));
      std::vector<Vec3> ring; for (auto& q : sh.root->prof) ring.push_back(f.p + f.A * (q.x * k) + B * (q.y * k) + f.T * (q.t * fade)); sh.rings.push_back(ring); }
    sh.tube = true; return sh;
  }
  // extruded: [L, R, R', L'] — the ribbon pushed out across itself, the edges offset, the top face glued
  auto X = maskOf(rib.value("ext"));
  bool anyExt = false; for (double v : X) if (v > 0.002) anyExt = true;
  if (anyExt) {
    auto sm = [](std::vector<double> a) { if (a.empty()) return a; for (int p = 0; p < 6; p++) { std::vector<double> b(a.size()); for (size_t i = 0; i < a.size(); i++) b[i] = (a[i ? i - 1 : 0] + 2 * a[i] + a[std::min(i + 1, a.size() - 1)]) / 4; a = b; } return a; };
    auto off = maskOf(rib.value("off"));
    auto SX = sm(X), SL = sm(rib.value("offL").isArray() ? maskOf(rib.value("offL")) : off), SR = sm(rib.value("offR").isArray() ? maskOf(rib.value("offR")) : off), SG = sm(maskOf(rib.value("glue")));
    double inv = rib.value("extInv").toBool() ? -1 : 1, H = num(rib, "extH", 1);
    for (auto& f : sh.F) {
      double ee = clamp(maskAt(SX, f.s), 0, 1) * ribFade(f.s, num(rib, "extFade", 0)), fo = ribFade(f.s, num(rib, "offFade", 0)), gg = clamp(maskAt(SG, f.s), 0, 1);
      double oL = clamp(maskAt(SL, f.s), -1, 1) * fo, oR = clamp(maskAt(SR, f.s), -1, 1) * fo;
      Vec3 B = f.T.cross(f.A).normalized(), top = f.p + B * (inv * ee * H * f.w);
      double xL = -clamp(f.w * (1 - oL), 0, 2 * f.w), xR = clamp(f.w * (1 - oR), 0, 2 * f.w), xm = (xL + xR) / 2;
      sh.e.push_back(ee); sh.g.push_back(gg);
      sh.rings.push_back({f.p - f.A * f.w, f.p + f.A * f.w, top + f.A * (xR + (xm - xR) * gg), top + f.A * (xL + (xm - xL) * gg)});
    }
    sh.tube = true; sh.extruded = true; return sh;
  }
  for (auto& f : sh.F) sh.rings.push_back({f.p - f.A * f.w, f.p + f.A * f.w});
  return sh;
}

std::vector<std::array<float, 3>> ribColors(const Item& c, const Shape& s, const QString& kind) {
  auto lin = [](double v) { return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)); };
  auto col = [&](unsigned hex) { return std::array<float, 3>{lin(((hex >> 16) & 255) / 255.0), lin(((hex >> 8) & 255) / 255.0), lin((hex & 255) / 255.0)}; };
  const auto base = col(0xb04fc0); const QJsonObject rib = R(c);
  const size_t M = s.rings.empty() ? 0 : s.rings[0].size(); std::vector<std::array<float, 3>> out;
  for (auto& f : s.F) {
    auto t = base;
    if (!kind.isEmpty()) {
      double v;
      if (kind == "off") { double vl = maskAt(maskOf(rib.value("offL")), f.s), vr = maskAt(maskOf(rib.value("offR")), f.s); v = std::abs(vl) > std::abs(vr) ? vl : vr; }
      else v = maskAt(maskOf(rib.value(kind)), f.s);
      auto to = col(kind == "off" && v < 0 ? 0x33a0ff : kind == "ext" ? 0xff9a30 : kind == "off" ? 0x33a0ff : 0x4de673);
      double k = clamp(std::abs(v), 0, 1); for (int q = 0; q < 3; q++) t[size_t(q)] = float(t[size_t(q)] + (to[size_t(q)] - t[size_t(q)]) * k);
    }
    for (size_t j = 0; j < M; j++) out.push_back(t);
  }
  return out;
}

bool followEnds(Scene& sc, Item& c) {
  auto ends = ribEnds(sc, c); if (ends.empty() || c.anchors.empty()) return false;
  Vec3 mid; for (int a : ends) mid += sc.anchor(a)->pos; mid = mid / double(ends.size());
  Anchor* a0 = sc.anchor(c.anchors[0]); if ((mid - a0->pos).lengthSq() <= 1e-12) return false;
  a0->pos = mid; return true;
}

bool paintRib(Scene& sc, const CurveCache& cc, const std::vector<int>& locks, const std::vector<Shape*>& shapes, const QString& kind, const QString& op,
              double amt, double strength, double intensity, const std::function<double(const Vec3&)>& weightAt) {
  double lo = kind == "off" ? -1 : 0, target = op == "erase" ? 0 : (op == "invert" ? -1 : 1) * (kind == "off" ? intensity : strength);
  bool changed = false;
  for (size_t li = 0; li < locks.size(); li++) {
    Item* c = sc.item(locks[li]); const CurveSampling* S = smp(cc, c->id); if (!c || !S) continue;
    QJsonObject rib = R(*c);
    if (rib.value("off").isArray()) { rib.insert("offL", rib.value("off")); rib.insert("offR", rib.value("off")); rib.remove("off"); }
    std::vector<std::pair<QString, int>> lanes = kind == "off" ? std::vector<std::pair<QString, int>>{{"offL", 3}, {"offR", 2}} : std::vector<std::pair<QString, int>>{{kind, -1}};
    const Shape* sh = li < shapes.size() ? shapes[li] : nullptr;
    for (auto& [key, col] : lanes) {
      std::vector<double> W = maskOf(rib.value(key)); if (int(W.size()) != MASK_N) W.assign(MASK_N, 0);
      const auto src = W; bool ch = false;
      for (int k = 0; k < MASK_N; k++) {
        auto P = pointOnCurve(S, double(k) / (MASK_N - 1)); if (!P) continue;
        if (col >= 0 && sh && sh->extruded && !sh->rings.empty()) P = sh->rings[size_t(jsRound(double(k) / (MASK_N - 1) * (sh->rings.size() - 1)))][size_t(col)];
        double bw = weightAt(*P); if (!(bw > 0)) continue;
        double old = W[size_t(k)];
        if (op == "smooth") { double sum = 0; int nn = 0; for (int j = k - 3; j <= k + 3; j++) if (j >= 0 && j < MASK_N) { sum += src[size_t(j)]; nn++; } W[size_t(k)] = old + (sum / nn - old) * bw * 0.5; }
        else if (op == "erase") W[size_t(k)] = old > 0 ? std::max(0.0, old - bw * amt) : std::min(0.0, old + bw * amt);
        else W[size_t(k)] = clamp(old + target * bw * amt, lo, 1);
        if (W[size_t(k)] != old) ch = true;
      }
      if (ch) { QJsonArray a; for (double v : W) a.append(v); rib.insert(key, a); changed = true; }
    }
    c->raw.insert("rib", rib);
  }
  return changed;
}

namespace {
void placeAfter(Scene& sc, int id, int after, int k) {
  Item* it = sc.item(id); auto& arr = it->parent >= 0 && sc.item(it->parent) ? sc.item(it->parent)->children : sc.roots;
  arr.erase(std::remove(arr.begin(), arr.end(), id), arr.end());
  auto at = std::find(arr.begin(), arr.end(), after); size_t pos = at == arr.end() ? arr.size() : size_t(at - arr.begin()) + 1 + size_t(k);
  arr.insert(arr.begin() + std::min(pos, arr.size()), id);
}
Item& newCurve(Scene& sc, int parent, const QString& name) { Item& c = edit::makeUserCurve(sc, parent); c.name = name; return c; }
std::vector<int> withTrunk(const Scene& sc, int x, std::vector<int> an) {   // (an edge curve goes on back along its anchor's curve to its root)
  int h = ribHost(sc, x); if (h < 0) return an; const auto& A = sc.item(h)->anchors; auto it = std::find(A.begin(), A.end(), x);
  if (it == A.end() || it == A.begin()) return an;
  std::vector<int> o(A.begin(), it); o.insert(o.end(), an.begin(), an.end()); return o;
}
void wallFlat(Scene& sc, int a, int b) { int s = edit::makeSurface(sc, a, b); if (s >= 0) { QJsonObject p = sc.item(s)->raw.value("p").toObject(); p.insert("bulge", 0); sc.item(s)->raw.insert("p", p); } }
void dropCurve(Scene& sc, const CurveCache& cc, int id) { Selection d; d.items = {id}; sc.reindex(); edit::deleteItems(sc, cc, d); }
}  // namespace

// ✔ Convert: a flat lock → two curves along the edges and a flat wall; extruded → four curves and four walls; a volume
// branch → a curve from every anchor it started on, walls round them. The middle curve goes.
QString convert(Scene& sc, const CurveCache& cc, Selection& sel, int cid, const Vec3& camDir, double scale) {
  Item* c = sc.item(cid); if (!c || !isRib(*c)) return "!";
  auto sh = ribShape(sc, cc, *c, camDir, scale); if (!sh || sh->F.size() < 2) return QObject::tr("!The lock has no shape yet");
  const QString cname = c->name; const int parent = c->parent;
  const int n = int(c->anchors.size()), N = int(sh->F.size()); double per = double(N - 1) / std::max(1, n - 1);
  std::vector<int> idx; for (int k = 0; k < n; k++) { idx.push_back(int(jsRound(k * per))); if (n < 6 && k < n - 1) idx.push_back(int(jsRound((k + 0.5) * per))); }
  auto ends = ribEnds(sc, *c);
  std::vector<int> made; QString msg;
  if (sh->tube && !sh->extruded) {                           // a volume branch
    int tip = sh->F.back().w <= scale * 1e-4 ? sc.addAnchor(sh->F.back().p).id : -1;
    for (size_t j = 0; j < ends.size(); j++) {
      Item& cv = newCurve(sc, parent, QString("%1 %2").arg(cname).arg(j + 1)); int id = cv.id;
      std::vector<int> an; for (int i : idx) an.push_back(sc.addAnchor(sh->rings[size_t(i)][j]).id);
      an[0] = ends[j]; if (tip >= 0) an.back() = tip;
      sc.item(id)->anchors = withTrunk(sc, ends[j], an); placeAfter(sc, id, cid, int(j)); made.push_back(id);
    }
    sc.reindex();
    for (size_t j = 0; j < made.size(); j++) wallFlat(sc, made[j], made[(j + 1) % made.size()]);
    msg = QObject::tr("%1: %2 curves and %2 walls round the branch (joined to its anchors)").arg(cname).arg(made.size());
  } else if (sh->extruded) {
    const double eps = scale * 1e-4; const int col[4] = {0, 1, 3, 2}; const char* nm[4] = {"L", "R", "TL", "TR"};
    std::vector<std::array<int, 4>> A;
    for (int i : idx) {
      std::array<Vec3, 4> q; for (int k = 0; k < 4; k++) q[size_t(k)] = sh->rings[size_t(i)][size_t(col[k])];
      if (sh->g[size_t(i)] >= 0.95) { Vec3 mid = q[2].lerp(q[3], 0.5); q[2] = q[3] = mid; }   // a glued top face: one ridge
      std::array<int, 4> an; for (int k = 0; k < 4; k++) an[size_t(k)] = sc.addAnchor(q[size_t(k)]).id;
      for (int k = 1; k < 4; k++) for (int j = 0; j < k; j++) if (sc.anchor(an[size_t(j)])->pos.distanceTo(sc.anchor(an[size_t(k)])->pos) <= eps) { an[size_t(k)] = an[size_t(j)]; break; }
      A.push_back(an);
    }
    if (!ends.empty()) { int o0 = A[0][0], o1 = A[0][1]; for (int& a : A[0]) a = a == o0 ? ends[0] : a == o1 ? ends[1] : a; }
    for (int j = 0; j < 4; j++) {
      Item& cv = newCurve(sc, parent, QString("%1 %2").arg(cname, nm[j])); int id = cv.id;
      std::vector<int> an; for (auto& a : A) an.push_back(a[size_t(j)]);
      if (!ends.empty() && j < 2) an = withTrunk(sc, ends[size_t(j)], an);
      sc.item(id)->anchors = an; placeAfter(sc, id, cid, j); made.push_back(id);
    }
    sc.reindex();
    for (auto [a, b] : {std::pair<int, int>{0, 1}, {0, 2}, {1, 3}, {2, 3}}) wallFlat(sc, made[size_t(a)], made[size_t(b)]);
    msg = QObject::tr("%1: 4 curves and 4 walls (extruded; shared anchors where the edges meet)").arg(cname);
  } else {
    int L = newCurve(sc, parent, cname + " L").id, Rr = newCurve(sc, parent, cname + " R").id;
    for (auto [cv, s] : {std::pair<int, double>{L, -1}, {Rr, 1}}) { std::vector<int> an; for (int i : idx) an.push_back(sc.addAnchor(sh->F[size_t(i)].p + sh->F[size_t(i)].A * (s * sh->F[size_t(i)].w)).id); sc.item(cv)->anchors = an; }
    if (sh->F.back().w <= scale * 1e-4) { int t = sc.addAnchor(sh->F.back().p).id; sc.item(L)->anchors.back() = t; sc.item(Rr)->anchors.back() = t; }
    // started from two anchors: each edge curve starts there and goes on back along that anchor's curve (a common trunk)
    if (!ends.empty()) for (auto [cv, x] : {std::pair<int, int>{L, ends[0]}, {Rr, ends[1]}}) { auto an = sc.item(cv)->anchors; an[0] = x; sc.item(cv)->anchors = withTrunk(sc, x, an); }
    placeAfter(sc, L, cid, 0); placeAfter(sc, Rr, cid, 1); made = {L, Rr};
    sc.reindex();
    wallFlat(sc, L, Rr);
    msg = QObject::tr("%1: 2 curves and a wall%2").arg(cname).arg(ends.empty() ? QString() : QObject::tr(" (joined to the two anchors)"));
  }
  dropCurve(sc, cc, cid);
  sel.anchors.clear(); sel.items = made;
  sc.reindex();
  return msg;
}

}  // namespace hm::rib
