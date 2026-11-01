#include "scene/Crown.h"
#include "scene/Tubes.h"
#include "scene/Edit.h"
#include <QJsonArray>
#include <QObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <set>

namespace hm::crown {

namespace {

const int WHORL_IN = 2;                                     // at most this many crown loops reach one parting vertex
double jsRound(double x) { return std::floor(x + 0.5); }
double ease(double s) { return s * s * (3 - 2 * s); }
template <class T> bool has(const std::vector<T>& v, const T& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

const CurveSampling* smp(const CurveCache& cc, int id) { auto it = cc.find(id); return it == cc.end() || it->second.p.size() < 2 || it->second.arc.size() != it->second.p.size() ? nullptr : &it->second; }
std::vector<Vec3> resample(const std::vector<Vec3>& pts, int n) {
  std::vector<double> L{0}; for (size_t i = 1; i < pts.size(); i++) L.push_back(L.back() + pts[i].distanceTo(pts[i - 1]));
  double tot = L.back(); std::vector<Vec3> out; size_t j = 0;
  for (int i = 0; i < n; i++) {
    double d = tot * i / (n - 1);
    while (j + 2 < pts.size() && L[j + 1] < d) j++;
    double sl = L[j + 1] - L[j]; out.push_back(pts[j].lerp(pts[j + 1], sl > 0 ? clamp((d - L[j]) / sl, 0, 1) : 0));
  }
  return out;
}
double polyLen(const std::vector<Vec3>& P) { double l = 0; for (size_t i = 1; i < P.size(); i++) l += P[i].distanceTo(P[i - 1]); return l; }
double segPos(double s, const QJsonObject& p, int segs) {
  if (segs < 2 || s <= 0 || s >= 1) return s;
  double sp = segs >= 3 ? clamp(p.value("segSpread").toDouble(0), -1, 1) : 0, off = clamp(p.value("segOff").toDouble(0), -1, 1);
  if (sp != 0) { double u = s * 2 - 1, g = std::pow(2, -sp * 1.5); s = (((u > 0) - (u < 0)) * std::pow(std::abs(u), g) + 1) / 2; }
  if (off != 0) { double m = 0.5 + 0.4 * off, k = m / (1 - m); s = k * s / (1 + (k - 1) * s); }
  return s;
}
Vec3 faceNormal(const std::vector<Vec3>& pos, const std::vector<int>& f) {   // (Newell)
  Vec3 n;
  for (size_t i = 0; i < f.size(); i++) { const Vec3 &a = pos[size_t(f[i])], &b = pos[size_t(f[(i + 1) % f.size()])]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
  return n;
}
Vec3 faceCenter(const std::vector<Vec3>& pos, const std::vector<int>& f) { Vec3 c; for (int i : f) c += pos[size_t(i)]; return c / double(f.size()); }
QJsonObject attOf(const Anchor* a) { return a ? a->raw.value("at").toObject() : QJsonObject(); }
std::vector<int> curvesWith(const Scene& sc, int a) { std::vector<int> o; for (const Item* c : sc.curves()) if (has(c->anchors, a)) o.push_back(c->id); return o; }

// the reference (a Maya scene of a crown): whorl first; lower connector front-left → back → front-right
const double TPART[][3] = {{-0.292, 152.001, -7.159}, {-0.214, 152.122, -5.566}, {-0.178, 152.0, -3.971}, {-0.125, 151.931, -2.401}, {-0.124, 151.477, -0.875}};
const std::vector<std::vector<Vec3>>& TRAD() {
  static const std::vector<std::vector<Vec3>> R{
    {{-5.735, 149.179, -1.348}, {-3.175, 151.339, -0.846}, {-0.124, 151.477, -0.875}},
    {{-6.453, 149.263, -3.354}, {-4.716, 151.037, -3.06}, {-2.526, 152.181, -2.831}, {-0.104, 152.044, -2.731}},
    {{-6.471, 149.467, -7.359}, {-4.828, 151.209, -6.546}, {-2.642, 152.281, -5.862}, {-0.214, 152.122, -5.546}},
    {{-5.019, 149.634, -9.876}, {-3.809, 151.168, -8.771}, {-2.046, 152.068, -7.713}, {-0.219, 152.102, -6.412}},
    {{-0.107, 149.854, -11.674}, {-0.096, 151.022, -10.384}, {-0.114, 151.859, -8.865}, {-0.292, 152.001, -7.159}},
    {{3.096, 149.895, -10.844}, {2.21, 151.144, -9.463}, {0.999, 152.017, -8.034}, {-0.219, 152.102, -6.412}},
    {{6.348, 149.778, -6.906}, {4.503, 151.371, -6.274}, {2.234, 152.354, -5.786}, {-0.214, 152.122, -5.546}},
    {{6.358, 149.591, -3.248}, {4.493, 151.161, -2.743}, {2.264, 152.222, -2.39}, {-0.144, 151.826, -2.094}},
    {{5.768, 149.475, -1.29}, {3.2, 151.35, -0.85}, {-0.124, 151.477, -0.875}}};
  return R;
}

// the Bangs preset (tools/bangs_template.py, from the bangs model: a quarter of a sphere): its parting a point — the
// pole on the top of the head (a short line across there); the curves fan out of it — down the
// sphere's back edges, the meridians between them evenly down to the lower edge (at the brows), and across the
// chamfer (a wall between the sphere's back edge and the back arch) to the arch. The lower connector: the back arch from
// its top down one side, the lower edge, up the other side to its top again — the arch unbroken over the pole
const double BPART[][3] = {{0.271, 152.335, -6.068}, {-0.271, 152.335, -6.068}};
const int BCORN[] = {9, 27};
const std::vector<Vec3>& BLOW() {
  static const std::vector<Vec3> L{{0.034, 152.250, -6.181}, {1.367, 152.082, -6.171}, {2.653, 151.609, -6.225}, {3.761, 150.922, -6.284}, {4.661, 150.093, -6.338}, {5.349, 149.111, -6.395}, {5.826, 147.975, -6.447}, {6.104, 146.747, -6.475}, {6.180, 145.457, -6.479}, {6.180, 144.469, -6.480}, {6.800, 144.469, -6.153}, {6.682, 144.478, -4.834}, {6.301, 144.481, -3.565}, {5.668, 144.480, -2.389}, {4.809, 144.477, -1.350}, {3.765, 144.474, -0.493}, {2.584, 144.476, 0.151}, {1.314, 144.481, 0.551}, {-0.000, 144.484, 0.686}, {-1.314, 144.481, 0.551}, {-2.584, 144.476, 0.151}, {-3.765, 144.474, -0.493}, {-4.809, 144.477, -1.350}, {-5.668, 144.480, -2.389}, {-6.301, 144.481, -3.565}, {-6.682, 144.478, -4.834}, {-6.800, 144.469, -6.153}, {-6.180, 144.469, -6.480}, {-6.180, 145.457, -6.479}, {-6.104, 146.747, -6.475}, {-5.826, 147.975, -6.447}, {-5.349, 149.111, -6.395}, {-4.661, 150.093, -6.337}, {-3.761, 150.922, -6.284}, {-2.653, 151.609, -6.225}, {-1.367, 152.082, -6.171}, {-0.034, 152.250, -6.181}};
  return L;
}
const std::vector<std::vector<Vec3>>& BRAD() {
  static const std::vector<std::vector<Vec3>> R{
    {{0.034, 152.250, -6.181}, {0.025, 152.271, -6.153}, {0.017, 152.293, -6.125}, {0.008, 152.314, -6.096}, {0.000, 152.335, -6.068}},
    {{5.349, 149.111, -6.395}, {4.445, 150.486, -6.253}, {3.176, 151.531, -6.149}, {1.644, 152.141, -6.092}, {0.000, 152.335, -6.068}},
    {{6.180, 144.469, -6.480}, {6.127, 147.359, -6.382}, {5.073, 150.024, -6.213}, {2.837, 151.807, -6.098}, {0.000, 152.335, -6.068}},
    {{6.800, 144.469, -6.153}, {6.580, 147.449, -6.149}, {5.348, 150.144, -6.078}, {2.945, 151.858, -6.056}, {0.000, 152.335, -6.068}},
    {{6.301, 144.481, -3.565}, {6.125, 147.477, -3.646}, {4.992, 150.200, -4.046}, {2.742, 151.907, -4.937}, {0.000, 152.335, -6.068}},
    {{4.809, 144.477, -1.350}, {4.660, 147.467, -1.482}, {3.784, 150.195, -2.269}, {2.090, 151.917, -3.974}, {0.000, 152.335, -6.068}},
    {{2.584, 144.476, 0.151}, {2.486, 147.475, 0.017}, {2.009, 150.199, -1.070}, {1.119, 151.909, -3.325}, {0.000, 152.335, -6.068}},
    {{0.000, 144.484, 0.686}, {-0.000, 147.494, 0.544}, {-0.000, 150.223, -0.645}, {0.000, 151.909, -3.097}, {0.000, 152.335, -6.068}},
    {{-2.584, 144.476, 0.151}, {-2.486, 147.475, 0.017}, {-2.009, 150.199, -1.070}, {-1.119, 151.909, -3.325}, {0.000, 152.335, -6.068}},
    {{-4.809, 144.477, -1.350}, {-4.660, 147.467, -1.482}, {-3.784, 150.194, -2.269}, {-2.090, 151.917, -3.974}, {0.000, 152.335, -6.068}},
    {{-6.301, 144.481, -3.565}, {-6.125, 147.477, -3.646}, {-4.992, 150.199, -4.046}, {-2.742, 151.907, -4.937}, {0.000, 152.335, -6.068}},
    {{-6.800, 144.469, -6.153}, {-6.580, 147.449, -6.149}, {-5.348, 150.144, -6.078}, {-2.945, 151.858, -6.057}, {0.000, 152.335, -6.068}},
    {{-6.180, 144.469, -6.480}, {-6.127, 147.359, -6.382}, {-5.074, 150.024, -6.213}, {-2.837, 151.807, -6.098}, {0.000, 152.335, -6.068}},
    {{-5.349, 149.111, -6.395}, {-4.445, 150.486, -6.253}, {-3.176, 151.531, -6.149}, {-1.644, 152.141, -6.092}, {0.000, 152.335, -6.068}},
    {{-0.034, 152.250, -6.181}, {-0.025, 152.271, -6.153}, {-0.017, 152.293, -6.125}, {-0.008, 152.314, -6.096}, {0.000, 152.335, -6.068}}};
  return R;
}

int partOf(const Item& k) { return k.raw.value("part").toInt(-1); }
int lowOf(const Item& k) { return k.raw.value("low").toInt(-1); }
void sampleInto(const Scene& sc, CurveCache& cc, int id) { const Item* c = sc.item(id); if (c) cc[id] = sampleCurveFull(sc, *c); }
}  // namespace

std::vector<int> crowns(const Scene& sc) {
  std::vector<const Item*> v; for (auto& [id, it] : sc.items) if (it.type == ItemType::Crown) v.push_back(&it);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; });
  std::vector<int> o; for (auto* it : v) o.push_back(it->id); return o;
}

std::optional<Vec3> pointOnCurve(const CurveSampling* c, double t) {
  if (!c || c->p.size() < 2 || c->arc.size() != c->p.size()) return std::nullopt;
  const auto& L = c->arc; double d = clamp(t, 0, 1) * L.back();
  size_t i = 0; while (i + 2 < L.size() && L[i + 1] < d) i++;
  double s = L[i + 1] - L[i];
  return c->p[i].lerp(c->p[i + 1], s > 0 ? clamp((d - L[i]) / s, 0, 1) : 0);
}
double nearestT(const CurveSampling& c, const Vec3& p) {
  const auto& s = c.p; const auto& L = c.arc; if (L.size() != s.size() || s.size() < 2) return 0;
  double best = 0, bd = 1e300;
  for (size_t i = 0; i + 1 < s.size(); i++) {
    Vec3 ab = s[i + 1] - s[i]; double l2 = ab.lengthSq(), t = l2 > 0 ? clamp((p - s[i]).dot(ab) / l2, 0, 1) : 0;
    double d = (s[i] + ab * t - p).lengthSq();
    if (d < bd) { bd = d; best = (L[i] + t * (L[i + 1] - L[i])) / (L.back() ? L.back() : 1); }
  }
  return best;
}
bool onConnector(const Scene& sc, int a, const Item& h) {
  QJsonObject at = attOf(sc.anchor(a));
  return (!at.isEmpty() && at.value("c").toInt() == h.id) || has(h.anchors, a);
}
double connT(const Scene& sc, const CurveCache& cc, const Item& h, int a) {
  QJsonObject at = attOf(sc.anchor(a));
  if (!at.isEmpty() && at.value("c").toInt() == h.id) return at.value("t").toDouble();
  int i = -1; for (size_t k = 0; k < h.anchors.size(); k++) if (h.anchors[k] == a) { i = int(k); break; }
  if (i < 0) return 0;
  const CurveSampling* s = smp(cc, h.id);
  if (s) return s->arc[std::min<size_t>(size_t(i) * 24, s->arc.size() - 1)] / (s->arc.back() ? s->arc.back() : 1);
  return double(i) / std::max<size_t>(h.anchors.size() - 1, 1);
}

namespace {
std::vector<Rad> crownCurvesCC(const Scene& sc, const CurveCache& cc, const Item& k) {
  const Item *part = sc.item(partOf(k)), *low = sc.item(lowOf(k)); std::vector<Rad> out;
  if (!part || !low) return out;
  for (const Item* c : sc.curves()) {
    if (c->ring >= 0 || !c->conn.isEmpty() || c->anchors.size() < 2) continue;
    int a0 = c->anchors.front(), a1 = c->anchors.back();
    if (onConnector(sc, a0, *low) && onConnector(sc, a1, *part)) out.push_back({c->id, connT(sc, cc, *low, a0), connT(sc, cc, *part, a1), false});
    else if (onConnector(sc, a1, *low) && onConnector(sc, a0, *part)) out.push_back({c->id, connT(sc, cc, *low, a1), connT(sc, cc, *part, a0), true});
  }
  std::stable_sort(out.begin(), out.end(), [](const Rad& x, const Rad& y) { return x.u < y.u; });
  return out;
}
}  // namespace

std::vector<Rad> crownCurves(const Scene& sc, const CurveCache& cc, const Item& k) { return crownCurvesCC(sc, cc, k); }

// ---------- the crown's mesh ----------
std::optional<CrownMesh> crownData(const Scene& sc, const CurveCache& cc, const Item& k) {
  const Item *part = sc.item(partOf(k)), *low = sc.item(lowOf(k));
  if (!part || !low) return std::nullopt;
  const CurveSampling *PS = smp(cc, part->id), *LS = smp(cc, low->id); if (!PS || !LS) return std::nullopt;
  // (no finer than ~200 loops round it: a step far too small for the head — a project's step left from before its head was
  // known — made a crown of tens of millions of vertices and held the program for minutes)
  const double step = std::max(loopStep(sc, k), std::max(LS->arc.back(), PS->arc.back()) / 200); const int NV = 33;
  struct R { Rad r; std::vector<Vec3> P; };
  std::vector<R> rad;
  for (auto& r : crownCurvesCC(sc, cc, k)) { const CurveSampling* s = smp(cc, r.c); if (!s) continue; auto P = s->p; if (r.rev) std::reverse(P.begin(), P.end()); rad.push_back({r, resample(P, NV)}); }
  if (rad.empty()) return std::nullopt;
  // vertical loops: at both ends of the lower connector, at every crown curve and every lock curve attached to it
  const double Lw = LS->arc.back(); std::vector<double> us{0, 1}; for (auto& r : rad) us.push_back(r.r.u);
  for (const Item* c : sc.curves()) if (c->conn.isEmpty() && c->ring < 0 && !c->anchors.empty()) for (int a : {c->anchors.front(), c->anchors.back()}) if (onConnector(sc, a, *low)) us.push_back(connT(sc, cc, *low, a));
  std::sort(us.begin(), us.end());
  std::vector<double> U; for (double u : us) if (U.empty() || (u - U.back()) * Lw > step * 0.1) U.push_back(u);
  // a lock wall whose two curves both start on the lower connector: the crown gets the same columns there
  auto onLow = [&](const Item* c) -> std::optional<double> { if (!c || c->anchors.empty()) return std::nullopt; for (int a : {c->anchors.front(), c->anchors.back()}) if (onConnector(sc, a, *low)) return connT(sc, cc, *low, a); return std::nullopt; };
  struct W { double lo, hi, ua, ub; int segs; QJsonObject p; };
  std::vector<W> walls;
  std::vector<const Item*> surfs; for (auto& [id, s] : sc.items) if (s.type == ItemType::Surface) surfs.push_back(&s);
  std::sort(surfs.begin(), surfs.end(), [](auto* a, auto* b) { return a->order < b->order; });
  for (const Item* s : surfs) {
    auto ua = onLow(sc.item(s->a)), ub = onLow(sc.item(s->b));
    QJsonObject p = s->raw.value("p").toObject();
    if (ua && ub && std::abs(*ua - *ub) > 1e-6) walls.push_back({std::min(*ua, *ub), std::max(*ua, *ub), *ua, *ub, std::max(1, p.value("segs").toInt(1)), p});
  }
  auto near = [&](double x, double y) { return std::abs(x - y) * Lw < step * 0.1; };
  auto corner = [&](double u) { for (auto& w : walls) if (near(u, w.lo) || near(u, w.hi)) return true; return false; };
  auto inWall = [&](double u) { if (corner(u)) return false; for (auto& w : walls) if (u > w.lo && u < w.hi) return true; return false; };
  std::vector<double> cand; for (double u : U) if (!inWall(u)) cand.push_back(u);
  for (auto& w : walls) for (int j = 0; j < w.segs - 1; j++) cand.push_back(w.ua + (w.ub - w.ua) * segPos(double(j + 1) / w.segs, w.p, w.segs));
  std::sort(cand.begin(), cand.end());
  std::vector<double> U2; for (double u : cand) if (U2.empty() || !near(u, U2.back())) U2.push_back(u);
  std::vector<double> loopsU;
  for (size_t i = 0; i < U2.size(); i++) {
    loopsU.push_back(U2[i]);
    if (i + 1 == U2.size()) continue;
    double mid = (U2[i] + U2[i + 1]) / 2; bool inside = false; for (auto& w : walls) if (mid > w.lo && mid < w.hi) inside = true;
    if (inside) continue;                                             // within a wall: its columns only
    int n = int(jsRound((U2[i + 1] - U2[i]) * Lw / step)) - 1;
    for (int k2 = 1; k2 <= n; k2++) loopsU.push_back(U2[i] + (U2[i + 1] - U2[i]) * k2 / (n + 1));
  }
  // the parting line: vertices at about the loop step; every loop ends on the nearest one (V-shaped order: front →
  // whorl → front), several loops on one vertex → triangles (the fan at the whorl)
  const double Lp = PS->arc.back(); const int nP = std::max(1, int(jsRound(Lp / step)));
  auto sAt = [&](double u) {
    if (u <= rad[0].r.u) return rad[0].r.s;
    for (size_t i = 0; i + 1 < rad.size(); i++) if (u <= rad[i + 1].r.u) { double w = (u - rad[i].r.u) / std::max(rad[i + 1].r.u - rad[i].r.u, 1e-9); return rad[i].r.s + (rad[i + 1].r.s - rad[i].r.s) * w; }
    return rad.back().r.s;
  };
  std::vector<int> idx; for (double u : loopsU) idx.push_back(int(jsRound(clamp(sAt(u), 0, 1) * nP)));
  int mi = 0; for (size_t i = 0; i < idx.size(); i++) if (idx[i] < idx[size_t(mi)]) mi = int(i);
  for (int i = mi - 1; i >= 0; i--) idx[size_t(i)] = std::max(idx[size_t(i)], idx[size_t(i + 1)]);
  for (size_t i = size_t(mi) + 1; i < idx.size(); i++) idx[i] = std::max(idx[i], idx[i - 1]);
  std::vector<Vec3> PV; for (int j = 0; j <= nP; j++) PV.push_back(*pointOnCurve(PS, double(j) / nP));
  // shape of every loop: the neighbouring crown curves blended, ends pinned to the connectors
  auto shape = [&](double u, const Vec3& end) {
    size_t i = 0; while (i + 2 < rad.size() && u > rad[i + 1].r.u) i++;
    const R& A = rad[i]; const R& B = rad[std::min(i + 1, rad.size() - 1)];
    double w = &B == &A ? 0 : clamp((u - A.r.u) / std::max(B.r.u - A.r.u, 1e-9), 0, 1);
    std::vector<Vec3> base; for (int k2 = 0; k2 < NV; k2++) base.push_back(A.P[size_t(k2)].lerp(B.P[size_t(k2)], w));
    Vec3 L0 = *pointOnCurve(LS, u), c0 = L0 - base[0], c1 = end - base[size_t(NV - 1)];
    for (int k2 = 0; k2 < NV; k2++) { double v = double(k2) / (NV - 1); base[size_t(k2)] += c0 * (1 - v) + c1 * v; }
    return base;
  };
  std::vector<std::vector<Vec3>> paths; for (size_t i = 0; i < loopsU.size(); i++) paths.push_back(shape(loopsU[i], PV[size_t(idx[i])]));
  double avgLen = 0; for (auto& P : paths) avgLen += polyLen(P); avgLen /= double(paths.size());
  QJsonObject kp = k.raw.value("p").toObject();
  const int rings = kp.value("rings").toInt(0), NR = rings > 0 ? rings : std::max(2, int(jsRound(avgLen / step)));
  std::vector<std::vector<Vec3>> Rs; for (auto& P : paths) Rs.push_back(resample(P, NR + 1));
  const int n = int(paths.size());
  // The whorl: a run of neighbouring loops ending on one parting vertex is a fan; loops of a run longer than WHORL_IN end
  // below the top (e[i] — the last ring a loop reaches) — 'merge' (default): where they have bunched up; 'seam': next to
  // the crown curve nearest the whorl, a herringbone along it
  std::vector<int> e(size_t(n), NR - 1), side(size_t(n), 0); const bool seam = kp.value("whorl").toString() == "seam";
  const R* wr = &rad[0]; for (auto& r : rad) if (r.r.s < wr->r.s) wr = &r;
  std::vector<bool> onCurve; for (double u : loopsU) { bool on = false; for (auto& r : rad) if (near(r.r.u, u)) on = true; onCurve.push_back(on); }
  auto thin = [&](int a, int b) {
    std::vector<int> alive; for (int i = a; i <= b; i++) alive.push_back(i);
    auto freeL = [&](int i) { return i > 0 && i < n - 1 && !onCurve[size_t(i)]; };
    if (seam && idx[size_t(a)] == idx[size_t(mi)]) {
      int c = a; for (int i : alive) if (std::abs(loopsU[size_t(i)] - wr->r.u) < std::abs(loopsU[size_t(c)] - wr->r.u)) c = i;
      int Rn = int(std::ceil((int(alive.size()) - WHORL_IN) / 2.0));
      for (int r = std::max(1, NR - 1 - Rn); r <= NR - 2 && int(alive.size()) > WHORL_IN; r++) {
        int p = int(std::find(alive.begin(), alive.end(), c) - alive.begin());
        std::vector<int> kill; for (int q : {p - 1, p + 1}) if (q >= 0 && q < int(alive.size()) && freeL(alive[size_t(q)])) kill.push_back(alive[size_t(q)]);
        if (int(kill.size()) > int(alive.size()) - WHORL_IN) kill.resize(size_t(std::max(0, int(alive.size()) - WHORL_IN)));
        if (kill.empty()) break;
        for (int i : kill) { e[size_t(i)] = r; side[size_t(i)] = i < c ? 1 : -1; }
        std::vector<int> na; for (int i : alive) if (!has(kill, i)) na.push_back(i); alive = na;
      }
      return;
    }
    for (int r = 1; r <= NR - 2 && int(alive.size()) > WHORL_IN; r++) {
      bool last = r == NR - 2; struct C { int q; double d; }; std::vector<C> cs;
      for (int q = 1; q + 1 < int(alive.size()); q++) if (freeL(alive[size_t(q)])) cs.push_back({q, Rs[size_t(alive[size_t(q - 1)])][size_t(r + 1)].distanceTo(Rs[size_t(alive[size_t(q + 1)])][size_t(r + 1)])});
      std::stable_sort(cs.begin(), cs.end(), [](const C& x, const C& y) { return x.d < y.d; });
      std::set<int> kill, taken;
      for (auto& x : cs) {
        if (int(alive.size() - kill.size()) <= WHORL_IN) break;
        if (!last && x.d > step * 1.2) break;                        // not bunched up yet
        if (taken.count(x.q - 1) || taken.count(x.q + 1)) continue;  // one ending loop between two going on
        taken.insert(x.q); kill.insert(alive[size_t(x.q)]);
      }
      for (int i : kill) e[size_t(i)] = r;
      std::vector<int> na; for (int i : alive) if (!kill.count(i)) na.push_back(i); alive = na;
    }
  };
  for (int i0 = 0; i0 < n;) { int i1 = i0; while (i1 + 1 < n && idx[size_t(i1 + 1)] == idx[size_t(i0)]) i1++; if (i1 - i0 + 1 > WHORL_IN) thin(i0, i1); i0 = i1 + 1; }
  CrownMesh M; auto& pos = M.pos; pos = PV; M.ring.assign(PV.size(), -1);
  std::vector<std::vector<int>> vid;
  for (int i = 0; i < n; i++) { std::vector<int> row; for (int r = 0; r <= e[size_t(i)]; r++) { row.push_back(int(pos.size())); pos.push_back(Rs[size_t(i)][size_t(r)]); M.ring.push_back(r); } vid.push_back(row); }
  auto V = [&](int i, int r) { return vid[size_t(i)][size_t(r)]; };
  std::vector<std::pair<int, int>> fork;                              // an ending loop → the loop it branches off
  auto& faces = M.faces;
  for (int r = 0; r < NR - 1; r++) {
    std::vector<int> up; for (int i = 0; i < n; i++) if (e[size_t(i)] > r) up.push_back(i);
    for (size_t t = 0; t + 1 < up.size(); t++) {
      int L = up[t], Rr = up[t + 1]; std::vector<int> J; for (int i = L + 1; i < Rr; i++) if (e[size_t(i)] == r) J.push_back(i);
      if (J.empty()) { faces.push_back({V(L, r), V(Rr, r), V(Rr, r + 1), V(L, r + 1)}); continue; }
      bool toR = side[size_t(J[0])] ? side[size_t(J[0])] > 0 : pos[size_t(V(J[0], r))].distanceTo(pos[size_t(V(Rr, r))]) < pos[size_t(V(L, r))].distanceTo(pos[size_t(V(J[0], r))]);
      std::vector<int> B{L}; B.insert(B.end(), J.begin(), J.end()); B.push_back(Rr);
      for (int j : J) fork.push_back({j, toR ? Rr : L});
      if (toR) { faces.push_back({V(L, r), V(J[0], r), V(Rr, r + 1), V(L, r + 1)}); for (size_t q = 1; q + 1 < B.size(); q++) faces.push_back({V(B[q], r), V(B[q + 1], r), V(Rr, r + 1)}); }
      else { faces.push_back({V(J.back(), r), V(Rr, r), V(Rr, r + 1), V(L, r + 1)}); for (size_t q = 1; q + 1 < B.size(); q++) faces.push_back({V(B[q - 1], r), V(B[q], r), V(L, r + 1)}); }
    }
  }
  std::vector<int> top; for (int i = 0; i < n; i++) if (e[size_t(i)] == NR - 1) top.push_back(i);
  for (size_t t = 0; t + 1 < top.size(); t++) {
    int i = top[t], i2 = top[t + 1], a = vid[size_t(i)][size_t(NR - 1)], b = vid[size_t(i2)][size_t(NR - 1)], p0 = idx[size_t(i)], p1 = idx[size_t(i2)];
    if (p0 == p1) { faces.push_back({a, b, p1}); continue; }
    std::vector<int> seq; int sg = p0 > p1 ? 1 : -1; for (int j = p1;; j += sg) { seq.push_back(j); if (j == p0) break; }
    faces.push_back({a, b, seq[0], seq[1]});
    for (size_t j = 1; j + 1 < seq.size(); j++) faces.push_back({a, seq[j], seq[j + 1]});
  }
  // forks: a loop j ending on ring r and the loop X it branches off meet gradually, like a Y
  auto still = [&](int i) { return onCurve[size_t(i)] || i == 0 || i == n - 1; };
  auto bell = [](double t) { return 0.5 * std::exp(-t * t / 4.5); };
  for (auto& [j, X] : fork) {
    int r = e[size_t(j)]; bool fx = !still(X);
    for (int t = -4; t <= 4; t++) {
      int q = r + 1 + t; double w = bell(t); if (q < 1) continue;
      if (fx && q <= e[size_t(X)]) pos[size_t(V(X, q))] = pos[size_t(V(X, q))].lerp(Rs[size_t(j)][size_t(q)], w);
      if (t < 0) pos[size_t(V(j, q))] = pos[size_t(V(j, q))].lerp(Rs[size_t(X)][size_t(q)], fx ? w : std::min(0.9, 2 * w));
    }
  }
  // outward winding: away from a point well below the crown
  Box3 box; for (auto& p : pos) box.expand(p); Vec3 cen = box.center(); cen.y -= box.size().length();
  int vote = 0; for (auto& f : faces) { double d = faceNormal(pos, f).dot(faceCenter(pos, f) - cen); vote += (d > 0) - (d < 0); }
  if (vote < 0) for (auto& f : faces) std::reverse(f.begin(), f.end());
  M.loops = n; M.rings = NR; M.part = nP + 1;
  for (int i = 0; i < n; i++) M.root.push_back(V(i, 0));
  return M;
}

// ---------- create / add a crown curve ----------
// the head of the meshes marked as the head, from its top (topY — the skull's, under any antennae or ears sticking up)
// down: a whole character marked — only what is above its neck (the narrowest place under the head, the shoulders
// widening below it); a head alone — all of it
Box3 headRegion(const Scene& sc, double topY) {
  std::vector<Vec3> P; Box3 B;
  for (auto& [id, it] : sc.items) if (it.headTag && it.poly && sc.isVisible(it)) for (const Vec3& p : it.poly->pos) { P.push_back(p); B.expand(p); }
  const double H = topY - B.min.y; if (P.size() < 50 || !(H > 0)) return B;
  const int N = 160; const double dy = H * 0.6 / N;
  std::vector<double> lo(N, 1e300), hi(N, -1e300);
  for (const Vec3& p : P) { const int k = int((topY - p.y) / dy); if (k < 0 || k >= N) continue; lo[size_t(k)] = std::min(lo[size_t(k)], p.x); hi[size_t(k)] = std::max(hi[size_t(k)], p.x); }
  std::vector<double> w(N, -1); for (int k = 0; k < N; k++) if (hi[size_t(k)] >= lo[size_t(k)]) w[size_t(k)] = hi[size_t(k)] - lo[size_t(k)];
  double wm = 0; int neck = -1; double wn = 1e300; int kn = -1;
  for (int k = 0; k < N; k++) { const double x = w[size_t(k)]; if (x < 0) continue;
    if (kn < 0) { wm = std::max(wm, x); if (k > 4 && wm > 0 && x < 0.8 * wm) { kn = k; wn = x; } continue; }   // (below the head's widest: narrowing — the neck)
    if (x < wn) { wn = x; kn = k; }
    if (x > 1.6 * wn && x > wm) { neck = kn; break; }                 // (wider than the head again: the shoulders)
  }
  Box3 R; const double cut = neck >= 0 ? topY - (neck + 0.5) * dy : -1e300;
  for (const Vec3& p : P) if (p.y >= cut && p.y <= topY + 0.05 * H) R.expand(p);
  return R.empty() ? B : R;
}

int createCrown(Scene& sc, const std::optional<Vec3>& target, const std::optional<Vec3>& normal, const HeadHit& hit, const Vec3& center, double scale, bool sym, Selection& sel, QString* msg, const std::vector<Vec3>* coll, bool bangs) {
  // fitting the reference to the head: its top (rays from above), its width at the lower connector's height (rays from
  // the sides) → scale; the back of the head there → where the back of the lower connector goes
  bool head = false; for (auto& [id, it] : sc.items) if (it.headTag && it.poly && sc.isVisible(it)) head = true;
  Box3 B; for (auto& [id, it] : sc.items) if (it.headTag && it.poly && sc.isVisible(it)) for (const Vec3& p : it.poly->pos) B.expand(p);   // (its vertices — where it is drawn)
  const Vec3 part0 = bangs ? Vec3((BPART[0][0] + BPART[1][0]) / 2, BPART[0][1], BPART[0][2]) : Vec3(TPART[0][0], TPART[0][1], TPART[0][2]);   // (a click: where the whorl / the bangs' parting goes)
  std::function<Vec3(const Vec3&)> place;
  double topAt = std::nan("");                                       // (where the crown's top goes: the collision's top)
  if (head) {
    // (the top of the head: over the middle of the model's top eighth — the head's mass; the scene's middle, or the whole
    // model's, may lie well away from it: a tail, long hair, arms out. Rays from above along a line front to back through
    // it — the third highest they meet: an antenna or two at the front caught by a ray or two left out. Then the head
    // under it — a whole character marked: above its neck)
    Vec3 bc; { int n = 0; const double y0 = B.max.y - 0.12 * B.size().y; for (auto& [id, it] : sc.items) if (it.headTag && it.poly && sc.isVisible(it)) for (const Vec3& p : it.poly->pos) if (p.y >= y0) { bc += p; n++; } bc = n ? bc / double(n) : B.center(); }
    double big = scale * 2, R = B.size().y * 0.06; std::vector<Vec3> tops;
    for (int j = -8; j <= 8; j++) if (auto h = hit(Vec3(bc.x, B.max.y + big * 0.1, bc.z + j * R / 8), Vec3(0, -1, 0), false)) tops.push_back(*h);
    std::optional<Vec3> topP;
    if (!tops.empty()) { std::sort(tops.begin(), tops.end(), [](const Vec3& a, const Vec3& b) { return a.y > b.y; }); topP = tops[std::min<size_t>(2, tops.size() - 1)]; topP->x = bc.x; }
    if (topP) B = headRegion(sc, topP->y);
    // (the head's collision capsule: the crown stands on its top — the hair lies on the collision, not on the skin)
    if (coll && coll->size() > 3) {
      Vec3 t = (*coll)[0]; Box3 cb; for (const Vec3& p : *coll) { cb.expand(p); if (p.y > t.y) t = p; }
      topP = t; B = cb;
    }
    if (topP) {
      const double tw = 13.2, depth = 2.6; const Vec3 back0(0, 149.6, -11.45);
      double tdepth = 0; { double zmax = -1e300; for (auto& p : TPART) zmax = std::max(zmax, double(p[2])); for (auto& r : TRAD()) for (auto& p : r) zmax = std::max(zmax, p.z); tdepth = std::max(1.0, zmax - back0.z); }   // (the reference head's depth: its front to its back)
      double s = std::max(1e-6, std::max(B.size().x, B.size().z) / 16.0), cx = topP->x, y = topP->y;   // (first guess from the head's size — the template is in centimetres, a model may be in metres; then the width measured)
      // (on the capsule: its width and depth at the connector's height, from its own points — the head's rays would
      // measure the skin under it)
      auto section = [&](double yy, double& x0, double& x1, double& z0, double& z1) {
        x0 = z0 = 1e300; x1 = z1 = -1e300; const double band = 0.08 * B.size().y;
        for (const Vec3& p : *coll) if (std::abs(p.y - yy) <= band) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.z); z1 = std::max(z1, p.z); }
        return x1 > x0 && z1 > z0;
      };
      double backZ = 0; bool onCap = false;
      if (coll && coll->size() > 3) for (int it = 0; it < 4; it++) {
        y = topP->y - depth * s; double x0, x1, z0, z1; if (!section(y, x0, x1, z0, z1)) break;
        cx = (x0 + x1) / 2; s = std::min((x1 - x0) / tw, (z1 - z0) / tdepth); backZ = z0; onCap = true;
      }
      if (!onCap) for (int it = 0; it < 4; it++) {
        y = topP->y - depth * s;
        auto l = hit(Vec3(topP->x - big, y, topP->z), Vec3(1, 0, 0), false), r = hit(Vec3(topP->x + big, y, topP->z), Vec3(-1, 0, 0), false);
        if (!l || !r) break;
        double w = r->x - l->x; if (!(w > 1e-6)) break;
        cx = (l->x + r->x) / 2; s = w / tw;
        // (the head's depth there too, front to back through its middle: hair buns, ears or hair hanging at the sides
        // make it look wider than it is — the smaller of the two is taken)
        auto fr = hit(Vec3(cx, y, B.max.z + big), Vec3(0, 0, -1), false), bkk = hit(Vec3(cx, y, B.min.z - big), Vec3(0, 0, 1), false);
        if (fr && bkk && fr->z - bkk->z > 1e-6) s = std::min(s, (fr->z - bkk->z) / tdepth);
      }
      auto bk = onCap ? std::optional<Vec3>(Vec3(cx, y, backZ)) : hit(Vec3(cx, y, B.min.z - big), Vec3(0, 0, 1), false);
      Vec3 tp = *topP;
      if (!target) topAt = onCap ? tp.y : tp.y + 0.25 * s;
      if (target) { Vec3 nn = normal ? *normal : Vec3(0, 1, 0), tg = *target; place = [=](const Vec3& p) { return (p - part0) * s + tg + nn * (0.25 * s); }; }
      else place = [=](const Vec3& p) { return (p - back0) * s + Vec3(cx, y, bk ? bk->z : tp.z - 6 * s); };
    }
  }
  if (!place) { Vec3 off = target ? *target - part0 : Vec3(); place = [=](const Vec3& p) { return p + off; }; }
  // the group: the conversion settings of a hair group that has them, the project's step
  QJsonValue conv; for (auto& [id, g] : sc.items) if (g.type == ItemType::Group && g.raw.value("conv").isObject() && !g.raw.value("guidesGroup").toBool() && !g.raw.value("meshesGroup").toBool()) { conv = g.raw.value("conv"); break; }
  int cnum = sc.fileRaw.value("counters").toObject().value("crown").toInt(0);
  edit::nextNumber(sc, ItemType::Group);
  Item& g = sc.addItem(ItemType::Group, -1, QString(bangs ? "Bangs %1" : "Crown %1").arg(cnum + 1)); g.expanded = true; int gid = g.id;
  QJsonObject cv = conv.toObject(); cv.insert("step", projectStep(sc));
  g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "auto"}, {"conv", cv}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                      {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
  std::vector<int> all;
  auto mk = [&](const std::vector<Vec3>& pts, const QString& name) {
    edit::nextNumber(sc, ItemType::Curve);
    Item& c = sc.addItem(ItemType::Curve, gid, name); int id = c.id;
    std::vector<int> an; for (auto& p : pts) { int a = sc.addAnchor(place(p)).id; an.push_back(a); all.push_back(a); }
    sc.item(id)->anchors = an; return id;
  };
  std::vector<Vec3> tpart; if (bangs) for (auto& p : BPART) tpart.push_back({p[0], p[1], p[2]}); else for (auto& p : TPART) tpart.push_back({p[0], p[1], p[2]});
  int part = mk(tpart, "Parting line"), low = mk({}, "Lower connector");
  sc.item(part)->conn = "part"; sc.item(part)->raw.insert("conn", "part"); sc.item(low)->conn = "low"; sc.item(low)->raw.insert("conn", "low");
  int knum = edit::nextNumber(sc, ItemType::Crown);
  Item& k = sc.addItem(ItemType::Crown, gid, QString(bangs ? "Bangs %1" : "Crown %1").arg(knum)); int kid = k.id;
  k.raw = QJsonObject{{"part", part}, {"low", low}, {"p", QJsonObject{{"rings", 0}, {"visV", 1}}}};
  { auto& ch = sc.item(gid)->children; ch.erase(std::remove(ch.begin(), ch.end(), kid), ch.end()); ch.insert(ch.begin(), kid); }
  std::vector<int> rads; int i = 1; for (auto& r : bangs ? BRAD() : TRAD()) rads.push_back(mk(r, QString("Crown curve %1").arg(i++)));
  // the lower connector runs through the lower ends of the crown curves (shared anchors)
  { std::vector<int> la; for (int c : rads) la.push_back(sc.item(c)->anchors.front()); sc.item(low)->anchors = la; }
  if (bangs) {                                                      // (the bangs': along its whole lower edge — the curves' roots where they are, anchors of its own between)
    std::vector<int> la; const double eps = 1e-3;
    for (const Vec3& p : BLOW()) {
      int a = -1; for (size_t i = 0; i < rads.size(); i++) if (BRAD()[i].front().distanceTo(p) < eps) a = sc.item(rads[i])->anchors.front();
      if (a < 0) { a = sc.addAnchor(place(p)).id; all.push_back(a); }
      la.push_back(a);
    }
    for (int i : BCORN) if (i < int(la.size())) sc.item(low)->sharp.push_back(la[size_t(i)]);   // (its lower corners: sharp — on the connector only)
    sc.item(low)->anchors = la;
  }
  // (its shape is the template's — only scaled and moved, never an anchor pushed on its own: its top on the collision's
  // top — the capsule's, or the skull's with the crown's own thickness above it)
  if (std::isfinite(topAt) && !all.empty()) {
    double ymax = -1e300; for (int a : all) ymax = std::max(ymax, sc.anchor(a)->pos.y);
    for (int a : all) sc.anchor(a)->pos.y += topAt - ymax;
  }
  sc.reindex();
  CurveCache cc; for (const Item* c : sc.curves()) if (c->parent == gid) cc[c->id] = sampleCurveFull(sc, *c);
  if (bangs) {                                                      // (the bangs: small — the project's step would give it two rings; its own, about ten down a curve)
    double len = 0; int n = 0; for (int c : rads) if (auto it = cc.find(c); it != cc.end() && !it->second.arc.empty()) { len = std::max(len, it->second.arc.back()); n++; }
    if (n && len > 0) { const double st = std::min(projectStep(sc), len / 10); QJsonObject cv2 = sc.item(gid)->raw.value("conv").toObject(); cv2.insert("step", st); sc.item(gid)->raw.insert("conv", cv2); }
  }
  for (int c : rads) { int a = sc.item(c)->anchors.back(); sc.anchor(a)->raw.insert("at", QJsonObject{{"c", part}, {"t", nearestT(cc[part], sc.anchor(a)->pos)}}); }
  if (sym) { for (int c : rads) sampleInto(sc, cc, c); makeSymmetric(sc, cc, kid, 1, center.x, scale, nullptr); }
  contPivots(sc);                                                   // (the crown group's point at the crown's middle — where it appeared)
  sel.anchors.clear(); sel.items = {kid};
  sc.reindex();
  if (msg && bangs) *msg = QObject::tr("Bangs created — move the parting line, the lower connector and the crown curves; attach lock roots to the lower connector");
  else if (msg) *msg = QObject::tr("Crown created — move the parting line, the lower connector and the crown curves; attach lock roots to the lower connector");
  return kid;
}

int lowConnAt(const Scene& sc, int a) {
  for (const Item* c : sc.curves()) if (c->conn == "low" && has(c->anchors, a)) return c->id;
  return -1;
}

bool sharpCorner(const Scene& sc, int a) { const Item* c = sc.item(lowConnAt(sc, a)); return c && has(c->sharp, a); }

void setSharpCorner(Scene& sc, int a, bool on) {
  const int lid = lowConnAt(sc, a); Item* c = sc.item(lid); if (!c) return;
  std::vector<int> as{a};
  // (a symmetric crown: the anchor's pair too)
  for (int kid : crowns(sc)) { const Item* k = sc.item(kid); if (lowOf(*k) != lid) continue; QJsonObject sy = k->raw.value("p").toObject().value("sym").toObject(); if (!sy.value("on").toBool()) break;
    for (const QJsonValue& v : sy.value("pairs").toArray()) { QJsonArray pr = v.toArray(); int x = pr[0].toInt(), y = pr[1].toInt(); if (x == a && has(c->anchors, y)) as.push_back(y); else if (y == a && has(c->anchors, x)) as.push_back(x); } }
  for (int x : as) { auto it = std::find(c->sharp.begin(), c->sharp.end(), x); if (on && it == c->sharp.end()) c->sharp.push_back(x); else if (!on && it != c->sharp.end()) c->sharp.erase(it); }
}

void contPivots(Scene& sc) {
  for (auto& [gid, g] : sc.items) {
    if (g.type != ItemType::Group) continue;
    const bool cont = g.raw.value("cont").toBool() || g.name.endsWith(QStringLiteral(" · continuation")) || g.name == QStringLiteral("Crown continuation");
    bool crownG = false; for (auto& [id, c] : sc.items) if (c.parent == gid && c.type == ItemType::Crown) { crownG = true; break; }
    if (!cont && !crownG) continue;
    // (its spawn point: a continuation — the middle of its curves' roots; a crown — the middle of its curves)
    Vec3 C, lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300); int n = 0;
    for (auto& [id, c] : sc.items) if (c.parent == gid && c.type == ItemType::Curve && !c.anchors.empty()) {
      for (size_t i = 0; i < c.anchors.size(); i++) if (const Anchor* a = sc.anchor(c.anchors[i])) {
        lo = Vec3(std::min(lo.x, a->pos.x), std::min(lo.y, a->pos.y), std::min(lo.z, a->pos.z)); hi = Vec3(std::max(hi.x, a->pos.x), std::max(hi.y, a->pos.y), std::max(hi.z, a->pos.z));
        if (!cont || i == 0) { C += a->pos; n++; } } }
    if (!n) continue; C = C / double(n);
    // (kept when it has one near its contents — set there by hand; none, or far off them — the spawn point)
    const QJsonArray p = g.raw.value("gpivot").toArray();
    if (p.size() == 3) { const Vec3 q(p[0].toDouble(), p[1].toDouble(), p[2].toDouble()); const double d = (hi - lo).length();
      const bool near = q.x > lo.x - d && q.x < hi.x + d && q.y > lo.y - d && q.y < hi.y + d && q.z > lo.z - d && q.z < hi.z + d; if (near) continue; }
    g.raw.insert("gpivot", QJsonArray{C.x, C.y, C.z}); if (cont) g.raw.insert("cont", true);
  }
}

// one more crown curve in the middle of the widest gap between the crown curves (shaped like its neighbours)
QString addContinuation(Scene& sc, const CurveCache& cc, int kid, double len, const HeadHit& hit, Selection& sel) {
  const Item* k = sc.item(kid); if (!k) return "!";
  const Item *part = sc.item(partOf(*k)), *low = sc.item(lowOf(*k)); if (!part || !low || low->anchors.size() < 2) return QObject::tr("!The crown has no lower connector");
  // (the crown's middle and height: its parting line and lower connector)
  Vec3 C; int n = 0; double ylo = 1e300, yhi = -1e300;
  for (const Item* c : {part, low}) for (int a : c->anchors) { const Vec3& p = sc.anchor(a)->pos; C += p; n++; ylo = std::min(ylo, p.y); yhi = std::max(yhi, p.y); }
  C = C / double(std::max(1, n)); const double H = std::max(1e-6, yhi - ylo), L = std::max(0.2, len) * H;
  // (the connector's anchors a lock curve starts from already: none made there)
  std::set<int> used; for (const Item* c : sc.curves()) if (c->conn.isEmpty() && c->ring < 0 && !c->anchors.empty()) for (int a : {c->anchors.front(), c->anchors.back()}) used.insert(a);
  std::set<int> rad; for (auto& r : crownCurvesCC(sc, cc, *k)) if (const Item* c = sc.item(r.c)) for (int a : c->anchors) rad.insert(a);
  edit::nextNumber(sc, ItemType::Group);
  Item& g = sc.addItem(ItemType::Group, k->parent >= 0 ? sc.item(k->parent)->parent : -1, sc.item(k->parent) ? sc.item(k->parent)->name + QObject::tr(" · continuation") : QObject::tr("Crown continuation"));
  g.expanded = true; const int gid = g.id;
  if (const Item* kg = sc.item(k->parent)) { QJsonObject r = kg->raw; r.insert("sym", QJsonValue()); r.remove("gpivot"); r.remove("q"); sc.item(gid)->raw = r; }   // (the crown group's conversion settings — not its point)
  sc.item(gid)->raw.insert("cont", true);
  // (straight down from the connector's own points — plumb walls, no bend nor lean. The head in their way: the crown
  // itself made wider — every anchor of it out from its middle, across only — the least that keeps every wall off the head,
  // each point outside the outermost hit along its ray)
  std::set<int> own; for (const Item* c : sc.curves()) if (c->parent == k->parent) for (int a : c->anchors) own.insert(a);
  for (const Item* c : {part, low}) for (int a : c->anchors) own.insert(a);
  double wide = 1;
  auto widened = [&](const Vec3& p) { return Vec3(C.x + (p.x - C.x) * wide, p.y, C.z + (p.z - C.z) * wide); };
  if (hit) for (int it = 0; it < 30; it++) {
    bool inside = false;
    for (int a : low->anchors) { if (used.count(a) && !rad.count(a)) continue;
      const Vec3 p0 = widened(sc.anchor(a)->pos); const double r0 = std::hypot(p0.x - C.x, p0.z - C.z);   // (a hit far past the crown's own reach — a shoulder, an arm — not the head: left be)
      for (int i = 1; i <= 8 && !inside; i++) { const Vec3 q = p0 + Vec3(0, -L * i / 8.0, 0), o(C.x, q.y, C.z);
        Vec3 d = q - o; if (d.lengthSq() < 1e-12) continue; d = d.normalized();
        if (auto h = hit(o, d, true); h && h->distanceTo(o) < r0 * 1.6 && h->distanceTo(o) + H * 0.02 > q.distanceTo(o)) inside = true; } }
    if (!inside) break; wide += 0.02;
  }
  if (wide > 1) for (int a : own) if (Anchor* an = sc.anchor(a)) an->pos = widened(an->pos);
  std::vector<int> made;                                            // (in the connector's order; -1 — none there)
  for (int a : low->anchors) {
    if (used.count(a) && !rad.count(a)) { made.push_back(-1); continue; }
    const Vec3 p0 = sc.anchor(a)->pos;
    std::vector<int> an{a};
    for (int i = 1; i <= 3; i++) an.push_back(sc.addAnchor(p0 + Vec3(0, -L * i / 3.0, 0)).id);   // (plumb down: an even, upright wall)
    edit::nextNumber(sc, ItemType::Curve);
    Item& c = sc.addItem(ItemType::Curve, gid, QString("C_%1").arg(made.size() + 1)); c.anchors = an; made.push_back(c.id);
  }
  int walls = 0;
  for (size_t i = 0; i + 1 < made.size(); i++) if (made[i] >= 0 && made[i + 1] >= 0) if (edit::makeSurface(sc, made[i], made[i + 1]) >= 0) walls++;
  std::vector<int> cs; for (int x : made) if (x >= 0) cs.push_back(x);
  if (cs.empty()) { sc.removeFromParent(gid); sc.items.erase(gid); return QObject::tr("!Every anchor of the lower connector has a lock already"); }
  contPivots(sc);                                                   // (its point at its roots — where the crown's continuation starts)
  sc.reindex();
  sel.anchors.clear(); sel.items = {gid};
  return QObject::tr("Crown continuation: %1 curves, %2 walls from the lower connector — cut them into locks with the Scissors (NURBS surface)").arg(cs.size()).arg(walls);
}

QString addCrownCurve(Scene& sc, const CurveCache& cc, int kid, Selection& sel) {
  const Item* k = sc.item(kid); if (!k) return "!";
  const Item *part = sc.item(partOf(*k)), *low = sc.item(lowOf(*k)); auto rs = crownCurvesCC(sc, cc, *k);
  if (!part || !low || rs.size() < 2) return "!";
  size_t bi = 0; for (size_t i = 1; i + 1 < rs.size(); i++) if (rs[i + 1].u - rs[i].u > rs[bi + 1].u - rs[bi].u) bi = i;
  const Rad A = rs[bi], Bq = rs[bi + 1];
  auto P = [&](const Rad& r) { const CurveSampling* s = smp(cc, r.c); auto p = s ? s->p : std::vector<Vec3>{}; if (r.rev) std::reverse(p.begin(), p.end()); return resample(p, 4); };
  auto pa = P(A), pb = P(Bq);
  edit::nextNumber(sc, ItemType::Curve);
  int parent = k->parent;
  Item& c = sc.addItem(ItemType::Curve, parent, QString("Crown curve %1").arg(rs.size() + 1)); int cid = c.id;
  std::vector<int> an; for (int i = 0; i < 4; i++) an.push_back(sc.addAnchor(pa[size_t(i)].lerp(pb[size_t(i)], 0.5)).id);
  { auto& arr = parent >= 0 ? sc.item(parent)->children : sc.roots; arr.erase(std::remove(arr.begin(), arr.end(), cid), arr.end());
    auto at = std::find(arr.begin(), arr.end(), Bq.c); arr.insert(at, cid); }
  // lower end: a new anchor of the connector between the neighbours' ones (shared), else attached
  const Item *ca = sc.item(A.c), *cb = sc.item(Bq.c);
  int ea = A.rev ? ca->anchors.back() : ca->anchors.front(), eb = Bq.rev ? cb->anchors.back() : cb->anchors.front();
  Item* lw = sc.item(low->id);
  int ia = int(std::find(lw->anchors.begin(), lw->anchors.end(), ea) - lw->anchors.begin()), ib = int(std::find(lw->anchors.begin(), lw->anchors.end(), eb) - lw->anchors.begin());
  double um = (A.u + Bq.u) / 2;
  if (ia < int(lw->anchors.size()) && ib < int(lw->anchors.size()) && std::abs(ia - ib) == 1) {
    auto p = pointOnCurve(smp(cc, low->id), um); Anchor& n = sc.addAnchor(p ? *p : sc.anchor(an[0])->pos);
    an[0] = n.id; lw->anchors.insert(lw->anchors.begin() + std::max(ia, ib), n.id);
  } else sc.anchor(an[0])->raw.insert("at", QJsonObject{{"c", low->id}, {"t", um}});
  sc.anchor(an[3])->raw.insert("at", QJsonObject{{"c", part->id}, {"t", (A.s + Bq.s) / 2}});
  sc.item(cid)->anchors = an;
  sel.anchors.clear(); sel.items = {cid};
  sc.reindex();
  return QObject::tr("Crown curve added");
}

// ---------- symmetry ----------
QString makeSymmetric(Scene& sc, CurveCache& cc, int kid, int master, double cx, double scale, Selection* sel) {
  Item* k = sc.item(kid); if (!k) return "!";
  int partId = partOf(*k), lowId = lowOf(*k); if (!sc.item(partId) || !sc.item(lowId)) return "!";
  sampleInto(sc, cc, partId); sampleInto(sc, cc, lowId);
  const CurveSampling* LS = smp(cc, lowId);
  double tol = std::max((LS ? LS->arc.back() : scale) * 0.01, 1e-6);
  auto side = [&](int a) { double d = sc.anchor(a)->pos.x - cx; return std::abs(d) < tol ? 0 : (d > 0 ? 1 : -1); };
  auto mir = [&](Vec3 p) { p.x = 2 * cx - p.x; return p; };
  auto rads = crownCurvesCC(sc, cc, *k); std::set<int> crownIds; for (auto& r : rads) crownIds.insert(r.c);
  auto lowEnd = [&](const Rad& r) { const Item* c = sc.item(r.c); return r.rev ? c->anchors.back() : c->anchors.front(); };
  const std::vector<int> L = sc.item(lowId)->anchors; std::vector<int> sides; for (int a : L) sides.push_back(side(a));
  // (the connector's ends both next to the plane — the bangs' back arch meeting over the top: each end on its own side,
  // not two middles pulled together)
  if (L.size() > 2 && sides.front() == 0 && sides.back() == 0) {
    const double d0 = sc.anchor(L.front())->pos.x - cx, d1 = sc.anchor(L.back())->pos.x - cx;
    if (d0 != d1) { sides.front() = d0 > d1 ? 1 : -1; sides.back() = -sides.front(); }   // (by which is further +X: the head's middle found by rays — the plane may miss the gap between them)
  }
  std::map<int, int> sideAt; for (size_t i = 0; i < L.size(); i++) sideAt[L[i]] = sides[i];
  auto sideOf = [&](int a) { auto f = sideAt.find(a); return f != sideAt.end() ? f->second : side(a); };   // (a connector anchor: as sorted above)
  std::vector<int> masters; for (size_t i = 0; i < L.size(); i++) if (sides[i] == master) masters.push_back(L[i]);
  bool any = !masters.empty(); for (auto& r : rads) if (sideOf(lowEnd(r)) == master) any = true;
  if (!any) return QObject::tr("!The crown has nothing on that side to mirror");
  std::vector<std::pair<int, int>> pairs; std::vector<int> mid; std::map<int, int> amap;
  auto mirA = [&](int a) { auto f = amap.find(a); if (f != amap.end()) return f->second; const Anchor A = *sc.anchor(a); Anchor& n = sc.addAnchor(mir(A.pos)); n.smooth = A.smooth; amap[a] = n.id; pairs.push_back({a, n.id}); return n.id; };
  auto toMid = [&](int a) { Anchor* A = sc.anchor(a); if (!A->raw.value("at").isObject()) A->pos.x = cx; if (!has(mid, a)) mid.push_back(a); };
  for (int a : sc.item(partId)->anchors) toMid(a);                    // the parting line lies on the plane
  std::vector<int> foreign;
  for (size_t i = 0; i < L.size(); i++) if (sides[i] == -master) for (int c : curvesWith(sc, L[i])) { const Item* x = sc.item(c); if (x->conn.isEmpty() && x->ring < 0 && !crownIds.count(c)) { foreign.push_back(L[i]); break; } }
  std::vector<int> mids; for (size_t i = 0; i < L.size(); i++) if (sides[i] == 0) mids.push_back(L[i]);
  for (int a : mids) toMid(a);
  int iM = -1, iS = -1; for (size_t i = 0; i < L.size(); i++) { if (iM < 0 && sides[i] == master) iM = int(i); if (iS < 0 && sides[i] == -master) iS = int(i); }
  // a lock anchor there that already IS a master anchor's mirror (its pair from the last mirroring, or at the mirrored
  // spot) stays as that mirror
  QJsonArray oldPairs = k->raw.value("p").toObject().value("sym").toObject().value("pairs").toArray();
  std::set<int> reused; double reach = std::max(tol, loopStep(sc, *k) * 1.5);
  for (int m : masters) {
    int f = -1;
    for (const QJsonValue& v : oldPairs) { QJsonArray pr = v.toArray(); int x = pr[0].toInt(), y = pr[1].toInt(), o = x == m ? y : y == m ? x : -1;
      if (o >= 0 && f < 0 && has(foreign, o) && !reused.count(o)) f = o; }
    if (f < 0) { double bd = reach; for (int a : foreign) { if (reused.count(a)) continue; double dd = sc.anchor(a)->pos.distanceTo(mir(sc.anchor(m)->pos)); if (dd < bd) { bd = dd; f = a; } } }
    if (f >= 0) { reused.insert(f); sc.anchor(f)->pos = mir(sc.anchor(m)->pos); amap[m] = f; pairs.push_back({m, f}); }
  }
  std::vector<int> copies; for (int m : masters) copies.push_back(mirA(m)); std::reverse(copies.begin(), copies.end());
  std::vector<int> nl;
  if (iS >= 0 && iS < iM) { nl = copies; nl.insert(nl.end(), mids.begin(), mids.end()); nl.insert(nl.end(), masters.begin(), masters.end()); }
  else { nl = masters; nl.insert(nl.end(), mids.begin(), mids.end()); nl.insert(nl.end(), copies.begin(), copies.end()); }
  { std::vector<int> sh; for (int a : sc.item(lowId)->sharp) if (has(nl, a)) { sh.push_back(a); if (auto f = amap.find(a); f != amap.end()) sh.push_back(f->second); }   // (the connector's corners: the master side's, mirrored)
    sc.item(lowId)->sharp = sh; }
  sc.item(lowId)->anchors = nl; sc.reindex(); sampleInto(sc, cc, lowId);
  // crown curves: the master side's are mirrored, the other side's go, the middle ones go onto the plane
  Selection del;
  for (auto& r : rads) {
    int s = sideOf(lowEnd(r));
    if (s == -master) { del.items.push_back(r.c); continue; }
    const Item src = *sc.item(r.c);
    if (s == 0) { for (int a : src.anchors) if (!has(sc.item(partId)->anchors, a) && !has(sc.item(lowId)->anchors, a)) toMid(a); continue; }
    QString nm = src.name; nm.remove(QRegularExpression("(\\s+mirror)+$", QRegularExpression::CaseInsensitiveOption));
    edit::nextNumber(sc, ItemType::Curve);
    Item& n = sc.addItem(ItemType::Curve, src.parent, nm + " mirror"); int nid = n.id;
    std::vector<int> an;
    for (int a : src.anchors) {
      if (has(sc.item(partId)->anchors, a)) { an.push_back(a); continue; }   // on the parting line (the plane) — shared
      QJsonObject at = sc.anchor(a)->raw.value("at").toObject();
      if (!at.isEmpty()) {
        Vec3 mp = mir(sc.anchor(a)->pos); Anchor& m = sc.addAnchor(mp); int ac = at.value("c").toInt();
        m.raw.insert("at", QJsonObject{{"c", ac}, {"t", ac == lowId && smp(cc, lowId) ? nearestT(*smp(cc, lowId), mp) : at.value("t").toDouble()}});
        an.push_back(m.id); continue;
      }
      an.push_back(mirA(a));
    }
    Item& ni = *sc.item(nid); ni.anchors = an;
    for (auto& [aid, o] : src.ovr) { auto it = std::find(src.anchors.begin(), src.anchors.end(), aid); if (it == src.anchors.end()) continue;
      int m = an[size_t(it - src.anchors.begin())]; HandleOverride h; if (o.in) h.in = Vec3(-o.in->x, o.in->y, o.in->z); if (o.out) h.out = Vec3(-o.out->x, o.out->y, o.out->z); ni.ovr[m] = h; }
  }
  if (!del.items.empty()) { sc.reindex(); edit::deleteItems(sc, cc, del); }
  // the other lock curves on the replaced side stay attached (the connector rebuilt after each)
  for (int a : foreign) if (!reused.count(a)) { sc.reindex(); insertIntoConnector(sc, cc, lowId, a); sampleInto(sc, cc, lowId); }
  QJsonArray P; for (auto& [a, b] : pairs) P.append(QJsonArray{a, b});
  QJsonArray Mi; for (int a : mid) Mi.append(a);
  k = sc.item(kid); QJsonObject kp = k->raw.value("p").toObject(); kp.insert("sym", QJsonObject{{"on", true}, {"master", master}, {"pairs", P}, {"mid", Mi}}); k->raw.insert("p", kp);
  sc.reindex();
  if (!sel) return {};
  sel->anchors.clear(); sel->items = {kid};
  return master > 0 ? QObject::tr("Crown mirrored: left (+X) → right (−X) — symmetric now, either side drives the other")
                    : QObject::tr("Crown mirrored: right (−X) → left (+X) — symmetric now, either side drives the other");
}

std::vector<int> symSync(Scene& sc, std::map<int, std::map<int, Vec3>>& last, double cx) {
  std::vector<int> moved;
  for (int kid : crowns(sc)) {
    const Item* k = sc.item(kid); QJsonObject sy = k->raw.value("p").toObject().value("sym").toObject();
    if (!sy.value("on").toBool()) { last.erase(kid); continue; }
    std::set<int> mine; for (const Item* c : sc.curves()) if (c->parent == k->parent || c->id == lowOf(*k) || c->id == partOf(*k)) mine.insert(c->anchors.begin(), c->anchors.end());
    bool fresh = !last.count(kid); auto& L = last[kid];
    for (const QJsonValue& v : sy.value("pairs").toArray()) {
      QJsonArray pr = v.toArray(); int ia = pr[0].toInt(), ib = pr[1].toInt();
      if (!mine.count(ia) || !mine.count(ib)) continue;
      Anchor *a = sc.anchor(ia), *b = sc.anchor(ib); if (!a || !b) continue;
      if (!fresh && L.count(ia) && L.count(ib)) {
        bool am = !(L[ia] == a->pos), bm = !(L[ib] == b->pos);
        if (am && !bm) { b->pos = Vec3(2 * cx - a->pos.x, a->pos.y, a->pos.z); moved.push_back(ib); }
        else if (bm && !am) { a->pos = Vec3(2 * cx - b->pos.x, b->pos.y, b->pos.z); moved.push_back(ia); }
      }
      L[ia] = a->pos; L[ib] = b->pos;
    }
    for (const QJsonValue& v : sy.value("mid").toArray()) { Anchor* a = sc.anchor(v.toInt()); if (a && mine.count(a->id) && !a->raw.value("at").isObject() && std::abs(a->pos.x - cx) > 1e-9) { a->pos.x = cx; moved.push_back(a->id); } }
  }
  return moved;
}

// ---------- attachments ----------
std::vector<int> updateAttachments(Scene& sc, const CurveCache& cc, std::map<int, Vec3>& last, double scale, QString* msg) {
  std::vector<int> moved; std::set<int> seen;
  for (const Item* c : sc.curves()) for (int aid : c->anchors) {
    Anchor* a = sc.anchor(aid); if (!a || !seen.insert(aid).second) continue;
    QJsonObject at = a->raw.value("at").toObject(); if (at.isEmpty()) continue;
    const Item* h = sc.item(at.value("c").toInt(-1));
    if (!h || h->type != ItemType::Curve || has(h->anchors, aid)) { a->raw.remove("at"); last.erase(aid); continue; }
    const CurveSampling* hs = smp(cc, h->id); if (!hs) continue;
    // (the connector moved too and its samples not made again yet — a group carried whole: wait for them, nothing comes off)
    if (h->anchors.empty()) continue;
    { const Anchor* f0 = sc.anchor(h->anchors.front()); const Anchor* f1 = sc.anchor(h->anchors.back());
      if (hs->p.empty() || (f0 && (hs->p.front() - f0->pos).lengthSq() > 1e-12) || (f1 && (hs->p.back() - f1->pos).lengthSq() > 1e-12)) continue; }
    auto lp = last.find(aid);
    if (lp != last.end() && (a->pos - lp->second).lengthSq() > 1e-14) {
      double t2 = nearestT(*hs, a->pos); auto q2 = pointOnCurve(hs, t2);
      if (q2 && q2->distanceTo(a->pos) > scale * 0.002) {
        a->raw.remove("at"); last.erase(aid); moved.push_back(aid);
        if (msg) *msg = !h->conn.isEmpty() ? QObject::tr("Anchor came off the connector (Ctrl + drag slides along it)") : QObject::tr("Anchor came off the curve (Shift + drag slides along it)");
        continue;
      }
      at.insert("t", t2); a->raw.insert("at", at);
    }
    auto q = pointOnCurve(hs, at.value("t").toDouble());
    if (q && (*q - a->pos).lengthSq() > 1e-14) { a->pos = *q; moved.push_back(aid); }
    last[aid] = a->pos;
  }
  return moved;
}

void insertIntoConnector(Scene& sc, const CurveCache& cc, int connId, int a) {
  Item* conn = sc.item(connId); Anchor* A = sc.anchor(a); if (!conn || !A) return;
  CurveSampling S = smp(cc, connId) ? *smp(cc, connId) : sampleCurveFull(sc, *conn);
  if (S.p.size() < 2) { conn->anchors.push_back(a); return; }
  double t = nearestT(S, A->pos); auto p = pointOnCurve(&S, t); const auto& L = S.arc; double d = t * L.back();
  size_t i = 0; while (i + 2 < L.size() && L[i + 1] < d) i++;
  int seg = S.seg.size() > i ? S.seg[i] : 0;
  A->raw.remove("at"); if (p) A->pos = *p;
  conn->anchors.insert(conn->anchors.begin() + std::min<size_t>(size_t(seg) + 1, conn->anchors.size()), a);
  sc.reindex();
}

// a curve end on a connector becomes an anchor OF the connector (shared, like the crown curves made with the crown)
QString connectToConnector(Scene& sc, const CurveCache& cc, int a, int conn) {
  std::vector<int> other; for (int c : curvesWith(sc, a)) { const Item* x = sc.item(c); other.push_back(x->anchors.front() == a ? x->anchors.back() : x->anchors.front()); }
  int kid = -1; for (int k : crowns(sc)) if (lowOf(*sc.item(k)) == conn) kid = k;
  const Item* part = kid >= 0 ? sc.item(partOf(*sc.item(kid))) : nullptr;
  insertIntoConnector(sc, cc, conn, a);
  bool crownCurve = false;
  if (part) for (int o : other) if (onConnector(sc, o, *part)) crownCurve = true;
  return crownCurve ? QObject::tr("Crown curve connected to the lower connector")
                    : QObject::tr("Attached — the anchor is part of the connector now: drag it to reshape the connector, Ctrl + drag slides it along, Ctrl + click detaches");
}

// Ctrl + click on a curve's anchor on a connector: the curve comes off — an anchor of its own at the same spot; an
// old-style attached anchor just lets go
QString detachFromConnector(Scene& sc, int a, Selection& sel) {
  Anchor* A = sc.anchor(a); if (!A) return "!";
  if (A->raw.value("at").isObject()) { A->raw.remove("at"); return QObject::tr("Anchor detached from the connector"); }
  bool onConn = false; for (int c : curvesWith(sc, a)) if (!sc.item(c)->conn.isEmpty()) onConn = true;
  if (!onConn) return QObject::tr("!A crown curve stays on its connector");
  CurveCache none; std::set<int> crownC; for (int k : crowns(sc)) for (auto& r : crownCurvesCC(sc, none, *sc.item(k))) crownC.insert(r.c);
  std::vector<int> users; for (int c : curvesWith(sc, a)) { const Item* x = sc.item(c); if (x->conn.isEmpty() && x->ring < 0 && !crownC.count(c)) users.push_back(c); }
  if (users.empty()) return QObject::tr("!A crown curve stays on its connector");
  Vec3 p = A->pos; double sm = A->smooth;
  Anchor& n = sc.addAnchor(p); n.smooth = sm; int nid = n.id;
  for (int cid : users) {
    Item* c = sc.item(cid); for (int& x : c->anchors) if (x == a) x = nid;
    auto o = c->ovr.find(a); if (o != c->ovr.end()) { HandleOverride h = o->second; c->ovr.erase(a); c->ovr[nid] = h; }
  }
  sel.anchors = {nid}; sel.items.clear();
  sc.reindex();
  return QObject::tr("%1 detached from the connector").arg(users.size() == 1 ? sc.item(users[0])->name : QObject::tr("%1 curves").arg(users.size()));
}

bool attachable(const Scene& sc, int a) {
  const Anchor* A = sc.anchor(a); if (!A || A->raw.value("at").isObject() || edit::isRingAnchor(sc, a) || tube::profiledAt(sc, a)) return false;   // (a curve with a profile modifier: never onto a connector)
  for (int c : curvesWith(sc, a)) { const Item* x = sc.item(c); if (x->conn.isEmpty() && (x->anchors.front() == a || x->anchors.back() == a)) return true; }
  return false;
}

}  // namespace hm::crown
