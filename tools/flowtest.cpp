// Console check of the hair flow coordinates (paint/HairFlow): hm_flowtest <project.hairmesh> <outdir> [mesh name] —
// every converted hair mesh (or the one named) gets automatic flows, the field is solved, and the hair is drawn by a small
// software rasteriser from the front, the side and the back: (1) the coordinates (red — across, green — root→tip, blue
// stripes — v units; magenta — no flow), (2) the CPU twin of the planned shading (toon light + a Kajiya–Kay highlight with
// per-strip jitter + pastel tips), (3) a band in flow space (the angel ring at a fixed distance from the roots). Numbers:
// flows, covered polygons, round sections, the JSON round trip, time.
#include "io/ProjectIO.h"
#include "paint/HairFlow.h"
#include "scene/Convert.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace hm;

namespace {
// a converted hair mesh: since 2.0.1 Convert makes an ordinary mesh that remembers its cocoon (older projects: the hair-mesh kind)
bool isHair(const Item& it) { return it.type == ItemType::Mesh || (it.type == ItemType::IMesh && it.raw.contains("fromCocoon")); }
struct V { Vec3 p, n, t; float u = 0, v = 0, along = 0; int flow = -1; bool hair = true; };
struct Tri { V a, b, c; };

std::vector<Vec3> normals(const PolyMesh& m) {
  std::vector<Vec3> n(m.pos.size());
  for (int f = 0; f < m.faceCount(); f++) { const int a = m.faceStart[size_t(f)], b = m.faceStart[size_t(f) + 1]; Vec3 fn;
    for (int k = a; k < b; k++) { const Vec3& p = m.pos[size_t(m.faceVerts[size_t(k)])]; const Vec3& q = m.pos[size_t(m.faceVerts[size_t(k + 1 < b ? k + 1 : a)])]; fn += Vec3((p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x), (p.x - q.x) * (p.y + q.y)); }
    for (int k = a; k < b; k++) n[size_t(m.faceVerts[size_t(k)])] += fn; }
  for (auto& v : n) v = v.normalized();
  return n;
}
double sstep(double a, double b, double x) { const double k = std::clamp((x - a) / (b - a), 0.0, 1.0); return k * k * (3 - 2 * k); }
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc < 3) { std::printf("usage: hm_flowtest <project> <outdir> [mesh name]\n"); return 2; }
  Scene sc; LoadResult r = loadProject(QString::fromLocal8Bit(argv[1]), sc); if (!r.ok) { std::printf("load failed: %s\n", qPrintable(r.error)); return 1; }
  const QString dir = QString::fromLocal8Bit(argv[2]), only = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString(); QDir().mkpath(dir);
  // (a project not converted yet: all its cocoons converted first, as the Convert button would)
  if (std::none_of(sc.items.begin(), sc.items.end(), [](const auto& e) { return isHair(e.second) && e.second.poly; })) {
    HairState hs; hs.load(sc); std::vector<const Item*> o; for (auto& [id, it] : sc.items) if (it.type == ItemType::Cocoon) o.push_back(&it);
    std::sort(o.begin(), o.end(), [](auto* a, auto* b) { return a->order < b->order; }); std::vector<int> sel; for (auto* it : o) sel.push_back(it->id);
    QElapsedTimer t; t.start(); const ConvertResult cr = convertCocoons(sc, hs, sel); std::printf("converted %zu cocoons (%lld ms): %s\n", sel.size(), t.elapsed(), qPrintable(cr.message));
  }
  const Vec3 center = flow::headCenter(sc); const double headSize = sceneScale(sc);
  std::printf("head centre %.3f %.3f %.3f, size %.3f\n", center.x, center.y, center.z, headSize);
  std::vector<Tri> tris; Box3 box; int meshes = 0, failures = 0;
  for (auto& [id, it] : sc.items) {
    if (!it.poly || !it.visible && only.isEmpty()) continue;
    const bool hair = isHair(it) && (only.isEmpty() || it.name == only), head = it.type == ItemType::Head || it.headTag;
    if (!hair && !head) continue;
    const PolyMesh& M = *it.poly; const auto N = normals(M);
    flow::Field F;
    if (hair) {
      QElapsedTimer t; t.start();
      const flow::FlowSet fs = flow::autoFlows(M, center); const qint64 tA = t.elapsed(); t.restart();
      F = flow::solve(M, fs, headSize); const qint64 tS = t.elapsed();
      // (the record survives a save: the same flows, polygons and curve points back)
      const flow::FlowSet back = flow::FlowSet::fromJson(QJsonDocument::fromJson(QJsonDocument(fs.toJson()).toJson()).object());
      bool same = back.flows.size() == fs.flows.size();
      for (size_t i = 0; same && i < fs.flows.size(); i++) same = back.flows[i].faces == fs.flows[i].faces && back.flows[i].curves.size() == fs.flows[i].curves.size() && back.flows[i].curves[0].pts.size() == fs.flows[i].curves[0].pts.size();
      double lmin = 1e300, lmax = 0; for (const auto& f : fs.flows) { double l = 0; for (size_t i = 1; i < f.curves[0].pts.size(); i++) l += f.curves[0].pts[i].distanceTo(f.curves[0].pts[i - 1]); lmin = std::min(lmin, l); lmax = std::max(lmax, l); }
      int bad = 0; for (size_t k = 0; k < F.uv.size(); k++) if (!std::isfinite(F.uv[k][0]) || !std::isfinite(F.uv[k][1])) bad++;
      std::printf("%-28s %6d faces  %4zu flows  covered %6d  round %4d  curve %.3f..%.3f  unit %.3f  json %s  nan %d  auto %lld ms  solve %lld ms\n",
                  qPrintable(it.name), M.faceCount(), fs.flows.size(), F.covered, F.tubes, lmin, lmax, F.unit, same ? "ok" : "DIFF", bad, tA, tS);
      if (!same || bad || F.covered != M.faceCount()) failures++;
      meshes++;
    }
    for (int f = 0; f < M.faceCount(); f++) {
      const int a = M.faceStart[size_t(f)], b = M.faceStart[size_t(f) + 1];
      auto vert = [&](int k) { V v; const int vi = M.faceVerts[size_t(k)]; v.p = M.pos[size_t(vi)]; v.n = N[size_t(vi)]; v.hair = hair;
        if (hair) { v.u = F.uv[size_t(k)][0]; v.v = F.uv[size_t(k)][1]; v.along = F.along[size_t(k)]; v.t = Vec3(F.tangent[size_t(k)][0], F.tangent[size_t(k)][1], F.tangent[size_t(k)][2]); v.flow = F.faceFlow[size_t(f)]; }
        return v; };
      for (int k = a + 1; k + 1 < b; k++) tris.push_back({vert(a), vert(k), vert(k + 1)});
      if (hair) for (int k = a; k < b; k++) box.expand(M.pos[size_t(M.faceVerts[size_t(k)])]);   // (framed on the hair)
    }
  }
  if (!meshes) { std::printf("no hair meshes\n"); return 1; }

  // the pictures: yaw 0 — the front (+Z towards the viewer), 90 — the side, 180 — the back
  const int W = 900, H = 900;
  for (int view = 0; view < 3; view++) {
    const double yaw = view * 90 * 3.14159265358979 / 180; const Vec3 d(std::sin(yaw), 0, std::cos(yaw)), rt(std::cos(yaw), 0, -std::sin(yaw)), up(0, 1, 0);
    const Vec3 c = box.center(); const double half = std::max({box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z}) * 0.55, k = W / (2 * half);
    const Vec3 L = (d * 1.0 + up * 0.7 + rt * -0.5).normalized();     // (the key light: above, a little left of the camera)
    for (int mode = 0; mode < 3; mode++) {
      QImage im(W, H, QImage::Format_RGB32); im.fill(QColor(22, 22, 26)); std::vector<double> zb(size_t(W * H), -1e300);
      for (const Tri& T : tris) {
        const V* P[3] = {&T.a, &T.b, &T.c}; double sx[3], sy[3], sz[3];
        for (int j = 0; j < 3; j++) { const Vec3 q = P[j]->p - c; sx[j] = W / 2 + q.dot(rt) * k; sy[j] = H / 2 - q.dot(up) * k; sz[j] = q.dot(d); }
        const double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]); if (std::abs(area) < 1e-9) continue;
        const int x0 = std::max(0, int(std::floor(std::min({sx[0], sx[1], sx[2]})))), x1 = std::min(W - 1, int(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
        const int y0 = std::max(0, int(std::floor(std::min({sy[0], sy[1], sy[2]})))), y1 = std::min(H - 1, int(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
          const double px = x + 0.5, py = y + 0.5;
          const double w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area, w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area, w2 = 1 - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          const double z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2]; double& zz = zb[size_t(y * W + x)]; if (z <= zz) continue; zz = z;
          Vec3 n = (T.a.n * w0 + T.b.n * w1 + T.c.n * w2).normalized(); if (n.dot(d) < 0) n = -n;
          double cr, cg, cb;
          if (!T.a.hair) { const double l = 0.35 + 0.55 * std::max(0.0, n.dot(L)); cr = 0.93 * l; cg = 0.82 * l; cb = 0.78 * l; }
          else if (T.a.flow < 0) { cr = 1; cg = 0; cb = 1; }
          else {
            const double u = w0 * T.a.u + w1 * T.b.u + w2 * T.c.u, v = w0 * T.a.v + w1 * T.b.v + w2 * T.c.v, al = w0 * T.a.along + w1 * T.b.along + w2 * T.c.along;
            if (mode == 0) { cr = u; cg = al; cb = std::fmod(std::abs(v) * 2, 1.0) < 0.5 ? 0.15 : 0.75; }
            else {
              Vec3 t = T.a.t * w0 + T.b.t * w1 + T.c.t * w2; t = (t - n * t.dot(n)).normalized();
              const double tip = sstep(0.72, 1.0, al);
              const double br = 0.07 + tip * 0.48, bg = 0.07 + tip * 0.56, bb = 0.10 + tip * 0.80;   // (near black, pastel blue at the tips)
              const double lit = sstep(0.42, 0.48, 0.5 + 0.5 * n.dot(L)); const double sh = 0.55 + 0.6 * lit;
              cr = br * sh; cg = bg * sh; cb = bb * sh;
              const double rim = std::pow(1 - std::max(0.0, n.dot(d)), 4) * 0.35; cr += rim * 0.6; cg += rim * 0.65; cb += rim * 0.75;
              if (mode == 1) {   // Kajiya–Kay: the main band (white) and a second, shifted, tinted one; the strips' jitter cuts the edge
                const double j = flow::strandJitter(u, 5, T.a.flow);
                const double s1 = flow::strandSpec(flow::shiftTangent(t, n, -0.05 + 0.10 * j), d, L, 160), s2 = flow::strandSpec(flow::shiftTangent(t, n, 0.25 + 0.10 * j), d, L, 60);
                const double h1 = sstep(0.42, 0.5, s1) * lit, h2 = sstep(0.5, 0.6, s2) * 0.45 * lit;
                cr += h1 * 0.85 + h2 * 0.45; cg += h1 * 0.88 + h2 * 0.55; cb += h1 * 0.95 + h2 * 0.75;
              } else {           // a band in flow space: one ring at 0.9 units from the roots, its edge cut per strip (cyan)
                const double m = flow::bandMask(u, v, 0.9, 0.35, 0.8, 9);
                cr += m * 0.25; cg += m * 0.75; cb += m * 0.75;
              }
            }
          }
          im.setPixel(x, y, qRgb(std::clamp(int(cr * 255), 0, 255), std::clamp(int(cg * 255), 0, 255), std::clamp(int(cb * 255), 0, 255)));
        }
      }
      static const char* vn[3] = {"front", "side", "back"}; static const char* mn[3] = {"coords", "shade", "band"};
      im.save(dir + QString("/%1_%2.png").arg(mn[mode], vn[view]));
    }
  }
  std::printf("%d meshes, %d with problems; pictures in %s\n", meshes, failures, qPrintable(dir));
  return failures ? 1 : 0;
}
