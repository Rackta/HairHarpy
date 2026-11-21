// console check of the primitives: hm_primtest <outdir> — PolySphere with Stretch: closed (each edge in two polygons, turned
// the same way), polygon counts, a side-view PNG of each case
#include "scene/Primitives.h"
#include <QImage>
#include <QPainter>
#include <cstdio>
#include <map>

using namespace hm;

static bool closedOk(const PolyMesh& m, int& bad) {
  std::map<std::pair<int, int>, int> dir; bad = 0;
  for (size_t f = 0; f + 1 < m.faceStart.size(); f++) {
    const int a = m.faceStart[f], b = m.faceStart[f + 1];
    for (int i = a; i < b; i++) dir[{m.faceVerts[size_t(i)], m.faceVerts[size_t(i + 1 < b ? i + 1 : a)]}]++; }
  for (auto& [e, k] : dir) { auto o = dir.find({e.second, e.first}); if (k != 1 || o == dir.end() || o->second != 1) bad++; }
  return bad == 0;
}

int main(int argc, char** argv) {
  const QString out = argc > 1 ? argv[1] : ".";
  struct Case { int div; double st; int seg; } cases[] = {{4, 0, 1}, {4, 1, 1}, {4, 1, 3}, {2, 0.8, 2}, {3, 0, 1}, {3, 1, 4}, {1, 1, 2}, {5, 0.5, 2}};
  int fails = 0;
  for (auto& k : cases) {
    QJsonObject p = prim::defaults("psphere"); p["div"] = k.div; p["st"] = k.st; p["stseg"] = k.seg;
    const PolyMesh m = prim::build("psphere", p, Vec3(0, 0, 0), 1.0);
    int bad = 0; const bool ok = closedOk(m, bad); fails += !ok;
    double y0 = 1e9, y1 = -1e9; for (auto& v : m.pos) { y0 = std::min(y0, v.y); y1 = std::max(y1, v.y); }
    std::printf("div %d st %.2f seg %d: %d verts %d polys height %.3f %s\n", k.div, k.st, k.seg, int(m.pos.size()), int(m.faceStart.size()) - 1, y1 - y0, ok ? "closed" : "OPEN/BAD");
    QImage img(400, 600, QImage::Format_RGB32); img.fill(QColor(40, 40, 40)); QPainter g(&img); g.setRenderHint(QPainter::Antialiasing);
    g.setPen(QColor(200, 160, 255));
    auto P = [&](const Vec3& v) { const double ca = 0.83, sa = 0.56; return QPointF(200 + 220 * (v.x * ca - v.z * sa), 300 - 220 * (v.y + 0.25 * (v.x * sa + v.z * ca))); };
    for (size_t f = 0; f + 1 < m.faceStart.size(); f++) { const int a = m.faceStart[f], b = m.faceStart[f + 1];
      for (int i = a; i < b; i++) g.drawLine(P(m.pos[size_t(m.faceVerts[size_t(i)])]), P(m.pos[size_t(m.faceVerts[size_t(i + 1 < b ? i + 1 : a)])])); }
    img.save(out + QString("/psphere_d%1_s%2_g%3.png").arg(k.div).arg(k.st).arg(k.seg));
  }
  return fails;
}
