#include "scene/Walls.h"
#include "scene/CurveMath.h"
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace hm {

namespace {

const double PI = 3.14159265358979323846;
double ease(double s) { return s * s * (3 - 2 * s); }
double angleTo(const Vec3& a, const Vec3& b) {
  double d = std::sqrt(a.lengthSq() * b.lengthSq());
  return d == 0 ? PI / 2 : std::acos(clamp(a.dot(b) / d, -1, 1));
}

const CurveSampling* sampling(const CurveCache& cc, int id) {
  auto it = cc.find(id);
  return it == cc.end() || it->second.p.size() < 2 ? nullptr : &it->second;
}

const Item* parentGroup(const Scene& sc, const Item& it) {
  for (const Item* p = it.parent >= 0 ? sc.item(it.parent) : nullptr; p; p = p->parent >= 0 ? sc.item(p->parent) : nullptr)
    if (p->type == ItemType::Group) return p;
  return nullptr;
}

// pointOnCurve: the point at arc fraction t
std::optional<Vec3> pointOnCurve(const CurveSampling* s, double t) {
  if (!s) return std::nullopt;
  const auto& L = s->arc; double d = clamp(t, 0, 1) * L.back();
  size_t i = 0; while (i < L.size() - 2 && L[i + 1] < d) i++;
  double g = L[i + 1] - L[i];
  return s->p[i].lerp(s->p[i + 1], g > 0 ? clamp((d - L[i]) / g, 0, 1) : 0);
}

// arc fraction of anchor a on connector h (attached → its t; one of h's anchors → where it sits on h)
double connT(const Scene& sc, const CurveCache& cc, const Item& h, int a) {
  const Anchor* an = sc.anchor(a);
  if (an) { QJsonObject at = an->raw.value("at").toObject(); if (!at.isEmpty() && at.value("c").toInt() == h.id) return at.value("t").toDouble(); }
  int i = -1; for (size_t k = 0; k < h.anchors.size(); k++) if (h.anchors[k] == a) { i = int(k); break; }
  if (i < 0) return 0;
  const CurveSampling* s = sampling(cc, h.id);
  if (s && !s->arc.empty()) return s->arc[std::min<size_t>(size_t(i) * 24, s->arc.size() - 1)] / (s->arc.back() ? s->arc.back() : 1);
  return double(i) / std::max<size_t>(h.anchors.size() - 1, 1);
}

struct CrownRoot { const Item* low; int ea, eb; double ua, ub; };
// a wall whose curves both end on a crown's lower connector (attached, or sharing its anchor)
std::optional<CrownRoot> wallCrownRoot(const Scene& sc, const CurveCache& cc, const Item& sf) {
  const Item *ca = sc.item(sf.a), *cb = sc.item(sf.b);
  if (!ca || !cb || ca->type != ItemType::Curve || cb->type != ItemType::Curve || ca->anchors.empty() || cb->anchors.empty()) return std::nullopt;
  for (auto& [kid, k] : sc.items) {
    if (k.type != ItemType::Crown) continue;
    const Item* low = sc.item(k.raw.value("low").toInt(-1));
    if (!low || !sampling(cc, low->id)) continue;
    auto onLow = [&](int a) {
      const Anchor* an = sc.anchor(a); if (!an) return false;
      QJsonObject at = an->raw.value("at").toObject();
      if (!at.isEmpty() && at.value("c").toInt() == low->id) return true;
      return std::find(low->anchors.begin(), low->anchors.end(), a) != low->anchors.end();
    };
    auto end = [&](const Item* c) { for (int a : {c->anchors.front(), c->anchors.back()}) if (onLow(a)) return a; return -1; };
    int ea = end(ca), eb = end(cb);
    if (ea >= 0 && eb >= 0 && ea != eb) return CrownRoot{low, ea, eb, connT(sc, cc, *low, ea), connT(sc, cc, *low, eb)};
  }
  return std::nullopt;
}

// Where segment line s (0 … 1 across the wall) goes: p.segSpread pulls the lines to the middle / edges, p.segOff shifts them
double segPos(double s, const QJsonObject& p, int segs) {
  if (segs < 2 || s <= 0 || s >= 1) return s;
  double sp = segs >= 3 ? clamp(p.value("segSpread").toDouble(0), -1, 1) : 0, off = clamp(p.value("segOff").toDouble(0), -1, 1);
  if (sp != 0) { double u = s * 2 - 1, g = std::pow(2, -sp * 1.5); s = ((u > 0) - (u < 0)) * std::pow(std::abs(u), g); s = (s + 1) / 2; }
  if (off != 0) { double m = 0.5 + 0.4 * off, k = m / (1 - m); s = k * s / (1 + (k - 1) * s); }
  return s;
}

// lockCurvesFrom([surface]): the curves joined to it through walls (edge curves count as the curves under them)
std::vector<const Item*> lockCurves(const Scene& sc, const Item& sf, const std::vector<const Item*>& surfaces) {
  std::set<int> ids{sf.a, sf.b};
  auto pathOf = [](const Item& c) { std::vector<int> out; for (auto& s : c.path) out.push_back(s[0]); return out; };
  for (auto& [id, c] : sc.items) if (c.type == ItemType::Curve && c.hasPath()) for (int p : pathOf(c)) if (ids.count(p)) { ids.insert(id); break; }
  for (bool grew = true; grew;) {
    grew = false;
    for (const Item* s : surfaces) if (ids.count(s->a) != ids.count(s->b)) { ids.insert(s->a); ids.insert(s->b); grew = true; }
  }
  for (int id : std::vector<int>(ids.begin(), ids.end())) {
    const Item* c = sc.item(id);
    if (c && c->hasPath()) { for (int p : pathOf(*c)) ids.insert(p); ids.erase(id); }
  }
  std::vector<const Item*> out;
  for (int id : ids) { const Item* c = sc.item(id); if (c && c->type == ItemType::Curve && !c->hasPath()) out.push_back(c); }
  return out;
}

struct Rows { std::vector<Vec3> A, B; std::vector<Vec3> cent; int n = 2; };

// wallShapeRows: adaptive rows — one where either curve turns by 6° or where 1.2 loop steps have gone by (never closer
// than 0.15 step), both curves by length fraction; the lock's centre at each row tells which side is outward
std::optional<Rows> wallShapeRows(const Scene& sc, const CurveCache& cc, const Item& sf, const std::vector<const Item*>& surfaces,
                                  std::map<std::string, std::vector<Vec3>>& lockCent, bool draft) {
  const CurveSampling *A = sampling(cc, sf.a), *B = sampling(cc, sf.b);
  if (!A || !B) return std::nullopt;
  auto even = [](const CurveSampling& c, int n) {
    const auto& L = c.arc; double T = L.back(); std::vector<Vec3> o; size_t i = 1;
    for (int q = 0; q <= n; q++) {
      double t = T * q / n; while (i < L.size() - 1 && L[i] < t) i++;
      double g = L[i] - L[i - 1]; o.push_back(c.p[i - 1].lerp(c.p[i], clamp((t - L[i - 1]) / (g ? g : 1), 0, 1)));
    }
    return o;
  };
  double h = loopStep(sc, sf), len = std::max(A->arc.back(), B->arc.back());
  // (while anchors are dragged — draft — a coarser search: the walls are built right once the drag ends)
  int fine = int(clamp(std::ceil(len / ((draft ? 0.3 : 0.1) * h)), 16, 1200));
  // (two edges of a converted modifier — para: anchor k of each is one cross-section — are paired anchor by anchor, not by
  // length fraction: in a curl the inner edge is much shorter than the outer, and by length the rows would slant across
  // the turns, the wall caving in)
  auto byAnchors = [&](const CurveSampling& c, int nA, int n) {
    std::vector<double> at(size_t(nA), c.arc.back()); at[0] = 0;
    for (size_t i = 0; i < c.seg.size() && i < c.arc.size(); i++) { const int k = c.seg[i]; if (k > 0 && k < nA && at[size_t(k)] >= c.arc.back() && c.arc[i] < at[size_t(k)]) at[size_t(k)] = c.arc[i]; }
    for (int k = 1; k < nA; k++) at[size_t(k)] = std::max(at[size_t(k)], at[size_t(k - 1)]);
    const auto& L = c.arc; std::vector<Vec3> o; size_t i = 1;
    for (int q = 0; q <= n; q++) {
      const double u = double(q) / n * (nA - 1); const int k = std::min(int(std::floor(u)), nA - 2);
      const double t = at[size_t(k)] + (u - k) * (at[size_t(k + 1)] - at[size_t(k)]);
      while (i < L.size() - 1 && L[i] < t) i++;
      double g = L[i] - L[i - 1]; o.push_back(c.p[i - 1].lerp(c.p[i], clamp((t - L[i - 1]) / (g ? g : 1), 0, 1)));
    }
    return o;
  };
  const Item *ca = sc.item(sf.a), *cb = sc.item(sf.b);
  const bool para = ca && cb && ca->para && cb->para && ca->anchors.size() == cb->anchors.size() && ca->anchors.size() >= 2 && !A->seg.empty() && !B->seg.empty();
  auto PA = para ? byAnchors(*A, int(ca->anchors.size()), fine) : even(*A, fine), PB = para ? byAnchors(*B, int(cb->anchors.size()), fine) : even(*B, fine);
  if (PA[0].distanceTo(PB[0]) + PA[fine].distanceTo(PB[fine]) > PA[0].distanceTo(PB[fine]) + PA[fine].distanceTo(PB[0])) std::reverse(PB.begin(), PB.end());
  auto tan = [&](const std::vector<Vec3>& P, int q) { return (P[std::min(q + 1, fine)] - P[std::max(q - 1, 0)]).normalized(); };
  double maxD = 1.2 * h, minD = 0.15 * h, turn = 6 * PI / 180;
  std::vector<int> qs{0}; int lq = 0;
  for (int q = 1; q < fine; q++) {
    double d = std::max(PA[q].distanceTo(PA[lq]), PB[q].distanceTo(PB[lq]));
    double tw = std::max(angleTo(tan(PA, q), tan(PA, lq)), angleTo(tan(PB, q), tan(PB, lq)));
    if (d >= maxD || (tw >= turn && d >= minD)) { qs.push_back(q); lq = q; }
  }
  qs.push_back(fine);

  // the lock's centre line: 200 points, each curve of the lock root (higher end) first, averaged
  auto lock = lockCurves(sc, sf, surfaces);
  std::vector<const CurveSampling*> ls; std::string key;
  for (const Item* c : lock) if (auto* s = sampling(cc, c->id)) { ls.push_back(s); key += std::to_string(c->id) + ","; }
  Rows r; r.n = std::max(int(ls.size()), 2);
  auto ci = lockCent.find(key);
  if (ci == lockCent.end()) {
    std::vector<Vec3> CL;
    if (!ls.empty()) {
      const int NS = 200; CL.assign(NS + 1, Vec3{});
      for (auto* s : ls) { auto P = even(*s, NS); if (P[0].y < P[NS].y) std::reverse(P.begin(), P.end()); for (int k = 0; k <= NS; k++) CL[k] += P[k]; }
      for (auto& v : CL) v = v / double(ls.size());
    }
    ci = lockCent.emplace(key, std::move(CL)).first;
  }
  const auto& CL = ci->second;
  bool rev = !CL.empty() && CL[0].distanceTo(PA[0]) + CL[200].distanceTo(PA[fine]) > CL[0].distanceTo(PA[fine]) + CL[200].distanceTo(PA[0]);
  auto atF = [&](double f) { double x = clamp(rev ? 1 - f : f, 0, 1) * 200; int i = std::min(199, int(std::floor(x))); return CL[i].lerp(CL[i + 1], x - i); };
  for (int q : qs) {
    r.A.push_back(PA[q]); r.B.push_back(PB[q]);
    r.cent.push_back(!CL.empty() ? atF(double(q) / fine) : (PA[q] + PB[q]) * 0.5);
  }
  return r;
}

struct WallParams { int segs = 1; double bulge = 0; bool flip = false; QJsonObject p; bool twin = false; };

// surfaceData: a grid between rows A (column 0) and B (the last column), bulged along the outward normal
void surfaceData(const Scene& sc, const CurveCache& cc, const Item& sf, const Rows& rw, const WallParams& wp,
                 const Vec3& sceneCenter, double scale, WallMesh& out) {
  const auto& A = wp.twin ? rw.B : rw.A; const auto& B = wp.twin ? rw.A : rw.B;
  int R = int(A.size()), segs = std::max(1, wp.segs), M = segs + 1;
  std::vector<Vec3> nr(R); std::vector<double> wd(R); Vec3 prev; bool havePrev = false;
  for (int k = 0; k < R; k++) {
    Vec3 w = B[k] - A[k]; wd[k] = w.length();
    int k0 = std::max(k - 1, 0), k1 = std::min(k + 1, R - 1);
    Vec3 t = A[k1] + B[k1] - A[k0] - B[k0], n = t.cross(w);
    if (n.lengthSq() < 1e-16) n = havePrev ? prev : Vec3(0, 0, 1);
    n = n.normalized(); prev = n; havePrev = true; nr[k] = n;
  }
  // "outward" decided once, on the widest row, then kept continuous along the wall
  int kr = 0; for (int k = 0; k < R; k++) if (wd[k] > wd[kr]) kr = k;
  Vec3 midR = (A[kr] + B[kr]) * 0.5, outw = midR - rw.cent[kr];
  if (rw.n <= 2 || outw.lengthSq() < 1e-8 * scale * scale) outw = midR - sceneCenter;
  if (nr[kr].dot(outw) < 0) nr[kr] = -nr[kr];
  for (int k = kr + 1; k < R; k++) if (nr[k].dot(nr[k - 1]) < 0) nr[k] = -nr[k];
  for (int k = kr - 1; k >= 0; k--) if (nr[k].dot(nr[k + 1]) < 0) nr[k] = -nr[k];
  if (wp.flip) for (auto& n : nr) n = -n;

  // rooted on a crown's lower connector: the bulge fades in from the root (p.bfade), the root row lies on the connector
  std::function<double(int)> fade = [](int) { return 1.0; };
  std::vector<Vec3> delta;
  Item sfA = sf; if (wp.twin) std::swap(sfA.a, sfA.b);
  if (auto cr = wallCrownRoot(sc, cc, sfA)) {
    const Anchor* ea = sc.anchor(cr->ea);
    int kRoot = ea && A[0].distanceTo(ea->pos) <= A[R - 1].distanceTo(ea->pos) ? 0 : R - 1;
    double Lf = std::max(1.0, (R - 1) * clamp(wp.p.value("bfade").toDouble(0.2), 0.02, 1));
    fade = [kRoot, Lf](int k) { return ease(clamp(std::abs(k - kRoot) / Lf, 0, 1)); };
    if (!wp.twin) {
      Vec3 w0 = B[kRoot] - A[kRoot];
      const CurveSampling* lowS = sampling(cc, cr->low->id);
      for (int j = 0; j < M; j++) {
        double s = segPos(double(j) / segs, wp.p, segs);
        auto p = pointOnCurve(lowS, cr->ua + (cr->ub - cr->ua) * s);
        delta.push_back(p ? *p - (A[kRoot] + w0 * s) : Vec3{});
      }
    }
  }
  // a junction wall (p.jfade): the bulge fades out where the wall closes to zero width
  if (wp.p.contains("jfade") && !wp.p.value("jfade").isNull() && R > 1) {
    double mx = *std::max_element(wd.begin(), wd.end()), Lf = std::max(1.0, (R - 1) * clamp(wp.p.value("jfade").toDouble(), 0.02, 1));
    std::vector<double> dz(R, 1e300); double last = -1e300;
    for (int k = 0; k < R; k++) { if (wd[k] < 0.03 * mx) last = k; dz[k] = k - last; }
    last = 1e300; for (int k = R - 1; k >= 0; k--) { if (wd[k] < 0.03 * mx) last = k; dz[k] = std::min(dz[k], last - k); }
    auto f0 = fade; fade = [f0, dz, Lf](int k) { return f0(k) * ease(clamp(dz[k] / Lf, 0, 1)); };
  }

  int base = int(out.pos.size()), rowBase = out.vrow.empty() ? 0 : out.vrow.back() + 1;
  out.grids.push_back({base, R, M, wp.twin});
  for (int k = 0; k < R; k++) {
    Vec3 w = B[k] - A[k]; double f = fade(k);
    for (int j = 0; j < M; j++) {
      double s = segPos(double(j) / segs, wp.p, segs);
      Vec3 q = A[k] + w * s + nr[k] * (wp.bulge * f * wd[k] * std::sin(PI * s));
      if (!delta.empty()) q += delta[j] * (1 - f);
      out.pos.push_back(q); out.vrow.push_back(rowBase + k);
    }
  }
  std::vector<std::array<int, 4>> faces; double vote = 0;
  for (int k = 0; k < R - 1; k++) for (int j = 0; j < segs; j++) {
    std::array<int, 4> q{k * M + j, k * M + j + 1, (k + 1) * M + j + 1, (k + 1) * M + j};
    const auto& P = out.pos;
    Vec3 fn = (P[base + q[2]] - P[base + q[0]]).cross(P[base + q[3]] - P[base + q[1]]);
    double d = fn.dot(nr[k]); vote += (d > 0) - (d < 0);
    faces.push_back(q);
  }
  for (auto& q : faces) {
    if (vote < 0) std::reverse(q.begin(), q.end());        // one consistent outward winding per wall
    for (int& v : q) v += base;
    out.quads.push_back(q);
  }
}

}  // namespace

CurveSampling sampleCurveFull(const Scene& sc, const Item& c) {
  CurveSampling s;
  for (auto& x : sampleCurve(sc, c, 24)) { s.p.push_back(x.p); s.seg.push_back(x.seg); }
  s.arc.assign(s.p.size(), 0);
  for (size_t i = 1; i < s.p.size(); i++) s.arc[i] = s.arc[i - 1] + s.p[i].distanceTo(s.p[i - 1]);
  return s;
}

CurveCache sampleAllCurves(const Scene& sc) {
  CurveCache cc;
  for (const Item* c : sc.curves()) if (c->anchors.size() >= 2) cc.emplace(c->id, sampleCurveFull(sc, *c));
  return cc;
}

std::vector<uint32_t> polyFlatIndex(const std::vector<std::vector<int>>& faces, int nv) {
  std::vector<int> own(nv, -1), pv(faces.size(), -1);
  std::function<bool(int, int, std::set<int>*)> grab = [&](int f, int depth, std::set<int>* seen) {
    for (int v : faces[f]) if (own[v] < 0) { own[v] = f; pv[f] = v; return true; }
    if (depth <= 0) return false;
    for (int v : faces[f]) {
      if (seen->count(v)) continue; seen->insert(v);
      if (grab(own[v], depth - 1, seen)) { own[v] = f; pv[f] = v; return true; }
    }
    return false;
  };
  for (int f = 0; f < int(faces.size()); f++) grab(f, 0, nullptr);
  for (int f = 0; f < int(faces.size()); f++) if (pv[f] < 0) { std::set<int> seen; grab(f, 8, &seen); }
  std::vector<uint32_t> idx;
  for (size_t i = 0; i < faces.size(); i++) {
    const auto& f = faces[i]; int n = int(f.size()), c = 0;
    if (pv[i] >= 0) c = int(std::find(f.begin(), f.end(), pv[i]) - f.begin());
    for (int k = 1; k < n - 1; k++) { idx.push_back(uint32_t(f[(c + k) % n])); idx.push_back(uint32_t(f[(c + k + 1) % n])); idx.push_back(uint32_t(f[c])); }
  }
  return idx;
}

// usedStep: the step most walls' groups use
double usedStep(const Scene& sc) {
  std::map<double, int> w;
  for (auto& [id, s] : sc.items) {
    if (s.type != ItemType::Surface) continue;
    for (const Item* p = s.parent >= 0 ? sc.item(s.parent) : nullptr; p; p = p->parent >= 0 ? sc.item(p->parent) : nullptr) {
      double v = p->type == ItemType::Group ? p->raw.value("conv").toObject().value("step").toDouble(0) : 0;
      if (v > 0) { w[std::stod(QString::number(v, 'g', 6).toStdString())]++; break; }
    }
  }
  double best = 0; int bn = 0; for (auto& [k, n] : w) if (n > bn) { bn = n; best = k; }
  return best;
}

// defaultStep: S.step as loadProject leaves it (settings.step — replaced by the used step when they differ by over 2 %),
// else the used step, else the head's height / 24
double projectStep(const Scene& sc) {
  double st = sc.fileRaw.value("settings").toObject().value("step").toDouble(0), u = usedStep(sc);
  if (u > 0 && (!(st > 0) || std::abs(st / u - 1) > 0.02)) st = u;
  double headH = 0;
  for (auto& [id, it] : sc.items) if ((it.headTag || it.type == ItemType::Head) && it.poly) headH = std::max(headH, it.poly->bounds().size().y);
  if (st > 0 && !(headH > 0 && st < headH / 1000)) return st;   // (a step thousands of times finer than the head — left from before the head was known — not taken)
  return (headH > 0 ? headH : 1) / 24;
}

double loopStep(const Scene& sc, const Item& it) {
  for (const Item* p = it.parent >= 0 ? sc.item(it.parent) : nullptr; p; p = p->parent >= 0 ? sc.item(p->parent) : nullptr) {
    bool cocoonConv = p->type == ItemType::Cocoon && p->raw.value("conv").isObject();
    if (p->type == ItemType::Group || cocoonConv) {
      double v = p->raw.value("conv").toObject().value("step").toDouble(0);
      return v > 0 ? v : projectStep(sc);
    }
  }
  return projectStep(sc);
}

std::vector<WallMesh> buildWalls(const Scene& sc, const CurveCache& cc, bool includeHidden, const std::set<int>* only, bool draft) {
  std::vector<const Item*> surfaces;
  for (auto& [id, it] : sc.items) if (it.type == ItemType::Surface) surfaces.push_back(&it);
  std::sort(surfaces.begin(), surfaces.end(), [](const Item* a, const Item* b) { return a->order < b->order; });
  Box3 bb = sc.bounds(); Vec3 center = bb.empty() ? Vec3{} : bb.center();
  double scale = std::max({bb.size().x, bb.size().y, bb.size().z}); if (!(scale > 0)) scale = 1;
  std::map<std::string, std::vector<Vec3>> lockCent;              // (LOCK_CENT: one centre line per lock and build)
  std::vector<WallMesh> out;
  for (const Item* sf : surfaces) {
    if (!includeHidden && !sc.isVisible(*sf)) continue;
    if (only && !only->count(sf->id)) continue;
    auto rows = wallShapeRows(sc, cc, *sf, surfaces, lockCent, draft);
    if (!rows) continue;
    QJsonObject p = sf->raw.value("p").toObject();
    WallMesh m; m.surfaceId = sf->id;
    WallParams wp{std::max(1, p.value("segs").toInt(1)), p.value("bulge").toDouble(0), p.value("flip").toBool(), p, false};
    surfaceData(sc, cc, *sf, *rows, wp, center, scale, m);
    if (p.value("b2on").toBool() && wp.bulge != 0) {         // the twin wall: back from b to a
      WallParams tw{std::max(1, p.value("b2segs").toInt(1)), p.contains("b2bulge") && !p.value("b2bulge").isNull() ? p.value("b2bulge").toDouble() : wp.bulge,
                    p.contains("b2flip") && !p.value("b2flip").isNull() ? p.value("b2flip").toBool() : !wp.flip, QJsonObject{{"bfade", p.value("bfade")}}, true};
      surfaceData(sc, cc, *sf, *rows, tw, center, scale, m);
    }
    if (!m.quads.empty()) out.push_back(std::move(m));
  }
  return out;
}

}  // namespace hm
