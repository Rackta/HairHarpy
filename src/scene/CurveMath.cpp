#include "scene/CurveMath.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hm {

namespace {

const Vec3& P(const Scene& sc, int anchorId) {
  static const Vec3 zero;
  const Anchor* a = sc.anchor(anchorId);
  return a ? a->pos : zero;
}

int indexOf(const std::vector<int>& A, int a) {
  for (size_t i = 0; i < A.size(); i++) if (A[i] == a) return int(i);
  return -1;
}

const std::vector<int>& curvesAt(const Scene& sc, int a) {
  static const std::vector<int> none;
  auto it = sc.curvesAt.find(a);
  return it == sc.curvesAt.end() ? none : it->second;
}

// Bessel tangent: the direction of the parabola through 3 anchors (the shorter chord weighs more)
Vec3 autoTangent3(const Vec3& a, const Vec3& b, const Vec3& c) {
  Vec3 d0 = b - a, d1 = c - b;
  double h0 = d0.length(), h1 = d1.length();
  if (h0 < 1e-9) return d1.normalized();
  if (h1 < 1e-9) return d0.normalized();
  Vec3 t = d0 * (h1 / h0) + d1 * (h0 / h1);
  return t.lengthSq() < 1e-14 ? (c - a).normalized() : t.normalized();
}
Vec3 autoTangent(const Scene& sc, const std::vector<int>& A, int i) { return autoTangent3(P(sc, A[i - 1]), P(sc, A[i]), P(sc, A[i + 1])); }

std::optional<Vec3> junctionTangent(const Scene& sc, const Item& c, int a) {
  for (int hid : curvesAt(sc, a)) {
    const Item* h = sc.item(hid);
    if (!h || h->id == c.id || !h->conn.isEmpty() || h->junc) continue;   // curves ending on a crown connector leave it freely
    int k = indexOf(h->anchors, a);
    if (k > 0 && k < int(h->anchors.size()) - 1) return autoTangent(sc, h->anchors, k);
  }
  return std::nullopt;
}


// New logic (c.seg): curves cut at the forks meet only at their ends; every piece takes one common tangent there
std::optional<Vec3> nodeTangent(const Scene& sc, const Item& c, int a) {
  if (!c.seg) return std::nullopt;
  std::vector<int> ns;                     // neighbour anchor ids, first-seen order (a JS Map)
  for (int hid : curvesAt(sc, a)) {
    const Item* h = sc.item(hid);
    if (!h || !segNet(*h)) continue;
    const auto& A = h->anchors; int k = indexOf(A, a); if (k < 0) continue;
    if (k > 0 && k < int(A.size()) - 1) return std::nullopt;
    if (A.size() < 2) continue;
    int n = A[k == 0 ? 1 : A.size() - 2];
    if (indexOf(ns, n) < 0) ns.push_back(n);
  }
  if (ns.size() < 2) return std::nullopt;
  const Vec3& ap = P(sc, a);
  std::vector<Vec3> d; for (int n : ns) d.push_back((P(sc, n) - ap).normalized());
  int own = c.anchors[0] == a ? c.anchors[1] : c.anchors[c.anchors.size() - 2], oi = indexOf(ns, own);
  if (oi < 0) return std::nullopt;
  auto opp = [&](int i) { int b = -1; double v = std::numeric_limits<double>::infinity();
    for (int j = 0; j < int(d.size()); j++) if (j != i && d[j].dot(d[i]) < v) { v = d[j].dot(d[i]); b = j; } return b; };
  auto thru = [&](int i, int j) { return autoTangent3(P(sc, ns[i]), ap, P(sc, ns[j])); };
  int N = int(ns.size());
  if (N >= 4 && N % 2 == 0) {
    bool pairs = true;
    for (int i = 0; i < N && pairs; i++) { int j = opp(i); pairs = j >= 0 && opp(j) == i && d[i].dot(d[j]) < -0.5; }
    if (pairs) return thru(opp(oi), oi);
  }
  int t = 0; double best = std::numeric_limits<double>::infinity();
  for (int i = 0; i < N; i++) { double s = 0; for (int j = 0; j < N; j++) if (j != i) s += d[i].dot(d[j]); if (s < best) { best = s; t = i; } }
  std::vector<int> br; for (int j = 0; j < N; j++) if (j != t && d[j].dot(d[t]) < 0) br.push_back(j);
  if (br.empty() || (oi != t && indexOf(br, oi) < 0)) return std::nullopt;
  Vec3 T; for (int j : br) T += thru(t, j);
  return T.lengthSq() > 1e-12 ? std::optional<Vec3>(T.normalized()) : std::nullopt;
}

// welded trunks share one tangent at every anchor of the common run
Vec3 sharedTangent(const Scene& sc, const Item& c, int i) {
  const auto& A = c.anchors; int a = A[i];
  Vec3 own = autoTangent(sc, A, i), sum = own; int n = 1;
  for (int hid : curvesAt(sc, a)) {
    const Item* h = sc.item(hid);
    if (!h || h->id == c.id || h->junc) continue;
    int k = indexOf(h->anchors, a); if (k <= 0 || k >= int(h->anchors.size()) - 1) continue;
    int hp = h->anchors[k - 1], hn = h->anchors[k + 1], s = 0;
    if (hp == A[i - 1] || hn == A[i + 1]) s = 1; else if (hp == A[i + 1] || hn == A[i - 1]) s = -1;
    if (!s) continue;
    sum += autoTangent(sc, h->anchors, k) * double(s); n++;
  }
  return n > 1 && sum.lengthSq() > 1e-12 ? sum.normalized() : own;
}

struct Circle { Vec3 o, n; double r; };
std::optional<Circle> circleThrough(const Vec3& a, const Vec3& b, const Vec3& c) {
  Vec3 ab = b - a, ac = c - a, n = ab.cross(ac); double n2 = n.lengthSq();
  if (n2 < 1e-10 * ab.lengthSq() * ac.lengthSq()) return std::nullopt;
  Vec3 o = (n.cross(ab) * ac.lengthSq() + ac.cross(n) * ab.lengthSq()) / (2 * n2) + a;
  return Circle{o, n.normalized(), o.distanceTo(a)};
}
double arcHandle(double d, double r) { double th = 2 * std::asin(clamp(d / (2 * r), 0, 1)); return 4.0 / 3.0 * std::tan(th / 4) * r; }

bool sharedReal(const Scene& sc, const Item& c, int a) {
  for (int hid : curvesAt(sc, a)) { const Item* h = sc.item(hid); if (h && h->id != c.id && !h->junc) return true; }
  return false;
}

struct RoundOut { Vec3 dir; double lenIn, lenOut; };
std::optional<RoundOut> roundAt(const Scene& sc, const Item& c, int i, const Vec3& dir, double lenIn, double lenOut) {
  const auto& A = c.anchors; int n = int(A.size()); double k = c.round;
  if (!(k > 0) || c.closed || n < 3 || sharedReal(sc, c, A[i])) return std::nullopt;
  int j = std::clamp(i, 1, n - 2);
  auto C = circleThrough(P(sc, A[j - 1]), P(sc, A[j]), P(sc, A[j + 1])); if (!C) return std::nullopt;
  const Vec3& Pi = P(sc, A[i]);
  Vec3 t = C->n.cross(Pi - C->o).normalized();
  if (t.dot(P(sc, A[j + 1]) - P(sc, A[j - 1])) < 0) t = -t;
  return RoundOut{dir.lerp(t, k).normalized(),
                  i > 0 ? lenIn + (arcHandle(Pi.distanceTo(P(sc, A[i - 1])), C->r) - lenIn) * k : lenIn,
                  i < n - 1 ? lenOut + (arcHandle(Pi.distanceTo(P(sc, A[i + 1])), C->r) - lenOut) * k : lenOut};
}

// natural C2 cubic spline through the anchors (chord-length parameters): its derivatives D and the chords h
struct Fair { std::vector<Vec3> D; std::vector<double> h; };
Fair fairSpline(const Scene& sc, const Item& c) {
  const auto& A = c.anchors; int n = int(A.size());
  Fair F; F.h.resize(n - 1);
  for (int i = 0; i < n - 1; i++) F.h[i] = std::max(P(sc, A[i]).distanceTo(P(sc, A[i + 1])), 1e-9);
  std::vector<Vec3> d(n - 1), r(n); std::vector<double> a(n), b(n), cc(n);
  for (int i = 0; i < n - 1; i++) d[i] = (P(sc, A[i + 1]) - P(sc, A[i])) / F.h[i];
  for (int i = 0; i < n; i++) {
    if (i == 0) { a[i] = 0; b[i] = 2; cc[i] = 1; r[i] = d[0] * 3.0; }
    else if (i == n - 1) { a[i] = 1; b[i] = 2; cc[i] = 0; r[i] = d[n - 2] * 3.0; }
    else { a[i] = F.h[i]; b[i] = 2 * (F.h[i - 1] + F.h[i]); cc[i] = F.h[i - 1]; r[i] = d[i - 1] * (3 * F.h[i]) + d[i] * (3 * F.h[i - 1]); }
  }
  for (int i = 1; i < n; i++) { double w = a[i] / b[i - 1]; b[i] -= w * cc[i - 1]; r[i] -= r[i - 1] * w; }   // Thomas algorithm
  F.D.resize(n); F.D[n - 1] = r[n - 1] / b[n - 1];
  for (int i = n - 2; i >= 0; i--) F.D[i] = (r[i] - F.D[i + 1] * cc[i]) / b[i];
  return F;
}

// every anchor next to `a` on any (non-ring) curve, first-seen order
std::vector<int> junctionNeighbours(const Scene& sc, int a) {
  std::vector<int> m;
  auto add = [&](int x) { if (indexOf(m, x) < 0) m.push_back(x); };
  for (int hid : curvesAt(sc, a)) {
    const Item* h = sc.item(hid); if (!h || h->ring >= 0) continue;
    const auto& A = h->anchors; int i = indexOf(A, a); if (i < 0) continue;
    if (i > 0) add(A[i - 1]);
    if (i < int(A.size()) - 1) add(A[i + 1]);
  }
  return m;
}

// rotated junction (anchor.pass): the directions of the in / out handles of c at anchor i
struct PassDir { Vec3 d; bool br; };
struct PassRule { std::optional<PassDir> din, dout; };
std::optional<PassRule> passRule(const Scene& sc, const Item& c, int i) {
  const auto& A = c.anchors; const Anchor* an = sc.anchor(A[i]);
  if (!an || !an->pass || c.closed || c.ring >= 0) return std::nullopt;
  auto nb = junctionNeighbours(sc, an->id);
  int p = an->pass->first, q = an->pass->second;
  if (indexOf(nb, p) < 0 || indexOf(nb, q) < 0) return std::nullopt;
  Vec3 T = autoTangent3(P(sc, p), an->pos, P(sc, q));
  auto toward = [&](int n) -> PassDir {
    if (n == q) return {T, false};
    if (n == p) return {-T, false};
    return {T.dot(P(sc, n) - an->pos) >= 0 ? T : -T, true};
  };
  PassRule r;
  if (i > 0) r.din = toward(A[i - 1]);
  if (i < int(A.size()) - 1) r.dout = toward(A[i + 1]);
  return r;
}

Handles juncHandles(const Scene& sc, const Item& c, int i) {
  const auto& A = c.anchors; const Vec3& Pi = P(sc, A[i]);
  // the control point next to `at` on the segment at → to of a non-junction curve that has it (either direction)
  auto ctrl = [&](int at, int to) -> std::optional<Vec3> {
    for (int hid : curvesAt(sc, at)) {
      const Item* h = sc.item(hid);
      if (!h || h->junc || h->id == c.id) continue;
      const auto& H = h->anchors;
      for (int k = 0; k + 1 < int(H.size()); k++) if (H[k] == at && H[k + 1] == to) return curveHandles(sc, *h, k).hout;
      for (int q = 0; q + 1 < int(H.size()); q++) if (H[q] == to && H[q + 1] == at) return curveHandles(sc, *h, q + 1).hin;
    }
    return std::nullopt;
  };
  Handles r{Pi, Pi};
  if (i > 0) { auto x = ctrl(A[i], A[i - 1]); r.hin = x ? *x : Pi.lerp(P(sc, A[i - 1]), 1.0 / 3); }
  if (i < int(A.size()) - 1) { auto x = ctrl(A[i], A[i + 1]); r.hout = x ? *x : Pi.lerp(P(sc, A[i + 1]), 1.0 / 3); }
  return r;
}

Handles handlesImpl(const Scene& sc, const Item& c, int i, const Fair* F) {
  if (c.junc) return juncHandles(sc, c, i);
  const auto& A = c.anchors; int n = int(A.size());
  const Anchor* an = sc.anchor(A[i]);
  const Vec3 Pi = an ? an->pos : Vec3{};
  const bool brk = std::find(c.sharp.begin(), c.sharp.end(), A[i]) != c.sharp.end();   // (this curve's own corner — the anchor's other curves untouched)
  const double sm = brk ? 0 : an ? an->smooth : 1;
  auto oit = c.ovr.find(A[i]);
  const HandleOverride* o = oit == c.ovr.end() ? nullptr : &oit->second;
  double lenIn = i > 0 ? Pi.distanceTo(P(sc, A[i - 1])) / 3 : 0;
  double lenOut = i < n - 1 ? Pi.distanceTo(P(sc, A[i + 1])) / 3 : 0;
  if (auto pr = passRule(sc, c, i)) {
    auto h = [&](const std::optional<PassDir>& r, double len) { return r ? Pi + r->d * (len * sm * (r->br ? 1.35 : 1)) : Pi; };
    return {(o && o->in) ? Pi + *o->in * sm : h(pr->din, lenIn), (o && o->out) ? Pi + *o->out * sm : h(pr->dout, lenOut)};
  }
  Vec3 dir;
  if (c.closed) {                                                  // ring: neighbours wrap around
    const Vec3 &a0 = P(sc, A[(i + n - 1) % n]), &a2 = P(sc, A[(i + 1) % n]);
    dir = autoTangent3(a0, Pi, a2); lenIn = Pi.distanceTo(a0) / 3; lenOut = Pi.distanceTo(a2) / 3;
  } else if (i > 0 && i < n - 1) {
    dir = sharedTangent(sc, c, i);
    if (auto r = roundAt(sc, c, i, dir, lenIn, lenOut)) { dir = r->dir; lenIn = r->lenIn; lenOut = r->lenOut; }
  } else {
    int k = i == 0 ? 1 : n - 2;
    Vec3 b = (P(sc, A[k]) - Pi).normalized();                     // from the end toward its neighbour
    auto j = junctionTangent(sc, c, A[i]);
    auto nt = j ? std::nullopt : nodeTangent(sc, c, A[i]);
    if (nt) { Vec3 d = *nt * (b.dot(*nt) >= 0 ? 1.0 : -1.0); dir = i == 0 ? d : -d; }
    else if (j) {
      // Y-junction: our branch leaves along the host branch with the smaller angle (sharp crotch)
      Vec3 d = *j * (b.dot(*j) >= 0 ? 1.0 : -1.0);
      dir = i == 0 ? d : -d;
      if (i == 0) lenOut *= 1.35; else lenIn *= 1.35;
    } else {
      // natural end: aim at the neighbour's facing handle → the end segment is a smooth arc
      Vec3 aim = b;
      if (n > 2) {
        Handles hk = handlesImpl(sc, c, k, F);
        Vec3 t = (i == 0 ? hk.hin : hk.hout) - Pi;
        if (t.lengthSq() > 1e-14) aim = t.normalized();
      }
      dir = i == 0 ? aim : -aim;
      if (auto r = roundAt(sc, c, i, dir, lenIn, lenOut)) { dir = r->dir; lenIn = r->lenIn; lenOut = r->lenOut; }
    }
  }
  Vec3 hin = (o && o->in) ? Pi + *o->in * sm : Pi + dir * (-lenIn * sm);
  Vec3 hout = (o && o->out) ? Pi + *o->out * sm : Pi + dir * (lenOut * sm);
  // Smoothing (c.fair): the handles move towards those of the C2 spline through the same anchors
  double kf = c.fair;
  if (kf > 0 && !c.closed && n >= 3 && !sharedReal(sc, c, A[i])) {
    Fair local; if (!F) { local = fairSpline(sc, c); F = &local; }
    if (i > 0 && !(o && o->in)) hin = hin.lerp(Pi + F->D[i] * (-F->h[i - 1] / 3 * sm), kf);
    if (i < n - 1 && !(o && o->out)) hout = hout.lerp(Pi + F->D[i] * (F->h[i] / 3 * sm), kf);
  }
  return {hin, hout};
}

}  // namespace

Vec3 bez(const BezSeg& s, double t) {
  double u = 1 - t;
  return s[0] * (u * u * u) + s[1] * (3 * u * u * t) + s[2] * (3 * u * t * t) + s[3] * (t * t * t);
}

Handles curveHandles(const Scene& sc, const Item& c, int i) { return handlesImpl(sc, c, i, nullptr); }

std::vector<BezSeg> curveSegs(const Scene& sc, const Item& c) {
  const auto& A = c.anchors; int n = int(A.size());
  std::vector<BezSeg> segs; if (n < 2) return segs;
  Fair F; const Fair* Fp = nullptr;
  if (c.fair > 0 && !c.closed && n >= 3) { F = fairSpline(sc, c); Fp = &F; }
  std::vector<Handles> Hs(n);
  for (int i = 0; i < n; i++) Hs[i] = handlesImpl(sc, c, i, Fp);
  for (int i = 0; i < n - 1; i++) segs.push_back({P(sc, A[i]), Hs[i].hout, Hs[i + 1].hin, P(sc, A[i + 1])});
  if (c.closed && n > 2) segs.push_back({P(sc, A[n - 1]), Hs[n - 1].hout, Hs[0].hin, P(sc, A[0])});
  return segs;
}

std::vector<CurveSample> sampleCurve(const Scene& sc, const Item& c, int per) {
  std::vector<CurveSample> out; auto segs = curveSegs(sc, c);
  if (segs.empty()) { if (!c.anchors.empty()) out.push_back({P(sc, c.anchors[0]), 0}); return out; }
  for (int si = 0; si < int(segs.size()); si++) for (int k = 0; k < per; k++) out.push_back({bez(segs[si], double(k) / per), si});
  out.push_back({segs.back()[3], int(segs.size()) - 1});
  return out;
}

}  // namespace hm
