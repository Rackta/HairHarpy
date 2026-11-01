// Lining up: Grid (the quads of a shell as a grid — LSCM with every quad strip's edges tied level / upright), a chain
// of edges straightened, a shell turned by an edge or by the 3D up, shells stacked over one (or set apart), two shells
// snapped together along their seam. All in the texture's space (texel-square): U · Dw, V · Dh.
#include "scene/Uv.h"
#include "scene/UvSolve.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace hm::uv {

namespace {
struct Tex { double w = 1, h = 1; };
Tex texOf(const PackOpts* po) { if (!po) return {}; const double M = std::max(po->w, po->h); return {po->w / M, po->h / M}; }
// a shell's corners and polygons (by the UV's own shells)
struct ShellSet { Points P; Shells S; std::vector<std::vector<int>> faces; };
ShellSet shellSet(const PolyMesh& m, const topo::TTopo& T) {
  ShellSet r; r.P = points(m, T); r.S = shells(m, T, r.P); r.faces.resize(size_t(r.S.count));
  for (int f = 0; f < T.nf; f++) r.faces[size_t(r.S.of[size_t(f)])].push_back(f);
  return r;
}
std::vector<int> cornersOf(const topo::TTopo& T, const std::vector<int>& fs) { std::vector<int> k; for (int f : fs) for (int c = T.fo[size_t(f)]; c < T.fo[size_t(f) + 1]; c++) k.push_back(c); return k; }
double signedArea(const PolyMesh& m, const std::vector<int>& fs, const Tex& tx) { double a = 0; for (int f : fs) a += faceArea(m, f); return a * tx.w * tx.h; }
// the corners turned by `ang` about (cu, cv), in the texture's space
void turn(PolyMesh& m, const std::vector<int>& ks, double ang, double cu, double cv, const Tex& tx) {
  const double c = std::cos(ang), s = std::sin(ang);
  for (int k : ks) { UV& q = m.cornerUV[size_t(k)]; const double x = (q[0] - cu) * tx.w, y = (q[1] - cv) * tx.h; q = {float(cu + (x * c - y * s) / tx.w), float(cv + (x * s + y * c) / tx.h)}; }
}
std::array<double, 2> centre(const PolyMesh& m, const std::vector<int>& ks) { const auto b = box(m, ks); return {(b[0] + b[2]) / 2, (b[1] + b[3]) / 2}; }
}  // namespace

// ---------- Grid ----------
int rectify(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts* po) {
  if (!has(m)) return 0; const Tex tx = texOf(po);
  const ShellSet ss = shellSet(m, T); std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(ss.S.of[size_t(f)]);
  int done = 0;
  for (int s : want) {
    const auto& fs = ss.faces[size_t(s)]; std::unordered_map<int, int> loc; std::vector<int> pt;
    auto L = [&](int k) { const int p = ss.P.of[size_t(k)]; auto r = loc.emplace(p, int(pt.size())); if (r.second) pt.push_back(p); return r.first->second; };
    std::vector<solve::Tri> tris; std::vector<Vec3> X; double a3 = 0;
    for (int f : fs) { std::vector<int> li; std::vector<Vec3> ring; for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { li.push_back(L(k)); ring.push_back(m.pos[size_t(T.fv[size_t(k)])]); }
      for (auto& t : solve::triangulate(ring)) tris.push_back({li[size_t(t[0])], li[size_t(t[1])], li[size_t(t[2])]}); a3 += faceArea3(m, f); }
    X.resize(pt.size()); for (size_t i = 0; i < pt.size(); i++) X[i] = m.pos[size_t(ss.P.vert[size_t(pt[i])])];
    // the edges' classes over the quads: 1 — level (across), 2 — upright (along); a quad's opposite sides alike
    std::map<int, int> cls; std::vector<char> seen(size_t(T.nf), 0); std::set<int> inShell(fs.begin(), fs.end());
    auto quad = [&](int f) { return T.faceSize(f) == 4; };
    int seed = -1; for (int f : fs) if (quad(f)) { seed = f; break; }
    if (seed < 0) continue;
    { const int k0 = T.fo[size_t(seed)]; const UV &a = m.cornerUV[size_t(k0)], &b = m.cornerUV[size_t(k0 + 1)];
      const bool level = std::abs((double(b[0]) - a[0]) * tx.w) >= std::abs((double(b[1]) - a[1]) * tx.h);
      for (int j = 0; j < 4; j++) cls[T.ce[size_t(k0 + j)]] = ((j % 2 == 0) == level) ? 1 : 2; }
    std::vector<int> st{seed}; seen[size_t(seed)] = 1; int conflicts = 0;
    while (!st.empty()) { const int f = st.back(); st.pop_back(); const int k0 = T.fo[size_t(f)];
      for (int j = 0; j < 4; j++) { const int e = T.ce[size_t(k0 + j)]; if (!continuous(m, T, ss.P, e)) continue;
        const int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (g < 0 || seen[size_t(g)] || !inShell.count(g) || !quad(g)) continue;
        const int c = cls[e]; int jg = -1; for (int i = 0; i < 4; i++) if (T.ce[size_t(T.fo[size_t(g)] + i)] == e) jg = i; if (jg < 0) continue;
        bool ok = true; for (int i = 0; i < 4; i++) { const int eg = T.ce[size_t(T.fo[size_t(g)] + i)]; const int want = ((i - jg) % 2 == 0) ? c : 3 - c; auto it = cls.find(eg); if (it != cls.end() && it->second != want) ok = false; }
        if (!ok) { conflicts++; continue; }                            // (a pole, a twist: that quad left free)
        for (int i = 0; i < 4; i++) cls[T.ce[size_t(T.fo[size_t(g)] + i)]] = ((i - jg) % 2 == 0) ? c : 3 - c;
        seen[size_t(g)] = 1; st.push_back(g); } }
    // the ties: a level edge's ends one V, an upright one's one U (the UV points of the edge's corners in this shell)
    solve::Dofs D(int(pt.size())); int pa = -1, pb = -1; std::vector<std::array<int, 2>> soft;   // (soft: the same as pulls — for the start that can't keep them)
    for (int f : fs) if (seen[size_t(f)]) for (int j = 0; j < 4; j++) { const int k = T.fo[size_t(f)] + j, nk = T.fo[size_t(f)] + (j + 1) % 4; const int e = T.ce[size_t(k)];
      const int a = L(k), b = L(nk); if (a == b) continue; if (cls[e] == 1) { D.tie(b * 2 + 1, a * 2 + 1); soft.push_back({b * 2 + 1, a * 2 + 1}); if (pa < 0) { pa = a; pb = b; } } else { D.tie(b * 2, a * 2); soft.push_back({b * 2, a * 2}); } }
    if (pa < 0) continue;
    // (its place, size and way round as it was)
    const auto ks = cornersOf(T, fs); const auto c0 = centre(m, ks); const double sa0 = signedArea(m, fs, tx);
    solve::Dofs Dl = D; Dl.holdPoint(pa, {0, 0}); Dl.hold(pb * 2, (X[size_t(pb)] - X[size_t(pa)]).length());
    std::vector<solve::P2> g(X.size()); for (size_t i = 0; i < X.size(); i++) { const UV& q = m.cornerUV[size_t(ss.P.list[size_t(ss.P.off[size_t(pt[i])])])]; g[i] = {q[0] * tx.w, q[1] * tx.h}; }
    auto U = solve::lscm(X, tris, Dl, g);
    if (solve::flips(U, X, tris)) {
      // (the grid held outright folds — strips that bend, a pole: from the UV as it lies (at its 3D size), pulled towards
      // the grid; then held exactly if it can be — else as near as it came, never turned over)
      std::vector<solve::P2> W = g; const double k3 = std::abs(sa0) > 1e-30 ? std::sqrt(a3 / std::abs(sa0)) : 1; for (auto& q : W) q = {q[0] * k3, q[1] * k3};
      if (solve::flips(W, X, tris)) continue;
      if (!solve::slim(W, X, tris, solve::Dofs(int(pt.size())), 60, &soft).ok) continue;
      auto W2 = W; if (solve::slim(W2, X, tris, D, 40).ok && !solve::flips(W2, X, tris)) W = std::move(W2);
      U = std::move(W);
    } else solve::slim(U, X, tris, D, 40);                           // (the lengths evened out, the grid kept)
    if (solve::flips(U, X, tris) || !std::isfinite(solve::distortion(U, X, tris))) continue;   // (anything turned over: the shell left as it was)
    double sa = 0; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; sa += ((q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1])) / 2; }
    if ((sa < 0) != (sa0 < 0)) for (auto& q : U) q[0] = -q[0];      // (laid the way round it was)
    const double k = std::abs(sa) > 1e-30 && std::abs(sa0) > 1e-30 ? std::sqrt(std::abs(sa0) / std::abs(sa)) : 1;
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300; for (auto& q : U) { x0 = std::min(x0, q[0]); x1 = std::max(x1, q[0]); y0 = std::min(y0, q[1]); y1 = std::max(y1, q[1]); }
    std::vector<UV> was; for (int kk : ks) was.push_back(m.cornerUV[size_t(kk)]);   // (kept: put back if the written UV turns a triangle over)
    for (int f : fs) for (int kk = T.fo[size_t(f)]; kk < T.fo[size_t(f) + 1]; kk++) { const auto& q = U[size_t(L(kk))];
      m.cornerUV[size_t(kk)] = {float(c0[0] + (q[0] - (x0 + x1) / 2) * k / tx.w), float(c0[1] + (q[1] - (y0 + y1) / 2) * k / tx.h)}; }
    { std::vector<solve::P2> W(X.size()); for (int f : fs) for (int kk = T.fo[size_t(f)]; kk < T.fo[size_t(f) + 1]; kk++) { const UV& q = m.cornerUV[size_t(kk)]; W[size_t(L(kk))] = {q[0] * tx.w, q[1] * tx.h}; }
      double before = 0; { std::vector<solve::P2> O(X.size()); for (size_t i = 0; i < ks.size(); i++) O[size_t(L(ks[i]))] = {was[i][0] * tx.w, was[i][1] * tx.h}; before = solve::distortion(O, X, tris); }
      const double after = solve::distortion(W, X, tris);   // (squeezed flat somewhere — far more stretch than it had: left as it was)
      if (solve::flips(W, X, tris) || !std::isfinite(after) || (after > 20 && after > 3 * before)) { for (size_t i = 0; i < ks.size(); i++) m.cornerUV[size_t(ks[i])] = was[i]; continue; } }
    (void)conflicts; done++;
  }
  return done;
}

// ---------- Straighten a chain ----------
int straightenChain(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& edges, const PackOpts* po) {
  if (!has(m) || edges.empty()) return 0; const Tex tx = texOf(po); const Points P = points(m, T); const Shells S = shells(m, T, P);
  std::map<int, std::vector<int>> adj; for (int e : edges) if (e >= 0 && e < T.ne) { adj[T.ea[size_t(e)]].push_back(T.eb[size_t(e)]); adj[T.eb[size_t(e)]].push_back(T.ea[size_t(e)]); }
  int start = adj.begin()->first; for (auto& [v, nb] : adj) if (nb.size() == 1) { start = v; break; }
  std::vector<int> chain{start}; std::set<int> used{start};
  for (int cur = start; ; ) { int nx = -1; for (int w : adj[cur]) if (!used.count(w)) { nx = w; break; } if (nx < 0) break; chain.push_back(nx); used.insert(nx); cur = nx; }
  if (chain.size() < 3) return 0;
  // (the shell: the first edge's first polygon; each vertex's UV point there)
  const int e0 = T.edgeOf(chain[0], chain[1]); const int sh = S.of[size_t(T.ef0[size_t(e0)])];
  auto pointAt = [&](int v) { for (int i = T.vfo[size_t(v)]; i < T.vfo[size_t(v) + 1]; i++) { const int f = T.vF[size_t(i)]; if (S.of[size_t(f)] != sh) continue; const int k = cornerAt(T, f, v); if (k >= 0) return P.of[size_t(k)]; } return -1; };
  std::vector<int> pts; for (int v : chain) { const int p = pointAt(v); if (p < 0) return 0; pts.push_back(p); }
  auto uvOf = [&](int p) { const UV& q = m.cornerUV[size_t(P.list[size_t(P.off[size_t(p)])])]; return std::array<double, 2>{q[0] * tx.w, q[1] * tx.h}; };
  std::vector<double> acc{0}; for (size_t i = 1; i < chain.size(); i++) acc.push_back(acc.back() + m.pos[size_t(chain[i])].distanceTo(m.pos[size_t(chain[i - 1])]));
  const auto A = uvOf(pts.front()), B = uvOf(pts.back()); const double L = acc.back(); if (!(L > 0)) return 0;
  for (size_t i = 1; i + 1 < pts.size(); i++) { const double t = acc[i] / L; const float u = float((A[0] + (B[0] - A[0]) * t) / tx.w), v = float((A[1] + (B[1] - A[1]) * t) / tx.h);
    for (int j = P.off[size_t(pts[i])]; j < P.off[size_t(pts[i]) + 1]; j++) m.cornerUV[size_t(P.list[size_t(j)])] = {u, v}; }
  return int(pts.size());
}

// ---------- Orient ----------
double orientByEdge(PolyMesh& m, const topo::TTopo& T, int edge, const PackOpts* po) {
  if (!has(m) || edge < 0 || edge >= T.ne) return 0; const Tex tx = texOf(po);
  const ShellSet ss = shellSet(m, T); const int f = T.ef0[size_t(edge)]; const int ka = cornerAt(T, f, T.ea[size_t(edge)]), kb = cornerAt(T, f, T.eb[size_t(edge)]);
  const UV &a = m.cornerUV[size_t(ka)], &b = m.cornerUV[size_t(kb)];
  const double ang = std::atan2((double(b[1]) - a[1]) * tx.h, (double(b[0]) - a[0]) * tx.w), target = std::round(ang / (M_PI / 2)) * (M_PI / 2);
  const auto ks = cornersOf(T, ss.faces[size_t(ss.S.of[size_t(f)])]); const auto c = centre(m, ks);
  turn(m, ks, target - ang, c[0], c[1], tx);
  return (target - ang) * 180 / M_PI;
}
int orientUpright(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts* po) {
  if (!has(m)) return 0; const Tex tx = texOf(po);
  const ShellSet ss = shellSet(m, T); std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(ss.S.of[size_t(f)]);
  int done = 0;
  for (int s : want) {
    // (the 3D height as a plane over the UV: y ≈ a·x + b·y + c — its rise (a, b) turned to point up V)
    const auto ks = cornersOf(T, ss.faces[size_t(s)]); double n = 0, sx = 0, sy = 0, sz = 0, sxx = 0, sxy = 0, syy = 0, sxz = 0, syz = 0;
    for (int k : ks) { const double x = m.cornerUV[size_t(k)][0] * tx.w, y = m.cornerUV[size_t(k)][1] * tx.h, z = m.pos[size_t(T.fv[size_t(k)])].y;
      n++; sx += x; sy += y; sz += z; sxx += x * x; sxy += x * y; syy += y * y; sxz += x * z; syz += y * z; }
    const double cxx = sxx - sx * sx / n, cxy = sxy - sx * sy / n, cyy = syy - sy * sy / n, cxz = sxz - sx * sz / n, cyz = syz - sy * sz / n, det = cxx * cyy - cxy * cxy;
    if (!(std::abs(det) > 1e-30)) continue;
    const double a = (cxz * cyy - cyz * cxy) / det, b = (cyz * cxx - cxz * cxy) / det; if (!(std::hypot(a, b) > 0)) continue;
    const auto c = centre(m, ks); turn(m, ks, M_PI / 2 - std::atan2(b, a), c[0], c[1], tx); done++;
  }
  return done;
}

// ---------- Stack / Unstack ----------
int stackOnto(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, int refFace, const PackOpts* po) {
  if (!has(m) || refFace < 0 || refFace >= T.nf) return 0; const Tex tx = texOf(po);
  const ShellSet ss = shellSet(m, T); const int ref = ss.S.of[size_t(refFace)];
  std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf && ss.S.of[size_t(f)] != ref) want.insert(ss.S.of[size_t(f)]);
  // a shell in the texture's space: its points, its middle (by area), its main axis, its area
  struct Shape { std::vector<std::array<double, 2>> p; std::array<double, 2> c{0, 0}; double ang = 0, area = 0; };
  auto shape = [&](int s) { Shape r; const auto ks = cornersOf(T, ss.faces[size_t(s)]); for (int k : ks) r.p.push_back({m.cornerUV[size_t(k)][0] * tx.w, m.cornerUV[size_t(k)][1] * tx.h});
    for (auto& q : r.p) { r.c[0] += q[0] / double(r.p.size()); r.c[1] += q[1] / double(r.p.size()); }
    double xx = 0, xy = 0, yy = 0; for (auto& q : r.p) { const double x = q[0] - r.c[0], y = q[1] - r.c[1]; xx += x * x; xy += x * y; yy += y * y; }
    r.ang = 0.5 * std::atan2(2 * xy, xx - yy); r.area = signedArea(m, ss.faces[size_t(s)], tx); return r; };
  const Shape R = shape(ref); if (R.p.empty()) return 0;
  auto miss = [&](const std::vector<std::array<double, 2>>& Q) { double d = 0; const size_t step = std::max<size_t>(1, Q.size() / 300);   // (how far its points lie from the reference's)
    for (size_t i = 0; i < Q.size(); i += step) { double b = 1e300; const size_t st2 = std::max<size_t>(1, R.p.size() / 600); for (size_t j = 0; j < R.p.size(); j += st2) b = std::min(b, std::hypot(Q[i][0] - R.p[j][0], Q[i][1] - R.p[j][1])); d += b; } return d; };
  int done = 0;
  for (int s : want) {
    const Shape S = shape(s); if (S.p.empty() || !(std::abs(S.area) > 0)) continue;
    const double k = std::sqrt(std::abs(R.area) / std::abs(S.area));
    double best = 1e300; std::vector<std::array<double, 2>> bestQ;
    for (bool mir : {false, true}) for (double extra : {0.0, M_PI}) {   // (as it lies or mirrored — a mirror twin — each turned two ways)
      const double ang = R.ang - (mir ? -S.ang : S.ang) + extra, c = std::cos(ang), sn = std::sin(ang); std::vector<std::array<double, 2>> Q;
      for (auto& q : S.p) { double x = (q[0] - S.c[0]) * k, y = (q[1] - S.c[1]) * k; if (mir) x = -x; Q.push_back({R.c[0] + x * c - y * sn, R.c[1] + x * sn + y * c}); }
      const double d = miss(Q); if (d < best) { best = d; bestQ = std::move(Q); } }
    const auto ks = cornersOf(T, ss.faces[size_t(s)]); for (size_t i = 0; i < ks.size(); i++) m.cornerUV[size_t(ks[i])] = {float(bestQ[i][0] / tx.w), float(bestQ[i][1] / tx.h)};
    done++;
  }
  return done;
}
int unstack(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, double gap) {
  if (!has(m)) return 0; const ShellSet ss = shellSet(m, T);
  std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(ss.S.of[size_t(f)]);
  std::vector<int> sh(want.begin(), want.end()); std::vector<std::array<double, 4>> bx; for (int s : sh) bx.push_back(box(m, cornersOf(T, ss.faces[size_t(s)])));
  int moved = 0; std::vector<char> done(sh.size(), 0);
  for (size_t i = 0; i < sh.size(); i++) { if (done[i]) continue; double right = bx[i][2];   // (those over shell i: along U to its right, one after another)
    for (size_t j = i + 1; j < sh.size(); j++) { if (done[j]) continue;
      const double iw = std::min(bx[i][2], bx[j][2]) - std::max(bx[i][0], bx[j][0]), ih = std::min(bx[i][3], bx[j][3]) - std::max(bx[i][1], bx[j][1]);
      if (iw <= 0 || ih <= 0) continue; const double du = right + gap - bx[j][0];
      transform(m, cornersOf(T, ss.faces[size_t(sh[j])]), [&](const UV& q) { return UV{float(q[0] + du), q[1]}; });
      right += gap + (bx[j][2] - bx[j][0]); done[j] = 1; moved++; } }
  return moved;
}

// ---------- Snap together ----------
int snapTogether(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& edges, const PackOpts* po) {
  if (!has(m)) return 0; const Tex tx = texOf(po); const ShellSet ss = shellSet(m, T);
  // the seam's sides: per edge the corners at its ends in its two polygons — the two shells the most edges join
  std::map<std::pair<int, int>, int> cnt;
  for (int e : edges) { if (e < 0 || e >= T.ne || T.ef1[size_t(e)] < 0) continue; const int a = ss.S.of[size_t(T.ef0[size_t(e)])], b = ss.S.of[size_t(T.ef1[size_t(e)])]; if (a != b) cnt[std::minmax(a, b)]++; }
  if (cnt.empty()) return 0;
  const auto pr = std::max_element(cnt.begin(), cnt.end(), [](auto& x, auto& y) { return x.second < y.second; })->first;
  const double A0 = std::abs(signedArea(m, ss.faces[size_t(pr.first)], tx)), A1 = std::abs(signedArea(m, ss.faces[size_t(pr.second)], tx));
  const int fixedS = A0 >= A1 ? pr.first : pr.second, movS = fixedS == pr.first ? pr.second : pr.first;
  std::vector<std::array<int, 2>> pairs; std::vector<int> sewn;                     // (moving corner, fixed corner)
  for (int e : edges) { if (e < 0 || e >= T.ne || T.ef1[size_t(e)] < 0) continue; int f0 = T.ef0[size_t(e)], f1 = T.ef1[size_t(e)];
    if (ss.S.of[size_t(f0)] == fixedS && ss.S.of[size_t(f1)] == movS) std::swap(f0, f1); if (ss.S.of[size_t(f0)] != movS || ss.S.of[size_t(f1)] != fixedS) continue;
    for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) pairs.push_back({cornerAt(T, f0, v), cornerAt(T, f1, v)}); sewn.push_back(e); }
  if (pairs.size() < 2) return 0;
  // (the best turn and move of the moving side onto the fixed one — no scaling, no mirror)
  double ca[2] = {0, 0}, cb[2] = {0, 0}; const double n = double(pairs.size());
  for (auto& p : pairs) { ca[0] += m.cornerUV[size_t(p[0])][0] * tx.w / n; ca[1] += m.cornerUV[size_t(p[0])][1] * tx.h / n; cb[0] += m.cornerUV[size_t(p[1])][0] * tx.w / n; cb[1] += m.cornerUV[size_t(p[1])][1] * tx.h / n; }
  double sc = 0, ss2 = 0; for (auto& p : pairs) { const double x = m.cornerUV[size_t(p[0])][0] * tx.w - ca[0], y = m.cornerUV[size_t(p[0])][1] * tx.h - ca[1], u = m.cornerUV[size_t(p[1])][0] * tx.w - cb[0], v = m.cornerUV[size_t(p[1])][1] * tx.h - cb[1]; sc += x * u + y * v; ss2 += x * v - y * u; }
  const double th = std::atan2(ss2, sc), c = std::cos(th), s = std::sin(th);
  const auto ks = cornersOf(T, ss.faces[size_t(movS)]);
  for (int k : ks) { UV& q = m.cornerUV[size_t(k)]; const double x = q[0] * tx.w - ca[0], y = q[1] * tx.h - ca[1]; q = {float((cb[0] + x * c - y * s) / tx.w), float((cb[1] + x * s + y * c) / tx.h)}; }
  // (sewn: each moving corner onto its fixed one — the UV points join; the seam off those edges)
  for (auto& p : pairs) { const int mp = ss.P.of[size_t(p[0])]; const UV target = m.cornerUV[size_t(p[1])]; for (int j = ss.P.off[size_t(mp)]; j < ss.P.off[size_t(mp) + 1]; j++) m.cornerUV[size_t(ss.P.list[size_t(j)])] = target; }
  auto fl = seamFlags(m, T); for (int e : sewn) fl[size_t(e)] = 0; setSeams(m, T, fl);
  return int(sewn.size());
}

}  // namespace hm::uv
