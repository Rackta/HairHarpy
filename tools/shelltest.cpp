// Console check of the outer-shell analysis: hm_shelltest <project.hairmesh> <mesh name> <dir> — a high-poly reference with a double wall:
// its pieces, rims, and slices through it drawn to PNGs, each polygon's cut coloured by what the analysis says of it.
#include "io/ProjectIO.h"
#include "scene/OuterShell.h"
#include "scene/Topo.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <cstdio>
#include <cmath>
using namespace hm;
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv); if (argc < 4) { std::printf("usage: hm_shelltest <project> <mesh> <dir>\n"); return 2; }
  Scene sc; LoadResult r = loadProject(QString::fromLocal8Bit(argv[1]), sc); if (!r.ok) { std::printf("load failed: %s\n", qPrintable(r.error)); return 1; }
  const QString name = QString::fromLocal8Bit(argv[2]), dir = QString::fromLocal8Bit(argv[3]);
  for (auto& [id, it] : sc.items) { if (it.type != ItemType::IMesh || !it.poly || it.name != name) continue;
    const PolyMesh& M = *it.poly; std::printf("%s: %d faces, %zu verts\n", qPrintable(it.name), M.faceCount(), M.pos.size());
    std::vector<uint32_t> tris; std::vector<int> t2f; for (int f = 0; f < M.faceCount(); f++) for (int k = M.faceStart[size_t(f)] + 1; k + 1 < M.faceStart[size_t(f) + 1]; k++) { tris.push_back(uint32_t(M.faceVerts[size_t(M.faceStart[size_t(f)])])); tris.push_back(uint32_t(M.faceVerts[size_t(k)])); tris.push_back(uint32_t(M.faceVerts[size_t(k + 1)])); t2f.push_back(f); }
    QElapsedTimer t; t.start(); auto bvh = std::make_shared<topo::Bvh>(); bvh->build(M.pos, tris); std::printf("bvh %lld ms\n", t.elapsed());
    t.start(); shell::Report rep; std::vector<char> outer = shell::outerFaces(*bvh, tris, t2f, M.faceCount(), nullptr, &rep); std::printf("analysis %lld ms\n%s\n", t.elapsed(), rep.text.c_str());
    const Box3 b = M.bounds(); const Vec3 s = b.size();
    auto slice = [&](int ax, double frac, const QString& nm) {
      const double v = b.min[ax] + s[ax] * frac; const int u = ax == 1 ? 0 : 0, w = ax == 1 ? 2 : 1; const double sc2 = 1500 / std::max(s[u], s[w]); QImage im(int(s[u] * sc2) + 20, int(s[w] * sc2) + 20, QImage::Format_RGB32); im.fill(QColor(16, 16, 20)); QPainter p(&im);
      for (size_t i = 0; i < t2f.size(); i++) { const Vec3* T[3] = {&M.pos[tris[i * 3]], &M.pos[tris[i * 3 + 1]], &M.pos[tris[i * 3 + 2]]}; std::vector<QPointF> q;
        for (int j = 0; j < 3; j++) { const Vec3& A = *T[j]; const Vec3& B = *T[(j + 1) % 3]; if ((A[ax] < v) == (B[ax] < v)) continue; const double k = (v - A[ax]) / (B[ax] - A[ax]); const Vec3 P = A + (B - A) * k; q.push_back(QPointF((P[u] - b.min[u]) * sc2 + 10, im.height() - 10 - (P[w] - b.min[w]) * sc2)); }
        if (q.size() == 2) { p.setPen(QPen(outer[size_t(t2f[i])] ? QColor(90, 240, 120) : QColor(255, 70, 60), 1)); p.drawLine(q[0], q[1]); } }
      p.end(); im.save(dir + "/" + nm + ".png"); };
    auto zoom = [&](double fy, double cx, double cz, double half, const QString& nm) { const double v = b.min.y + s.y * fy, x0 = b.min.x + s.x * cx, z0 = b.min.z + s.z * cz, k = 700 / half; QImage im(1400, 1400, QImage::Format_RGB32); im.fill(QColor(16, 16, 20)); QPainter p(&im); p.setRenderHint(QPainter::Antialiasing);
      for (size_t i = 0; i < t2f.size(); i++) { const Vec3* T[3] = {&M.pos[tris[i * 3]], &M.pos[tris[i * 3 + 1]], &M.pos[tris[i * 3 + 2]]}; std::vector<QPointF> q; for (int j = 0; j < 3; j++) { const Vec3& A = *T[j]; const Vec3& B = *T[(j + 1) % 3]; if ((A.y < v) == (B.y < v)) continue; const double e = (v - A.y) / (B.y - A.y); const Vec3 Q = A + (B - A) * e; q.push_back(QPointF(700 + (Q.x - x0) * k, 700 - (Q.z - z0) * k)); } if (q.size() == 2) { p.setPen(QPen(outer[size_t(t2f[i])] ? QColor(90, 240, 120) : QColor(255, 70, 60), 2)); p.drawLine(q[0], q[1]); } }
      p.end(); im.save(dir + "/" + nm + ".png"); };
    zoom(0.72, 0.5, 1.0, 4, "chest"); zoom(0.72, 0.62, 0.9, 12, "chest2"); zoom(0.55, 0.5, 1.0, 6, "waist");
    slice(2, 0.5, "z50"); slice(2, 0.62, "z62");
  }
  return 0;
}
