#include "paint/HairFlow.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <queue>

namespace hm::flow {

namespace {

constexpr double kPi = 3.14159265358979323846;

QJsonArray vec(const Vec3& p) { return QJsonArray{p.x, p.y, p.z}; }
Vec3 vecOf(const QJsonValue& v, const Vec3& d) { const QJsonArray a = v.toArray(); return a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : d; }

// the polygons' vertex normals (area-weighted, Newell)
std::vector<Vec3> vertexNormals(const PolyMesh& m) {
  std::vector<Vec3> n(m.pos.size());
  for (int f = 0; f < m.faceCount(); f++) {
    const int a = m.faceStart[size_t(f)], b = m.faceStart[size_t(f) + 1]; Vec3 fn;
    for (int k = a; k < b; k++) { const Vec3& p = m.pos[size_t(m.faceVerts[size_t(k)])]; const Vec3& q = m.pos[size_t(m.faceVerts[size_t(k + 1 < b ? k + 1 : a)])];
      fn += Vec3((p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x), (p.x - q.x) * (p.y + q.y)); }
    for (int k = a; k < b; k++) n[size_t(m.faceVerts[size_t(k)])] += fn;
  }
  for (auto& v : n) v = v.normalized();
  return n;
}

// a polyline prepared for projections: lengths, tangents and a frame carried along it without twisting
struct Line {
  std::vector<Vec3> p, t, e1;
  std::vector<double> s;          // arc length at each point
  double len = 0;
  bool build(std::vector<Vec3> pts, const Vec3& seam) {
    pts.erase(std::unique(pts.begin(), pts.end(), [](const Vec3& a, const Vec3& b) { return a.distanceTo(b) < 1e-12; }), pts.end());
    if (pts.size() < 2) return false;
    p = std::move(pts); const size_t n = p.size(); s.assign(n, 0); t.assign(n, {}); e1.assign(n, {});
    for (size_t i = 1; i < n; i++) s[i] = s[i - 1] + p[i].distanceTo(p[i - 1]);
    len = s.back(); if (len <= 0) return false;
    for (size_t i = 0; i < n; i++) t[i] = (p[std::min(i + 1, n - 1)] - p[i ? i - 1 : 0]).normalized();
    Vec3 e = seam - t[0] * seam.dot(t[0]);
    if (e.lengthSq() < 1e-10) { e = Vec3(0, 1, 0) - t[0] * t[0].y; if (e.lengthSq() < 1e-10) e = Vec3(1, 0, 0) - t[0] * t[0].x; }
    e1[0] = e.normalized();
    for (size_t i = 1; i < n; i++) { Vec3 q = e1[i - 1] - t[i] * e1[i - 1].dot(t[i]); e1[i] = q.lengthSq() > 1e-14 ? q.normalized() : e1[i - 1]; }
    return true;
  }
  // the nearest point: its arc length, place, tangent and frame there; returns the distance squared
  struct Hit { double s = 0, d2 = 1e300; Vec3 c, t, e1; };
  Hit project(const Vec3& q) const {
    Hit h;
    for (size_t i = 0; i + 1 < p.size(); i++) {
      const Vec3 ab = p[i + 1] - p[i]; const double l2 = ab.lengthSq(); double k = l2 > 0 ? (q - p[i]).dot(ab) / l2 : 0; k = std::clamp(k, 0.0, 1.0);
      const Vec3 c = p[i] + ab * k; const double d2 = (q - c).lengthSq();
      if (d2 < h.d2) { h.d2 = d2; h.s = s[i] + (s[i + 1] - s[i]) * k; h.c = c; h.t = t[i].lerp(t[i + 1], k).normalized(); h.e1 = e1[i].lerp(e1[i + 1], k); }
    }
    h.e1 = (h.e1 - h.t * h.e1.dot(h.t)).normalized();
    return h;
  }
};

double smooth01(double a, double b, double x) { const double k = std::clamp((x - a) / (b - a), 0.0, 1.0); return k * k * (3 - 2 * k); }

}  // namespace

// ---------------------------------------------------------------- JSON

QJsonObject FlowSet::toJson() const {
  QJsonObject o; QJsonArray fa;
  for (const Flow& f : flows) {
    QJsonObject j; j["name"] = f.name; j["on"] = f.on; j["auto"] = f.autoMade; j["cross"] = f.cross == Cross::AroundTube ? "tube" : "fit";
    j["phase"] = f.phase; j["su"] = f.scaleU; j["sv"] = f.scaleV; j["rev"] = f.reversed;
    QJsonArray faces; for (int x : f.faces) faces.push_back(x); j["faces"] = faces;
    QJsonArray cs; for (const Curve& c : f.curves) { QJsonObject cj; cj["w"] = c.weight; QJsonArray p; for (const Vec3& v : c.pts) { p.push_back(v.x); p.push_back(v.y); p.push_back(v.z); } cj["p"] = p; cs.push_back(cj); }
    j["curves"] = cs; fa.push_back(j);
  }
  o["flows"] = fa; o["unit"] = s.unit; o["seam"] = vec(s.seam); o["bridge"] = s.autoBridge;
  return o;
}

FlowSet FlowSet::fromJson(const QJsonObject& o) {
  FlowSet r; r.s.unit = o.value("unit").toDouble(0); r.s.seam = vecOf(o.value("seam"), Vec3(0, 0, -1)); r.s.autoBridge = o.value("bridge").toBool(true);
  for (const QJsonValue& v : o.value("flows").toArray()) {
    const QJsonObject j = v.toObject(); Flow f; f.name = j.value("name").toString(); f.on = j.value("on").toBool(true); f.autoMade = j.value("auto").toBool();
    f.cross = j.value("cross").toString() == "tube" ? Cross::AroundTube : Cross::FitWidth;
    f.phase = float(j.value("phase").toDouble()); f.scaleU = float(j.value("su").toDouble(1)); f.scaleV = float(j.value("sv").toDouble(1)); f.reversed = j.value("rev").toBool();
    for (const QJsonValue& x : j.value("faces").toArray()) f.faces.push_back(x.toInt());
    for (const QJsonValue& cv : j.value("curves").toArray()) { const QJsonObject cj = cv.toObject(); Curve c; c.weight = float(cj.value("w").toDouble(1)); const QJsonArray p = cj.value("p").toArray();
      for (int i = 0; i + 2 < p.size(); i += 3) c.pts.emplace_back(p[i].toDouble(), p[i + 1].toDouble(), p[i + 2].toDouble()); f.curves.push_back(std::move(c)); }
    r.flows.push_back(std::move(f));
  }
  return r;
}

FlowSet readFlows(const Item& it) { return FlowSet::fromJson(it.raw.value("flow").toObject()); }
void writeFlows(Item& it, const FlowSet& fs) { if (fs.flows.empty()) it.raw.remove("flow"); else it.raw["flow"] = fs.toJson(); }

// ---------------------------------------------------------------- pieces, the head, auto curves

std::vector<int> pieces(const PolyMesh& m, int* count) {
  std::vector<int> up(m.pos.size()); std::iota(up.begin(), up.end(), 0);
  std::function<int(int)> find = [&](int x) { while (up[size_t(x)] != x) { up[size_t(x)] = up[size_t(up[size_t(x)])]; x = up[size_t(x)]; } return x; };
  for (int f = 0; f < m.faceCount(); f++) { const int a = m.faceStart[size_t(f)]; for (int k = a + 1; k < m.faceStart[size_t(f) + 1]; k++) { const int x = find(m.faceVerts[size_t(a)]), y = find(m.faceVerts[size_t(k)]); if (x != y) up[size_t(x)] = y; } }
  std::vector<int> label(m.pos.size(), -1), of(size_t(m.faceCount()), -1); int n = 0;
  for (int f = 0; f < m.faceCount(); f++) { if (m.faceStart[size_t(f)] == m.faceStart[size_t(f) + 1]) continue; const int r = find(m.faceVerts[size_t(m.faceStart[size_t(f)])]); if (label[size_t(r)] < 0) label[size_t(r)] = n++; of[size_t(f)] = label[size_t(r)]; }
  if (count) *count = n;
  return of;
}

Vec3 headCenter(const Scene& sc) {
  Box3 head, hair;
  for (const auto& [id, it] : sc.items) {
    if (!it.poly) continue;
    if (it.type == ItemType::Head || it.headTag) head.unite(it.poly->bounds());
    else if (it.type == ItemType::Mesh || (it.type == ItemType::IMesh && it.raw.contains("srcSurfs"))) hair.unite(it.poly->bounds());
  }
  if (!head.empty()) {   // (a whole body tagged as the head: its head is at the top, about an eighth of its height)
    const Vec3 c = head.center(), s = head.max - head.min; const double w = std::max(s.x, s.z);
    return s.y > 1.5 * w ? Vec3(c.x, head.max.y - s.y * 0.08, c.z) : c;
  }
  if (hair.empty()) return {};
  const Vec3 c = hair.center(); return {c.x, hair.max.y - (hair.max.y - hair.min.y) / 3, c.z};
}

Curve autoCurve(const PolyMesh& m, const std::vector<int>& faces, const Vec3& rootHint, int samples) {
  Curve out; if (faces.empty()) return out;
  // the piece's vertices and edges (local numbering)
  std::vector<int> loc(m.pos.size(), -1), verts;
  std::vector<std::vector<std::pair<int, double>>> adj;
  auto id = [&](int v) { if (loc[size_t(v)] < 0) { loc[size_t(v)] = int(verts.size()); verts.push_back(v); adj.emplace_back(); } return loc[size_t(v)]; };
  for (int f : faces) { const int a = m.faceStart[size_t(f)], b = m.faceStart[size_t(f) + 1];
    for (int k = a; k < b; k++) { const int v0 = m.faceVerts[size_t(k)], v1 = m.faceVerts[size_t(k + 1 < b ? k + 1 : a)]; const int i = id(v0), j = id(v1); if (i == j) continue;
      const double w = m.pos[size_t(v0)].distanceTo(m.pos[size_t(v1)]); adj[size_t(i)].push_back({j, w}); adj[size_t(j)].push_back({i, w}); } }
  const int n = int(verts.size()); if (n < 3) return out;
  auto dijkstra = [&](const std::vector<int>& from) { std::vector<double> d(size_t(n), 1e300); using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    for (int x : from) { d[size_t(x)] = 0; q.push({0, x}); }
    while (!q.empty()) { auto [dd, x] = q.top(); q.pop(); if (dd > d[size_t(x)]) continue; for (auto [y, w] : adj[size_t(x)]) if (dd + w < d[size_t(y)]) { d[size_t(y)] = dd + w; q.push({d[size_t(y)], y}); } }
    for (auto& x : d) if (x >= 1e300) x = -1;   // (not reached: a piece of one polygon set is connected, but stay safe)
    return d; };
  // the root: a converted lock is open at its root only (its tip closed to a point) — its open border, when that border is
  // small next to the piece (a strip's border runs all round it: then the vertex nearest the head's centre)
  std::vector<int> root;
  {
    std::map<std::pair<int, int>, int> uses;
    for (int f : faces) { const int a = m.faceStart[size_t(f)], b = m.faceStart[size_t(f) + 1];
      for (int k = a; k < b; k++) { int i = loc[size_t(m.faceVerts[size_t(k)])], j = loc[size_t(m.faceVerts[size_t(k + 1 < b ? k + 1 : a)])]; if (i == j) continue; if (i > j) std::swap(i, j); uses[{i, j}]++; } }
    std::vector<int> up(static_cast<size_t>(n)); std::iota(up.begin(), up.end(), 0); std::vector<char> onB(static_cast<size_t>(n), 0);
    auto find = [&](int x) { while (up[size_t(x)] != x) x = up[size_t(x)] = up[size_t(up[size_t(x)])]; return x; };
    for (const auto& [e, c] : uses) if (c == 1) { onB[size_t(e.first)] = onB[size_t(e.second)] = 1; up[size_t(find(e.first))] = find(e.second); }
    std::map<int, std::vector<int>> loops; for (int i = 0; i < n; i++) if (onB[size_t(i)]) loops[find(i)].push_back(i);
    Box3 all; for (int v : verts) all.expand(m.pos[size_t(v)]);
    const double ext = (all.max - all.min).length(); double bestExt = 1e300;
    for (const auto& [r, vs] : loops) { Box3 b; for (int i : vs) b.expand(m.pos[size_t(verts[size_t(i)])]); const double e = (b.max - b.min).length();
      // (several small open borders: the one nearest the head)
      const double near = b.center().distanceTo(rootHint);
      if (e < 0.6 * ext && near < bestExt) { bestExt = near; root = vs; } }
  }
  if (root.empty()) { int a = 0; double best = 1e300; for (int i = 0; i < n; i++) { const double d = m.pos[size_t(verts[size_t(i)])].distanceTo(rootHint); if (d < best) { best = d; a = i; } } root = {a}; }
  const auto da = dijkstra(root); const int tip = int(std::max_element(da.begin(), da.end()) - da.begin());
  const auto g = dijkstra({tip}); const double gmax = *std::max_element(g.begin(), g.end()); if (gmax <= 0) return out;
  samples = std::max(samples, 3);
  std::vector<Vec3> sum(static_cast<size_t>(samples)); std::vector<int> cnt(size_t(samples), 0);
  for (int i = 0; i < n; i++) { if (g[size_t(i)] < 0) continue; const int k = std::clamp(int((1 - g[size_t(i)] / gmax) * samples), 0, samples - 1); sum[size_t(k)] += m.pos[size_t(verts[size_t(i)])]; cnt[size_t(k)]++; }
  for (int k = 0; k < samples; k++) if (cnt[size_t(k)]) out.pts.push_back(sum[size_t(k)] / double(cnt[size_t(k)]));
  out.pts.push_back(m.pos[size_t(verts[size_t(tip)])]);                // (the tip itself: the last bin's centre falls short of it)
  for (int it = 0; it < 2; it++) { auto p = out.pts; for (size_t i = 1; i + 1 < p.size(); i++) out.pts[i] = (p[i - 1] + p[i] * 2.0 + p[i + 1]) / 4.0; }
  return out;
}

FlowSet autoFlows(const PolyMesh& m, const Vec3& rootHint, const FlowSet* keep) {
  FlowSet r; if (keep) r.s = keep->s;
  std::vector<char> taken(size_t(m.faceCount()), 0);
  if (keep) for (const Flow& f : keep->flows) if (!f.autoMade) { r.flows.push_back(f); for (int x : f.faces) if (x >= 0 && x < m.faceCount()) taken[size_t(x)] = 1; }
  int np = 0; const auto of = pieces(m, &np);
  std::vector<std::vector<int>> pf(static_cast<size_t>(np));
  for (int f = 0; f < m.faceCount(); f++) if (of[size_t(f)] >= 0 && !taken[size_t(f)]) pf[size_t(of[size_t(f)])].push_back(f);
  int k = 1;
  for (auto& faces : pf) {
    if (faces.empty()) continue;
    Curve c = autoCurve(m, faces, rootHint); if (c.pts.size() < 2) continue;
    Flow f; f.name = QString("Flow %1").arg(k++); f.autoMade = true; f.faces = std::move(faces); f.curves.push_back(std::move(c)); r.flows.push_back(std::move(f));
  }
  return r;
}

// ---------------------------------------------------------------- the solver

Field solve(const PolyMesh& m, const FlowSet& fs, double headSize) {
  Field F; const size_t nc = m.faceVerts.size();
  F.uv.assign(nc, {0.f, 0.f}); F.along.assign(nc, 0.f); F.tangent.assign(nc, {0.f, 0.f, 0.f}); F.faceFlow.assign(size_t(m.faceCount()), -1);
  F.unit = fs.s.unit > 0 ? fs.s.unit : (headSize > 0 ? headSize * 0.25 : 1.0);
  const auto N = vertexNormals(m);
  for (size_t fi = 0; fi < fs.flows.size(); fi++) { const Flow& f = fs.flows[fi]; if (!f.on) continue; for (int x : f.faces) if (x >= 0 && x < m.faceCount()) F.faceFlow[size_t(x)] = int(fi); }
  std::vector<int> slot(m.pos.size(), -1);
  for (size_t fi = 0; fi < fs.flows.size(); fi++) {
    const Flow& f = fs.flows[fi]; if (!f.on) continue;
    std::vector<int> faces; for (int x : f.faces) if (x >= 0 && x < m.faceCount() && F.faceFlow[size_t(x)] == int(fi)) faces.push_back(x);
    if (faces.empty()) continue;
    std::vector<Line> lines;
    std::vector<double> lineW;                                   // (each built line's own weight: a curve left out — a single point — no longer shifts the weights of those after it)
    for (const Curve& c : f.curves) { auto pts = c.pts; if (f.reversed) std::reverse(pts.begin(), pts.end()); Line l; if (l.build(std::move(pts), fs.s.seam)) { lines.push_back(std::move(l)); lineW.push_back(double(c.weight)); } }
    if (lines.empty()) continue;
    // each vertex once: the nearest curve (its weight shortening the distance), the point on it, the offset in its frame
    std::vector<int> verts;
    for (int x : faces) for (int k = m.faceStart[size_t(x)]; k < m.faceStart[size_t(x) + 1]; k++) { const int v = m.faceVerts[size_t(k)]; if (slot[size_t(v)] < 0) { slot[size_t(v)] = int(verts.size()); verts.push_back(v); } }
    struct VP { int line = 0; double s = 0, lx = 0, ly = 0; Vec3 t; };
    std::vector<VP> vp(verts.size());
    for (size_t i = 0; i < verts.size(); i++) {
      const Vec3& q = m.pos[size_t(verts[i])]; double bd = 1e300;
      for (size_t li = 0; li < lines.size(); li++) {
        const auto h = lines[li].project(q); const double w = std::max(0.05, lineW[li]), d = h.d2 / (w * w);
        if (d < bd) { bd = d; const Vec3 lat = q - h.c - h.t * (q - h.c).dot(h.t); const Vec3 e2 = h.t.cross(h.e1); vp[i] = {int(li), h.s, lat.dot(h.e1), lat.dot(e2), h.t}; }
      }
    }
    // across: round a tube — the angle from the seam; else the lock's width — per stretch of the curve, the main axis of the
    // cross-section's offsets (kept from flipping between stretches), the offsets along it spread to 0…1
    const int bins = 32;
    std::vector<double> u(verts.size(), 0.5);
    auto binOf = [&](const VP& p) { return std::clamp(int(p.s / lines[size_t(p.line)].len * bins), 0, bins - 1); };
    // (roundness: the cross-section's minor / major spread; an automatic flow on a round lock is solved round it)
    std::vector<double> cxx(bins, 0), cxy(bins, 0), cyy(bins, 0); std::vector<int> cn(bins, 0);
    for (const VP& p : vp) { const int b = binOf(p); cxx[size_t(b)] += p.lx * p.lx; cxy[size_t(b)] += p.lx * p.ly; cyy[size_t(b)] += p.ly * p.ly; cn[size_t(b)]++; }
    std::vector<double> ax(bins, 1), ay(bins, 0); std::vector<double> round;
    for (int b = 0; b < bins; b++) {
      if (!cn[size_t(b)]) { if (b) { ax[size_t(b)] = ax[size_t(b) - 1]; ay[size_t(b)] = ay[size_t(b) - 1]; } continue; }
      const double xx = cxx[size_t(b)], xy = cxy[size_t(b)], yy = cyy[size_t(b)], th = 0.5 * std::atan2(2 * xy, xx - yy);
      ax[size_t(b)] = std::cos(th); ay[size_t(b)] = std::sin(th);
      const double tr = xx + yy, det = xx * yy - xy * xy, disc = std::sqrt(std::max(0.0, tr * tr / 4 - det)), l1 = tr / 2 + disc, l2 = std::max(0.0, tr / 2 - disc);
      if (l1 > 0) round.push_back(std::sqrt(l2 / l1));
      if (b && ax[size_t(b)] * ax[size_t(b) - 1] + ay[size_t(b)] * ay[size_t(b) - 1] < 0) { ax[size_t(b)] = -ax[size_t(b)]; ay[size_t(b)] = -ay[size_t(b)]; }
    }
    for (int b = bins - 2; b >= 0; b--) if (!cn[size_t(b)]) { ax[size_t(b)] = ax[size_t(b) + 1]; ay[size_t(b)] = ay[size_t(b) + 1]; }
    for (int it = 0; it < 2; it++) { auto x0 = ax, y0 = ay; for (int b = 0; b < bins; b++) { const int p = std::max(b - 1, 0), q = std::min(b + 1, bins - 1); const double x = x0[size_t(p)] + 2 * x0[size_t(b)] + x0[size_t(q)], y = y0[size_t(p)] + 2 * y0[size_t(b)] + y0[size_t(q)], l = std::hypot(x, y); if (l > 0) { ax[size_t(b)] = x / l; ay[size_t(b)] = y / l; } } }
    // (an automatic flow on a round lock goes round it — across a round section "the width" has no direction; a hand-made
    // flow keeps its own choice)
    double roundness = 0;
    if (!round.empty()) { std::nth_element(round.begin(), round.begin() + long(round.size() / 2), round.end()); roundness = round[round.size() / 2]; }
    const bool tube = f.cross == Cross::AroundTube || (f.autoMade && roundness > 0.5);
    if (tube) F.tubes++;
    if (tube) {
      for (size_t i = 0; i < vp.size(); i++) u[i] = std::atan2(vp[i].ly, vp[i].lx) / (2 * kPi) + 0.5;
    } else {
      // (the axis and the edges blended between the stretches' middles: no step in u where one stretch meets the next)
      auto blend = [&](const VP& p, int& b0, int& b1, double& t) { const double x = p.s / lines[size_t(p.line)].len * bins - 0.5; b0 = std::clamp(int(std::floor(x)), 0, bins - 1); b1 = std::min(b0 + 1, bins - 1); t = std::clamp(x - b0, 0.0, 1.0); };
      std::vector<double> d(vp.size()), lo(bins, 1e300), hi(bins, -1e300);
      for (size_t i = 0; i < vp.size(); i++) {
        int b0, b1; double t; blend(vp[i], b0, b1, t);
        double x = ax[size_t(b0)] * (1 - t) + ax[size_t(b1)] * t, y = ay[size_t(b0)] * (1 - t) + ay[size_t(b1)] * t; const double l = std::hypot(x, y); if (l > 0) { x /= l; y /= l; }
        d[i] = vp[i].lx * x + vp[i].ly * y; const int b = binOf(vp[i]); lo[size_t(b)] = std::min(lo[size_t(b)], d[i]); hi[size_t(b)] = std::max(hi[size_t(b)], d[i]);
      }
      // (a stretch's edges taken with its neighbours': a thin bin's few vertices don't squeeze the spread; empty ones filled)
      auto lo0 = lo, hi0 = hi;
      for (int b = 0; b < bins; b++) for (int k = std::max(b - 1, 0); k <= std::min(b + 1, bins - 1); k++) { lo[size_t(b)] = std::min(lo[size_t(b)], lo0[size_t(k)]); hi[size_t(b)] = std::max(hi[size_t(b)], hi0[size_t(k)]); }
      for (int b = 1; b < bins; b++) if (lo[size_t(b)] > hi[size_t(b)]) { lo[size_t(b)] = lo[size_t(b) - 1]; hi[size_t(b)] = hi[size_t(b) - 1]; }
      for (int b = bins - 2; b >= 0; b--) if (lo[size_t(b)] > hi[size_t(b)]) { lo[size_t(b)] = lo[size_t(b) + 1]; hi[size_t(b)] = hi[size_t(b) + 1]; }
      for (size_t i = 0; i < vp.size(); i++) {
        int b0, b1; double t; blend(vp[i], b0, b1, t);
        const double l0 = lo[size_t(b0)] * (1 - t) + lo[size_t(b1)] * t, h0 = hi[size_t(b0)] * (1 - t) + hi[size_t(b1)] * t, w = h0 - l0;
        u[i] = w > 1e-12 ? std::clamp((d[i] - l0) / w, 0.0, 1.0) : 0.5;
      }
    }
    // the corners
    for (int x : faces) {
      const int a = m.faceStart[size_t(x)], b = m.faceStart[size_t(x) + 1];
      for (int k = a; k < b; k++) { const size_t i = size_t(slot[size_t(m.faceVerts[size_t(k)])]); const VP& p = vp[i]; const Line& L = lines[size_t(p.line)];
        F.uv[size_t(k)] = {float(u[i] * f.scaleU), float((p.s / F.unit) * f.scaleV + f.phase)}; F.along[size_t(k)] = float(p.s / L.len); F.tangent[size_t(k)] = {float(p.t.x), float(p.t.y), float(p.t.z)}; }
      if (tube) {   // (a polygon across the seam: its low side carried past 1)
        float mn = 1e9f, mx = -1e9f; for (int k = a; k < b; k++) { mn = std::min(mn, F.uv[size_t(k)][0]); mx = std::max(mx, F.uv[size_t(k)][0]); }
        if (mx - mn > 0.5f * f.scaleU) for (int k = a; k < b; k++) if (F.uv[size_t(k)][0] < 0.5f * f.scaleU) F.uv[size_t(k)][0] += f.scaleU;
      }
      F.covered++;
    }
    for (int v : verts) slot[size_t(v)] = -1;
  }
  return F;
}

// ---------------------------------------------------------------- the shading's CPU twin

Vec3 shiftTangent(const Vec3& t, const Vec3& n, double shift) { return (t + n * shift).normalized(); }

double strandSpec(const Vec3& t, const Vec3& v, const Vec3& l, double exponent) {
  const Vec3 h = (l + v).normalized(); const double th = t.dot(h), sinTH = std::sqrt(std::max(0.0, 1 - th * th));
  return smooth01(-1, 0, th) * std::pow(sinTH, exponent);
}

double strandJitter(double u, double strips, double seed) {
  const double k = std::floor(u * strips); const double x = std::sin(k * 12.9898 + seed * 78.233) * 43758.5453;
  return (x - std::floor(x)) * 2 - 1;
}

double bandMask(double u, double v, double phase, double width, double jag, double strips) {
  const double c = phase + jag * strandJitter(u, strips) * width * 0.5, half = width * 0.5;
  return 1 - smooth01(half * 0.8, half, std::abs(v - c));
}

}  // namespace hm::flow
