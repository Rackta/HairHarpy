// Console check of the UV solvers: hm_uvtest <project.hairmesh> [min faces]. Every mesh with a UV: its seams made where
// its UV breaks, then its shells unfolded anew (Unfold, Unfold + optimize) and its own UV optimized — each measured:
// the triangles turned over, the stretch (the symmetric Dirichlet energy, area-weighted over the shells: 4 — none), time
#include "io/ProjectIO.h"
#include "io/MeshImport.h"
#include <QFile>
#include "scene/Topo.h"
#include "scene/Uv.h"
#include "scene/UvSolve.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QElapsedTimer>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>

using namespace hm;

namespace {
struct Score { int flips = 0, shells = 0, bad = 0; double sd = 0; };
// per shell (as the UV joins it): its UV points, triangles, the energy; the shells weighted by their 3D area
Score score(const PolyMesh& m, const topo::TTopo& T) {
  Score S; if (!uv::has(m)) return S;
  const uv::Points P = uv::points(m, T); const uv::Shells Sh = uv::shells(m, T, P);
  std::vector<std::vector<int>> by(size_t(Sh.count)); for (int f = 0; f < T.nf; f++) by[size_t(Sh.of[size_t(f)])].push_back(f);
  double wsum = 0;
  for (auto& fs : by) {
    std::unordered_map<int, int> loc; std::vector<Vec3> X; std::vector<uv::solve::P2> U; std::vector<uv::solve::Tri> tris;
    auto L = [&](int k) { const int p = P.of[size_t(k)]; auto r = loc.emplace(p, int(X.size())); if (r.second) { X.push_back(m.pos[size_t(T.fv[size_t(k)])]); U.push_back({m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]}); } return r.first->second; };
    double a3 = 0;
    for (int f : fs) { const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; std::vector<Vec3> ring; std::vector<int> li;
      for (int k = s; k < e; k++) { ring.push_back(m.pos[size_t(T.fv[size_t(k)])]); li.push_back(L(k)); }
      for (auto& t : uv::solve::triangulate(ring)) tris.push_back({li[size_t(t[0])], li[size_t(t[1])], li[size_t(t[2])]});
      a3 += uv::faceArea3(m, f); }
    const int fl = uv::solve::flips(U, X, tris); const double sd = uv::solve::distortion(U, X, tris);
    S.shells++; S.flips += fl; if (fl) S.bad++;
    if (std::isfinite(sd) && a3 > 0) { S.sd += sd * a3; wsum += a3; }
  }
  if (wsum > 0) S.sd /= wsum;
  return S;
}
void line(const char* what, const Score& s, qint64 ms) { std::printf("    %-22s flips %6d (in %4d of %4d shells)  stretch %8.4f  %6lld ms\n", what, s.flips, s.bad, s.shells, s.sd, ms); }
// the shells of `faces` rasterised at w × h texels, each grown by `grow` texels: the texels two shells share; and the
// texels outside the tile
std::pair<long, long> clashes(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, int w, int h, double grow) {
  const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P);
  std::vector<int> owner(size_t(w) * size_t(h), -1); long both = 0, out = 0; std::set<long> bothSet;
  for (int f : faces) { const int s = S.of[size_t(f)]; const int a = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
    for (int k = a + 1; k + 1 < e; k++) {
      std::array<std::array<double, 2>, 3> t; int i = 0; for (int c : {a, k, k + 1}) t[size_t(i++)] = {m.cornerUV[size_t(c)][0] * w, m.cornerUV[size_t(c)][1] * h};
      const int x0 = int(std::floor(std::min({t[0][0], t[1][0], t[2][0]}) - grow)), x1 = int(std::ceil(std::max({t[0][0], t[1][0], t[2][0]}) + grow));
      const int y0 = int(std::floor(std::min({t[0][1], t[1][1], t[2][1]}) - grow)), y1 = int(std::ceil(std::max({t[0][1], t[1][1], t[2][1]}) + grow));
      for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const std::array<double, 2> p{x + 0.5, y + 0.5};
        auto cr = [](const std::array<double, 2>& o, const std::array<double, 2>& u, const std::array<double, 2>& v) { return (u[0] - o[0]) * (v[1] - o[1]) - (u[1] - o[1]) * (v[0] - o[0]); };
        const double d1 = cr(t[0], t[1], p), d2 = cr(t[1], t[2], p), d3 = cr(t[2], t[0], p); bool in = (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
        if (!in && grow > 0) { auto seg = [&](const std::array<double, 2>& u, const std::array<double, 2>& v) { const double dx = v[0] - u[0], dy = v[1] - u[1], L = dx * dx + dy * dy; double q = L > 0 ? ((p[0] - u[0]) * dx + (p[1] - u[1]) * dy) / L : 0; q = std::clamp(q, 0.0, 1.0); return std::hypot(p[0] - u[0] - q * dx, p[1] - u[1] - q * dy); };
          in = std::min({seg(t[0], t[1]), seg(t[1], t[2]), seg(t[2], t[0])}) <= grow; }
        if (!in) continue;
        if (x < 0 || y < 0 || x >= w || y >= h) { out++; continue; }
        int& o = owner[size_t(y) * size_t(w) + size_t(x)]; if (o < 0) o = s; else if (o != s && bothSet.insert(long(y) * w + x).second) both++; } } }
  return {both, out};
}
void packLine(const char* what, const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& fs, int w, int h, double pad, double fill, qint64 ms) {
  const auto ov = clashes(m, T, fs, w / 4, h / 4, 0), pd = clashes(m, T, fs, w, h, pad / 2 - 0.5);   // (overlaps at a quarter of the size; the padding at full)
  std::printf("    %-24s fill %5.1f%%  overlap texels %6ld  padding broken texels %6ld (out of the tile %ld)  %6lld ms\n", what, fill * 100, ov.first, pd.first, pd.second, ms);
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc < 2) { std::printf("usage: hm_uvtest <project.hairmesh> [min faces]\n"); return 2; }
  const int minFaces = argc > 2 ? std::atoi(argv[2]) : 0;
  if (argc > 3 && QString(argv[3]) == "mkicon") {              // (the Rectangle button's picture: a curved strand → one orange strip — made like the Strands all one)
    const int S = 2000; QImage im(S, S, QImage::Format_ARGB32); im.fill(Qt::transparent); QPainter p(&im); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(QColor(0x21, 0x21, 0x21)); p.drawRoundedRect(QRectF(0, 0, S, S), 300, 300);
    QPen dp(QColor(0x78, 0x78, 0x78), 36); dp.setCapStyle(Qt::RoundCap); dp.setStyle(Qt::CustomDashLine); dp.setDashPattern({2.2, 2.4}); p.setPen(dp); p.setBrush(Qt::NoBrush);
    QPainterPath a; a.moveTo(430, 420); a.cubicTo(780, 800, 120, 1080, 380, 1520);       // (the left side of a bent strand)
    QPainterPath b; b.moveTo(900, 420); b.cubicTo(1130, 800, 480, 1080, 860, 1520);       // (the right side)
    p.drawPath(a); p.drawPath(b); QPainterPath t; t.moveTo(430, 420); t.lineTo(900, 420); QPainterPath u; u.moveTo(360, 1520); u.lineTo(840, 1520); p.drawPath(t); p.drawPath(u);
    p.setPen(Qt::NoPen); p.setBrush(QColor(0xf2, 0x70, 0x1c)); p.drawRoundedRect(QRectF(1180, 405, 500, 1120), 34, 34);
    QPen lp(QColor(0x21, 0x21, 0x21), 30); lp.setCapStyle(Qt::RoundCap); p.setPen(lp); for (int y : {685, 965, 1245}) p.drawLine(QPointF(1200, y), QPointF(1660, y));
    QPen ap(QColor(0xf2, 0x70, 0x1c), 44); ap.setCapStyle(Qt::RoundCap); ap.setJoinStyle(Qt::RoundJoin); p.setPen(ap); p.drawLine(QPointF(940, 965), QPointF(1090, 965)); p.drawLine(QPointF(1090, 965), QPointF(985, 880)); p.drawLine(QPointF(1090, 965), QPointF(985, 1050));
    p.end(); return im.save(QString::fromLocal8Bit(argv[1])) ? 0 : 1; }
  Scene sc; LoadResult r = loadProject(QString::fromLocal8Bit(argv[1]), sc);
  if (!r.ok) { std::printf("load failed: %s\n", qPrintable(r.error)); return 1; }
  if (argc > 6 && QString(argv[3]) == "alignpng") {           // (the h / v constraints: the shell of a polygon as it is, unfolded with them, unfolded free — pictures, stretch, how straight). args: name face out-prefix
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue;
      auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp); const int f0 = std::atoi(argv[5]); const QString out = QString::fromLocal8Bit(argv[6]);
      uv::Pins pins, plain; for (const QJsonValue& x : it.raw.value("uvPins").toArray()) { const QJsonArray a = x.toArray(); if (a.size() < 2) continue; const int p = a[0].toInt(), q = a[1].toInt(); const QString t = a.size() >= 3 ? a[2].toString() : QString(); int f = a.size() >= 4 ? a[3].toInt(-1) : -1;
        if (p == q) { if (f >= 0) pins.insert({-1 - f, -1 - p}); else pins.insert({p, p}); continue; }
        const int e = T->edgeOf(p, q); if (e < 0) continue; if (f < 0) f = T->ef0[size_t(e)] >= 0 ? T->ef0[size_t(e)] : T->ef1[size_t(e)];
        if (t == "h" || t == "v") pins.insert({-1 - f, (t == "h" ? uv::kAlignH : uv::kAlignV) + e}); else pins.insert({-1 - f, e}); }
      for (auto& k : pins) if (k.second < uv::kAlignH) plain.insert(k);
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P);
      if (f0 < 0) { std::map<int, std::array<int, 3>> by; for (int f = 0; f < T->nf; f++) { auto& c = by[S.of[size_t(f)]]; if (!c[2]) c[2] = f + 1; c[0]++; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) if (uv::edgeAligned(pins, f, T->ce[size_t(k)])) c[1]++; }   // (-1: the shells with constraints — their size, a polygon of each)
        for (auto& [s, c] : by) if (c[1]) std::printf("shell %d: %d faces, %d aligned corners, face %d\n", s, c[0], c[1], c[2] - 1); return 0; }
      std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (S.of[size_t(f)] == S.of[size_t(f0)]) fs.push_back(f);
      auto shellSd = [&](const PolyMesh& m) { std::unordered_map<int, int> loc; std::vector<Vec3> X; std::vector<uv::solve::P2> U; std::vector<uv::solve::Tri> tris; const uv::Points PP = uv::points(m, *T);
        auto L = [&](int k) { auto r = loc.emplace(PP.of[size_t(k)], int(X.size())); if (r.second) { X.push_back(m.pos[size_t(T->fv[size_t(k)])]); U.push_back({m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]}); } return r.first->second; };
        for (int f : fs) { std::vector<Vec3> ring; std::vector<int> li; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { ring.push_back(m.pos[size_t(T->fv[size_t(k)])]); li.push_back(L(k)); } for (auto& t : uv::solve::triangulate(ring)) tris.push_back({li[size_t(t[0])], li[size_t(t[1])], li[size_t(t[2])]}); }
        if (std::getenv("ALIGNDUMP")) std::printf("    (flips %d of %zu)\n", uv::solve::flips(U, X, tris), tris.size()); return uv::solve::distortion(U, X, tris); };
      auto worst = [&](const PolyMesh& m) { double w = 0; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const int al = uv::edgeAligned(pins, f, T->ce[size_t(k)]); if (!al) continue;
        const auto &p = m.cornerUV[size_t(k)], &q = m.cornerUV[size_t(nk)]; const double du = std::abs(double(p[0]) - q[0]), dv = std::abs(double(p[1]) - q[1]), L = std::hypot(du, dv); if (L > 0) w = std::max(w, (al == 1 ? dv : du) / L); } return w; };
      auto draw = [&](const PolyMesh& m, const QString& file) { double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { x0 = std::min(x0, double(m.cornerUV[size_t(k)][0])); x1 = std::max(x1, double(m.cornerUV[size_t(k)][0])); y0 = std::min(y0, double(m.cornerUV[size_t(k)][1])); y1 = std::max(y1, double(m.cornerUV[size_t(k)][1])); }
        const int W = 900; const double s = (W - 40) / std::max(x1 - x0, y1 - y0); QImage im(W, W, QImage::Format_RGB32); im.fill(QColor(40, 40, 40)); QPainter pa(&im); pa.setRenderHint(QPainter::Antialiasing);
        auto pt = [&](int k) { return QPointF(20 + (m.cornerUV[size_t(k)][0] - x0) * s, W - 20 - (m.cornerUV[size_t(k)][1] - y0) * s); };
        for (int f : fs) { QPolygonF pg; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) pg << pt(k); pa.setPen(QPen(QColor(200, 200, 220), 1)); pa.setBrush(QColor(150, 90, 150)); pa.drawPolygon(pg); }
        for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const int al = uv::edgeAligned(pins, f, T->ce[size_t(k)]); if (!al) continue; pa.setPen(QPen(al == 1 ? QColor(60, 180, 255) : QColor(90, 220, 100), 4)); pa.drawLine(pt(k), pt(nk)); }
        pa.end(); im.save(file); };
      std::printf("shell of %d: %zu faces\n", f0, fs.size());
      if (std::getenv("ALIGNDUMP")) for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const int al = uv::edgeAligned(pins, f, T->ce[size_t(k)]); if (!al) continue;   // (each constrained edge: its kind, its ends in 3D, its UV)
        const Vec3 a = sp->pos[size_t(T->fv[size_t(k)])], b = sp->pos[size_t(T->fv[size_t(nk)])]; std::printf("  %c e%d v%d-v%d  (%.2f %.2f %.2f)-(%.2f %.2f %.2f)  uv (%.4f %.4f)-(%.4f %.4f) seam-border %d\n", al == 1 ? 'h' : 'v', T->ce[size_t(k)], T->fv[size_t(k)], T->fv[size_t(nk)], a.x, a.y, a.z, b.x, b.y, b.z, sp->cornerUV[size_t(k)][0], sp->cornerUV[size_t(k)][1], sp->cornerUV[size_t(nk)][0], sp->cornerUV[size_t(nk)][1], T->ef1[size_t(T->ce[size_t(k)])] < 0 ? 1 : 0); }
      std::printf("  as it is   stretch %.4f  worst off %.5f\n", shellSd(*sp), worst(*sp)); draw(*sp, out + "_0.png");
      PolyMesh a = *sp; uv::unfold(a, *T, fs, true, true, &pins); std::printf("  aligned    stretch %.4f  worst off %.5f\n", shellSd(a), worst(a)); draw(a, out + "_1.png");
      PolyMesh b = *sp; uv::unfold(b, *T, fs, true, true, &plain); std::printf("  free       stretch %.4f  worst off %.5f\n", shellSd(b), worst(b)); draw(b, out + "_2.png");
      { PolyMesh c = *sp; uv::SymUnfold su; su.mc = uv::mirrorCorners(c, *T); const auto r = uv::unfold(c, *T, fs, true, true, &pins, &su); std::printf("  aligned sym stretch %.4f  worst off %.5f  (symmetric %d)\n", shellSd(c), worst(c), r.symmetric); draw(c, out + "_3.png"); }
      { PolyMesh c = *sp; uv::SymUnfold su; su.mc = uv::mirrorCorners(c, *T); su.pairs = true; su.axis = 0.5;   // (as the app's U with Symmetry on, three times in a row: each from the one before)
        auto boxOf = [&](const PolyMesh& mm) { double a0 = 1e9, a1 = -1e9, b0 = 1e9, b1 = -1e9; for (int k : uv::faceCorners(*T, fs)) { a0 = std::min(a0, double(mm.cornerUV[size_t(k)][0])); a1 = std::max(a1, double(mm.cornerUV[size_t(k)][0])); b0 = std::min(b0, double(mm.cornerUV[size_t(k)][1])); b1 = std::max(b1, double(mm.cornerUV[size_t(k)][1])); } return std::array<double, 4>{a0, a1, b0, b1}; };
        { const auto b = boxOf(c); std::printf("  box before: U %.4f..%.4f V %.4f..%.4f\n", b[0], b[1], b[2], b[3]); }
        uv::PackOpts poA; poA.w = poA.h = 4096;
        for (int n = 1; n <= 3; n++) { const auto r = uv::unfold(c, *T, fs, true, true, &pins, &su, &poA); { const auto b = boxOf(c); std::printf("  box after U #%d: U %.4f..%.4f V %.4f..%.4f\n", n, b[0], b[1], b[2], b[3]); } uv::CheckOpts co; co.pad = 0; const auto rep = uv::check(c, *T, fs, co); std::printf("  app U #%d  stretch %.4f  worst off %.5f  overlap %zu flipped %zu  (done %d mirrored %d mended %d untangled %d)\n", n, shellSd(c), worst(c), rep.overlap.size(), rep.flipped.size(), r.done, r.mirrored, r.mended, r.untangled); draw(c, out + QString("_5_%1.png").arg(n)); } }
      { PolyMesh c = *sp; uv::optimize(c, *T, fs, &pins); std::printf("  optimized  stretch %.4f  worst off %.5f\n", shellSd(c), worst(c)); draw(c, out + "_4.png"); }
      return 0; }
    return 1; }
  if (argc > 3 && QString(argv[3]) == "items") {                 // (every mesh: id, name, polygons, its UV sets — names and how many polygons each, the raw "uvSets")
    for (auto& [id, it] : sc.items) { if (!it.poly) continue; const PolyMesh& B = *(it.base ? it.base : it.poly);
      QStringList s; if (!B.uvSetNames.empty() && int(B.faceSets.size()) == B.faceCount()) for (size_t k = 0; k < B.uvSetNames.size(); k++) { int n = 0; for (uint32_t m : B.faceSets) n += int(m >> k & 1u); s << QString("%1:%2").arg(B.uvSetNames[k]).arg(n); }
      QStringList r; for (const QJsonValue& v : it.raw.value("uvSets").toArray()) r << v.toObject().value("name").toString();
      std::printf("%d %s: %d polygons, uv %s, sets [%s], raw uvSets [%s], mats %zu\n", id, it.name.toUtf8().constData(), B.faceCount(), B.cornerUV.size() == B.faceVerts.size() ? "yes" : "no", qPrintable(s.join(" ")), qPrintable(r.join(", ")), B.matNames.size()); }
    return 0;
  }
  if (argc > 4 && QString(argv[3]) == "normals") {               // (a mesh's corner normals against its polygons' winding: how many polygons have them turned the other way). args: name
    for (auto& [id, it] : sc.items) { if (!it.poly || (it.name != QString(argv[4]) && QString(argv[4]) != "#" + QString::number(id))) continue; const PolyMesh& B = *(it.base ? it.base : it.poly);
      const bool cn = B.cornerNormal.size() == B.faceVerts.size(); int against = 0, with = 0; Vec3 sumN;
      for (int f = 0; f < B.faceCount(); f++) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; Vec3 n;
        for (int k = s; k < e; k++) { const Vec3& a = B.pos[size_t(B.faceVerts[size_t(k)])]; const Vec3& b = B.pos[size_t(B.faceVerts[size_t(k + 1 < e ? k + 1 : s)])]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
        sumN = sumN + n; if (!cn) continue; double d = 0; for (int k = s; k < e; k++) { const auto& q = B.cornerNormal[size_t(k)]; d += n.x * q[0] + n.y * q[1] + n.z * q[2]; } if (d < 0) against++; else with++; }
      std::printf("%d %s: %d polygons, corner normals %s — %d along the winding, %d against; the winding's sum %.3f %.3f %.3f\n", id, qPrintable(it.name), B.faceCount(), cn ? "kept" : "none (made)", with, against, sumN.x, sumN.y, sumN.z); }
    return 0;
  }
  if (argc > 5 && QString(argv[3]) == "padcheck") {             // (the shells of these meshes in one tile: the pairs closer than the padding, in px). args: name,name,... texPx padPx
    const QStringList names = QString(argv[4]).split(','); const double tex = argc > 5 ? std::atof(argv[5]) : 2048, pad = argc > 6 ? std::atof(argv[6]) : 8;
    struct Seg { double ax, ay, bx, by; int sh; }; std::vector<Seg> segs; int shBase = 0;
    for (auto& [id, it] : sc.items) { if (!it.poly || !names.contains(it.name)) continue; auto sp = std::make_shared<PolyMesh>(*(it.base ? it.base : it.poly)); auto T = topo::ttopo(sp); if (!T || !uv::has(*sp)) continue;
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P);
      for (int e = 0; e < T->ne; e++) { if (uv::continuous(*sp, *T, P, e) && T->ef1[size_t(e)] >= 0) continue;
        for (int f : {T->ef0[size_t(e)], T->ef1[size_t(e)]}) { if (f < 0) continue; const int a = uv::cornerAt(*T, f, T->ea[size_t(e)]), b = uv::cornerAt(*T, f, T->eb[size_t(e)]); if (a < 0 || b < 0) continue;
          segs.push_back({sp->cornerUV[size_t(a)][0] * tex, sp->cornerUV[size_t(a)][1] * tex, sp->cornerUV[size_t(b)][0] * tex, sp->cornerUV[size_t(b)][1] * tex, shBase + S.of[size_t(f)]}); } }
      shBase += S.count; }
    auto ptSeg = [](double px, double py, const Seg& s) { const double dx = s.bx - s.ax, dy = s.by - s.ay, l = dx * dx + dy * dy, t = l > 0 ? std::clamp(((px - s.ax) * dx + (py - s.ay) * dy) / l, 0.0, 1.0) : 0.0; return std::hypot(px - s.ax - t * dx, py - s.ay - t * dy); };
    const double g = 32; std::unordered_map<long long, std::vector<int>> grid; for (int i = 0; i < int(segs.size()); i++) { const auto& s = segs[size_t(i)];
      for (int x = int(std::floor((std::min(s.ax, s.bx) - pad) / g)); x <= int(std::floor((std::max(s.ax, s.bx) + pad) / g)); x++) for (int y = int(std::floor((std::min(s.ay, s.by) - pad) / g)); y <= int(std::floor((std::max(s.ay, s.by) + pad) / g)); y++) grid[(long long)x * 1000003LL + y].push_back(i); }
    std::map<std::pair<int, int>, double> pairD;
    for (auto& [k, v] : grid) for (size_t a = 0; a < v.size(); a++) for (size_t b = a + 1; b < v.size(); b++) { const Seg& p = segs[size_t(v[a])]; const Seg& q = segs[size_t(v[b])]; if (p.sh == q.sh) continue;
      const double d = std::min({ptSeg(p.ax, p.ay, q), ptSeg(p.bx, p.by, q), ptSeg(q.ax, q.ay, p), ptSeg(q.bx, q.by, p)}); if (d < pad - 0.5) { auto key = std::make_pair(std::min(p.sh, q.sh), std::max(p.sh, q.sh)); auto f2 = pairD.find(key); if (f2 == pairD.end()) pairD[key] = d; else f2->second = std::min(f2->second, d); } }
    std::map<int, std::array<double, 4>> box; for (const Seg& s : segs) { auto& b = box.emplace(s.sh, std::array<double, 4>{1e300, 1e300, -1e300, -1e300}).first->second; b[0] = std::min({b[0], s.ax, s.bx}); b[1] = std::min({b[1], s.ay, s.by}); b[2] = std::max({b[2], s.ax, s.bx}); b[3] = std::max({b[3], s.ay, s.by}); }
    int stacked = 0, touch = 0, near = 0; double nmin = 1e300;
    for (auto& [k, d] : pairD) { const auto& A = box[k.first]; const auto& B = box[k.second]; const bool same = std::abs(A[0] - B[0]) < 1.5 && std::abs(A[1] - B[1]) < 1.5 && std::abs(A[2] - B[2]) < 1.5 && std::abs(A[3] - B[3]) < 1.5;
      if (d < 0.5 && same) stacked++; else if (d < 0.5) { touch++; if (std::getenv("PAIRS")) std::printf("  touching: shell %d (%.0f..%.0f, %.0f..%.0f) and %d (%.0f..%.0f, %.0f..%.0f)\n", k.first, A[0], A[2], A[1], A[3], k.second, B[0], B[2], B[1], B[3]); }
      else { near++; nmin = std::min(nmin, d); if (std::getenv("PAIRS")) std::printf("  %.2f px: shell %d (%.0f..%.0f, %.0f..%.0f) and %d (%.0f..%.0f, %.0f..%.0f)\n", d, k.first, A[0], A[2], A[1], A[3], k.second, B[0], B[2], B[1], B[3]); } }
    std::printf("%d shells: %d stacked (alike, on each other), %d pairs touching or overlapping, %d pairs closer than %.0f px (closest %.2f px)\n", shBase, stacked, touch, near, pad, near ? nmin : 0.0);
    return 0;
  }
  if (argc > 4 && QString(argv[3]) == "attrs") {                // (each shell setting: its key polygon, the shell it is in, that shell's size). args: name
    for (auto& [id, it] : sc.items) { if (!it.poly || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*(it.base ? it.base : it.poly)); auto T = topo::ttopo(sp); if (!T || !uv::has(*sp)) continue;
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P); std::vector<int> sz(static_cast<size_t>(S.count), 0); for (int f = 0; f < T->nf; f++) sz[size_t(S.of[size_t(f)])]++;
      std::map<QString, int> byKey; for (int f = 0; f < T->nf; f++) { std::vector<int> v(T->fv.begin() + T->fo[size_t(f)], T->fv.begin() + T->fo[size_t(f) + 1]); std::sort(v.begin(), v.end()); QStringList l; for (int x : v) l << QString::number(x); byKey[l.join(',')] = f; }
      for (const QJsonValue& x : it.raw.value("uvShellAttr").toArray()) { const QJsonObject o = x.toObject(); QStringList l; for (const QJsonValue& q : o.value("k").toArray()) l << QString::number(q.toInt());
        auto g = byKey.find(l.join(',')); if (g == byKey.end()) { std::printf("  group %d: key %s — no such polygon\n", o.value("group").toInt(), qPrintable(l.join(','))); continue; }
        std::printf("  group %d: polygon %d, shell %d of %d polygons\n", o.value("group").toInt(), g->second, S.of[size_t(g->second)], sz[size_t(S.of[size_t(g->second)])]); } }
    return 0;
  }
  if (argc > 4 && QString(argv[3]) == "raw") {                  // (an item's raw keys, the source left out). args: id
    if (const Item* it = sc.item(std::atoi(argv[4]))) { QJsonObject o = it->raw; o.remove("src"); o.remove("selSets"); std::printf("%d %s type %d parent %d: %s\n", it->id, qPrintable(it->name), int(it->type), it->parent, QJsonDocument(o).toJson(QJsonDocument::Compact).left(3000).constData()); }
    return 0;
  }
  if (argc > 4 && QString(argv[3]) == "bounds") {               // (where a mesh is: its source (base) and as shown (poly), its raw transform keys). args: name
    for (auto& [id, it] : sc.items) { if (!it.poly || it.name != QString(argv[4])) continue;
      auto pb = [](const char* w, const PolyMesh* p) { if (!p) { std::printf("  %s: none\n", w); return; } const auto b = p->bounds(); const Vec3 c = b.center(), s = b.size(); std::printf("  %s: centre %.3f %.3f %.3f size %.3f %.3f %.3f\n", w, c.x, c.y, c.z, s.x, s.y, s.z); };
      std::printf("%d %s parent %d\n", id, qPrintable(it.name), it.parent); pb("base", it.base.get()); pb("poly", it.poly.get());
      for (const QString& k : {"pivot", "xf", "pos", "rot", "scl", "src"}) if (it.raw.contains(k)) std::printf("  raw.%s = %s\n", qPrintable(k), QJsonDocument(QJsonObject{{k, it.raw.value(k)}}).toJson(QJsonDocument::Compact).constData());
      for (auto& [cid, ci] : sc.items) if (ci.parent == id) std::printf("  child %d %s kind %s p %s\n", cid, qPrintable(ci.name), qPrintable(ci.raw.value("kind").toString()), QJsonDocument(ci.raw.value("p").toObject()).toJson(QJsonDocument::Compact).constData()); }
    return 0;
  }
  if (argc > 4 && QString(argv[3]) == "symerr") {               // (the shells symmetric in themselves: how far each one's UV is from symmetric, % of its size). args: name
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*(it.base ? it.base : it.poly)); auto T = topo::ttopo(sp); if (!T) continue;
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P); std::vector<std::vector<int>> by(static_cast<size_t>(S.count)); for (int f = 0; f < T->nf; f++) by[size_t(S.of[size_t(f)])].push_back(f);
      double worst = 0, sum = 0; int n = 0; for (int s = 0; s < S.count; s++) { if (const char* on = std::getenv("ONLY"); on && s != std::atoi(on)) continue; const double e = uv::selfSymError(*sp, *T, by[size_t(s)]); if (e < 0) continue; n++; sum += e; worst = std::max(worst, e); std::printf("  shell %d (%zu polygons): %.3f%%\n", s, by[size_t(s)].size(), e * 100); }
      std::printf("%s: %d symmetric in themselves, asymmetry mean %.3f%%, worst %.3f%%\n", qPrintable(it.name), n, n ? sum / n * 100 : 0, worst * 100); }
    return 0;
  }
  if (argc > 5 && QString(argv[3]) == "uvdiff") {               // (the vertices whose UV moved between this project and another). args: other.hairmesh name
    Scene sc2; if (!loadProject(QString::fromLocal8Bit(argv[4]), sc2).ok) { std::printf("cannot load %s\n", argv[4]); return 1; }
    for (auto& [id, it] : sc.items) { if (!it.poly || it.name != QString(argv[5])) continue; const Item* o = sc2.item(id); if (!o || !o->poly) continue;
      const PolyMesh& A = *(it.base ? it.base : it.poly); const PolyMesh& B2 = *(o->base ? o->base : o->poly); if (A.cornerUV.size() != B2.cornerUV.size()) { std::printf("corner counts differ\n"); continue; }
      std::set<int> moved; for (size_t k = 0; k < A.cornerUV.size(); k++) if (std::abs(A.cornerUV[k][0] - B2.cornerUV[k][0]) > 1e-6f || std::abs(A.cornerUV[k][1] - B2.cornerUV[k][1]) > 1e-6f) moved.insert(A.faceVerts[k]);
      QStringList l; for (int v : moved) l << QString::number(v); std::printf("%s: %zu vertices moved: %s\n", qPrintable(it.name), moved.size(), qPrintable(l.mid(0, 40).join(","))); }
    return 0;
  }
  if (argc > 3 && QString(argv[3]) == "livestat") {             // (each mesh's UV sets: polygons, how many show another set's layout, the overlaps with cornerUV as it is and with the set's own UV). args: [set name]
    for (auto& [id, it] : sc.items) { if (!it.poly) continue; const PolyMesh& B0 = *(it.base ? it.base : it.poly); if (B0.uvSetNames.empty() || int(B0.faceSets.size()) != B0.faceCount() || B0.cornerUV.size() != B0.faceVerts.size()) continue;
      auto sp = std::make_shared<PolyMesh>(B0); auto T = topo::ttopo(sp); if (!T) continue;
      { std::vector<int> all(size_t(T->nf)); std::iota(all.begin(), all.end(), 0); uv::CheckOpts c0; c0.pad = 0; std::printf("%d %s: all %d polygons as one tile — overlap %zu\n", id, qPrintable(it.name), T->nf, uv::check(*sp, *T, all, c0).overlap.size()); }
      for (size_t s = 0; s < B0.uvSetNames.size(); s++) { if (argc > 4 && B0.uvSetNames[s] != QString(argv[4])) continue;
        std::vector<int> fs; int notLive = 0; for (int f = 0; f < B0.faceCount(); f++) if (B0.faceSets[size_t(f)] >> s & 1u) { fs.push_back(f); if (B0.faceLive[size_t(f)] != int(s)) notLive++; }
        if (fs.empty()) continue; uv::CheckOpts co; co.pad = 0; const auto r1 = uv::check(*sp, *T, fs, co);
        PolyMesh own = B0; if (s < B0.setUV.size() && B0.setUV[s].size() == own.cornerUV.size()) for (int f : fs) if (B0.faceLive[size_t(f)] != int(s)) for (int k = B0.faceStart[size_t(f)]; k < B0.faceStart[size_t(f) + 1]; k++) own.cornerUV[size_t(k)] = B0.setUV[s][size_t(k)];
        const auto r2 = uv::check(own, *T, fs, co);
        std::printf("%d %s / %s: %zu polygons, %d show another set, overlap as shown %zu, overlap in the set's own UV %zu\n", id, qPrintable(it.name), qPrintable(B0.uvSetNames[s]), fs.size(), notLive, r1.overlap.size(), r2.overlap.size()); } }
    return 0;
  }
  if (argc > 4 && QString(argv[3]) == "heal") {                 // (the tears sewn back: how many, and the hidden cuts left). args: name [out.hairmesh — the project saved so]
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*(it.base ? it.base : it.poly)); auto T = topo::ttopo(sp);
      auto count = [&](const PolyMesh& mm) { const uv::Points P = uv::points(mm, *T); const auto fl = uv::seamFlags(mm, *T); int g = 0; for (int e = 0; e < T->ne; e++) { const int a = T->ef0[size_t(e)], b = T->ef1[size_t(e)]; if (b < 0 || fl[size_t(e)] || uv::continuous(mm, *T, P, e)) continue; const int la = size_t(a) < mm.faceLive.size() ? mm.faceLive[size_t(a)] : -9, lb = size_t(b) < mm.faceLive.size() ? mm.faceLive[size_t(b)] : -9; if (la == lb) g++; } return g; };
      std::printf("hidden cuts before: %d\n", count(*sp)); const int n = uv::healTears(*sp, *T, {}); std::printf("pieces set back: %d; hidden cuts after: %d; flipped %zu\n", n, count(*sp), uv::flipped(*sp, *T, uv::shells(*sp, *T, uv::points(*sp, *T))).size());
      return 0; }
    return 1; }
  if (argc > 4 && QString(argv[3]) == "smallshells") {          // (the UV shells of up to N polygons: their size, group, where, and their border — seams, mesh border, hidden cuts). args: name [N]
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp);
      const int N = argc > 5 ? std::atoi(argv[5]) : 300; const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P); const auto fl = uv::seamFlags(*sp, *T);
      std::map<int, std::vector<int>> by; for (int f = 0; f < T->nf; f++) by[S.of[size_t(f)]].push_back(f);
      for (auto& [s, fs] : by) { if (int(fs.size()) > N) continue; std::set<int> in(fs.begin(), fs.end()); int seam = 0, rim = 0, hid = 0; Vec3 c; int nc = 0;
        for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { c += sp->pos[size_t(T->fv[size_t(k)])]; nc++; const int e = T->ce[size_t(k)]; const int g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)];
          if (g < 0) rim++; else if (in.count(g)) continue; else if (fl[size_t(e)]) seam++; else hid++; }
        c = c * (1.0 / std::max(1, nc)); const int g0 = size_t(fs[0]) < sp->faceGroup.size() ? sp->faceGroup[size_t(fs[0])] : -1;
        std::printf("shell %d: %zu polygons (face %d), group %s, at %.1f %.1f %.1f — border: %d seam, %d mesh rim, %d hidden cut, live set %d\n", s, fs.size(), fs[0], g0 >= 0 && size_t(g0) < sp->groupNames.size() ? qPrintable(sp->groupNames[size_t(g0)]) : "-", c.x, c.y, c.z, seam, rim, hid, size_t(fs[0]) < sp->faceLive.size() ? sp->faceLive[size_t(fs[0])] : -9); }
      return 0; }
    return 1; }
  if (argc > 4 && QString(argv[3]) == "flatuv") {              // (the polygons with no UV area — by group, by set; their pieces along the seams unfolded as the app does, with the symmetry — what comes out). args: name
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp);
      std::vector<int> flat; std::map<int, int> byG; for (int f = 0; f < T->nf; f++) if (std::abs(uv::faceArea(*sp, f)) < 1e-12) { flat.push_back(f); byG[size_t(f) < sp->faceGroup.size() ? sp->faceGroup[size_t(f)] : -1]++; }
      std::printf("%zu polygons with no UV area:", flat.size()); for (auto& [g, c] : byG) std::printf(" group %d (%s): %d", g, g >= 0 && size_t(g) < sp->groupNames.size() ? qPrintable(sp->groupNames[size_t(g)]) : "-", c); std::printf("\n");
      if (const char* pc = std::getenv("PIECE")) { const auto fl0 = uv::seamFlags(*sp, *T); std::vector<int> par(size_t(T->nf)); std::iota(par.begin(), par.end(), 0); std::function<int(int)> fd = [&](int x) { return par[size_t(x)] == x ? x : par[size_t(x)] = fd(par[size_t(x)]); };
        for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] >= 0 && !fl0[size_t(e)]) par[size_t(fd(T->ef0[size_t(e)]))] = fd(T->ef1[size_t(e)]);
        const int r0 = fd(std::atoi(pc)); flat.clear(); for (int f = 0; f < T->nf; f++) if (fd(f) == r0) flat.push_back(f); std::printf("  the piece of face %s: %zu polygons\n", pc, flat.size()); }
      if (flat.empty()) return 0;
      { std::set<std::pair<float, float>> pts; for (int k : uv::faceCorners(*T, flat)) pts.insert({sp->cornerUV[size_t(k)][0], sp->cornerUV[size_t(k)][1]}); std::printf("  their corners: %zu distinct UV points, e.g. %.4f %.4f\n", pts.size(), pts.begin()->first, pts.begin()->second); }
      for (int sym = 0; sym < 2; sym++) { PolyMesh c = *sp; uv::SymUnfold su; su.mc = uv::mirrorCorners(c, *T); su.pairs = true; su.axis = 0.5; uv::PackOpts po; po.w = po.h = 4096;
        const auto r = uv::unfold(c, *T, flat, true, true, nullptr, sym ? &su : nullptr, &po); int still = 0; for (int f : flat) if (std::abs(uv::faceArea(c, f)) < 1e-12) still++;
        { double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9; for (int k : uv::faceCorners(*T, flat)) { x0 = std::min(x0, double(c.cornerUV[size_t(k)][0])); x1 = std::max(x1, double(c.cornerUV[size_t(k)][0])); y0 = std::min(y0, double(c.cornerUV[size_t(k)][1])); y1 = std::max(y1, double(c.cornerUV[size_t(k)][1])); }
          std::printf("  -> box %.4f..%.4f x %.4f..%.4f; a face %d: area %.3g, 3D %.3g\n", x0, x1, y0, y1, flat[0], uv::faceArea(c, flat[0]), uv::faceArea3(c, flat[0])); }
        std::printf("  unfolded %s: done %d mirrored %d asymPair %d collapsed %d closed %d holes %d — %d still with no area\n", sym ? "with the symmetry" : "alone", r.done, r.mirrored, r.asymPair, r.collapsed, r.closed, r.holes, still);
        if (sym) { const auto fl = uv::seamFlags(c, *T); std::vector<int> par(size_t(T->nf)); std::iota(par.begin(), par.end(), 0); std::function<int(int)> fd = [&](int x) { return par[size_t(x)] == x ? x : par[size_t(x)] = fd(par[size_t(x)]); };
          for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] >= 0 && !fl[size_t(e)]) par[size_t(fd(T->ef0[size_t(e)]))] = fd(T->ef1[size_t(e)]);
          std::map<int, std::array<int, 3>> pc; for (int f : flat) if (std::abs(uv::faceArea(c, f)) < 1e-12) { auto& a = pc[fd(f)]; a[0]++; a[2] = f; } for (int f = 0; f < T->nf; f++) if (pc.count(fd(f))) pc[fd(f)][1]++;
          for (auto& [p, a] : pc) { int brd = 0; std::set<int> vs; for (int f = 0; f < T->nf; f++) if (fd(f) == p) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { vs.insert(T->fv[size_t(k)]); const int e = T->ce[size_t(k)]; if (T->ef1[size_t(e)] < 0 || fl[size_t(e)]) brd++; }
            const int g = size_t(a[2]) < c.faceGroup.size() ? c.faceGroup[size_t(a[2])] : -1; std::printf("    piece of face %d: %d of its %d polygons with no area, %zu vertices, %d border sides, group %s, live set %d\n", a[2], a[0], a[1], vs.size(), brd, g >= 0 && size_t(g) < c.groupNames.size() ? qPrintable(c.groupNames[size_t(g)]) : "-", size_t(a[2]) < c.faceLive.size() ? c.faceLive[size_t(a[2])] : -9); } } }
      return 0; }
    return 1; }
  if (argc > 4 && QString(argv[3]) == "toposym") {             // (the mirror pairs grown along the connections: how many, where, how far off their mirror place, by group and side — a dry run). args: name [maxDist]
    for (auto& [id, it] : sc.items) { if (!it.poly || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*(it.base ? it.base : it.poly)); auto T = topo::ttopo(sp);
      auto pv = topo::symPairs(*sp, sp->pos); int before = 0; for (int x : pv) if (x < 0) before++;
      const double md = argc > 5 ? std::atof(argv[5]) : 0; const auto old = pv; const int add = topo::symPairsByLinks(*T, sp->pos, pv, md); int after = 0; for (int x : pv) if (x < 0) after++;
      std::vector<int> vg(sp->pos.size(), -1); for (int f = 0; f < T->nf; f++) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vg[size_t(T->fv[size_t(k)])] = size_t(f) < sp->faceGroup.size() ? sp->faceGroup[size_t(f)] : -1;
      struct St { int n = 0; double mx = 0, sum = 0, xmin = 1e9, xmax = -1e9; }; std::map<int, St> byG;
      for (size_t v = 0; v < pv.size(); v++) { if (old[v] >= 0 || pv[v] < 0) continue; const Vec3 mpl(-sp->pos[size_t(pv[v])].x, sp->pos[size_t(pv[v])].y, sp->pos[size_t(pv[v])].z); const double d = sp->pos[v].distanceTo(mpl); St& s = byG[vg[v]]; s.n++; s.mx = std::max(s.mx, d); s.sum += d; s.xmin = std::min(s.xmin, double(sp->pos[v].x)); s.xmax = std::max(s.xmax, double(sp->pos[v].x)); }
      std::printf("%s: no pair by place %d; by the connections +%d; still none %d\n", qPrintable(it.name), before, add, after);
      for (auto& [g, s] : byG) std::printf("  group %d (%s): %d paired anew, off by %.3f on average, %.3f at most, x %.1f..%.1f\n", g, g >= 0 && size_t(g) < sp->groupNames.size() ? qPrintable(sp->groupNames[size_t(g)]) : "-", s.n, s.sum / std::max(1, s.n), s.mx, s.xmin, s.xmax);
      std::map<int, int> leftG; std::map<int, std::array<double, 2>> leftX; for (size_t v = 0; v < pv.size(); v++) if (pv[v] < 0) { leftG[vg[v]]++; auto& lx = leftX.emplace(vg[v], std::array<double, 2>{1e9, -1e9}).first->second; lx[0] = std::min(lx[0], double(sp->pos[v].x)); lx[1] = std::max(lx[1], double(sp->pos[v].x)); }
      for (auto& [g, c] : leftG) std::printf("  still no pair, group %d (%s): %d, x %.1f..%.1f\n", g, g >= 0 && size_t(g) < sp->groupNames.size() ? qPrintable(sp->groupNames[size_t(g)]) : "-", c, leftX[g][0], leftX[g][1]);
      return 0; }
    return 1; }
  if (argc > 4 && QString(argv[3]) == "openseams") {            // (the mesh open where it looks closed: border edges whose ends lie on another border edge's ends — two sheets touching, not welded). args: name
    for (auto& [id, it] : sc.items) { if (!it.poly || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp);
      std::map<std::tuple<long, long, long>, std::vector<int>> at; auto key = [&](const Vec3& p) { return std::make_tuple(std::lround(p.x * 1000), std::lround(p.y * 1000), std::lround(p.z * 1000)); };
      for (int v = 0; v < int(sp->pos.size()); v++) at[key(sp->pos[size_t(v)])].push_back(v);
      int dupV = 0; for (auto& [k, vs] : at) if (vs.size() > 1) dupV++;
      int n = 0; std::map<int, int> byGroup; std::vector<Vec3> mids;
      for (int e = 0; e < T->ne; e++) { if (T->ef1[size_t(e)] >= 0) continue; const int a = T->ea[size_t(e)], b = T->eb[size_t(e)]; if (at[key(sp->pos[size_t(a)])].size() < 2 || at[key(sp->pos[size_t(b)])].size() < 2) continue;
        n++; const int f = T->ef0[size_t(e)]; byGroup[size_t(f) < sp->faceGroup.size() ? sp->faceGroup[size_t(f)] : -1]++; if (mids.size() < 6) mids.push_back((sp->pos[size_t(a)] + sp->pos[size_t(b)]) * 0.5); }
      std::printf("%s: %d vertices sharing a place with another, %d border edges with both ends doubled\n", qPrintable(it.name), dupV, n);
      if (uv::has(*sp)) { const uv::Points P = uv::points(*sp, *T); const auto fl = uv::seamFlags(*sp, *T); int ghost = 0; std::map<int, int> perLive;   // (the UV broken across an edge that is no seam: a hidden cut)
        for (int e = 0; e < T->ne; e++) { const int a = T->ef0[size_t(e)], b = T->ef1[size_t(e)]; if (b < 0 || fl[size_t(e)] || uv::continuous(*sp, *T, P, e)) continue;
          const int la = size_t(a) < sp->faceLive.size() ? sp->faceLive[size_t(a)] : -9, lb = size_t(b) < sp->faceLive.size() ? sp->faceLive[size_t(b)] : -9; if (la != lb) continue;
          ghost++; perLive[la]++; { auto gn = [&](int f) { const int g = size_t(f) < sp->faceGroup.size() ? sp->faceGroup[size_t(f)] : -1; return g >= 0 && size_t(g) < sp->groupNames.size() ? sp->groupNames[size_t(g)] + QString("#%1").arg(g) : QString("-"); }; static std::map<QString, int> pairs; pairs[gn(a) + " | " + gn(b)]++; static int shown2 = 0; if (std::getenv("CUTGROUPS") && ghost == 1) std::atexit([] { for (auto& [k, c] : pairs) std::printf("  cut between %s: %d\n", qPrintable(k), c); }); (void)shown2; }
          if (ghost <= 4) { const Vec3 m2 = (sp->pos[size_t(T->ea[size_t(e)])] + sp->pos[size_t(T->eb[size_t(e)])]) * 0.5; std::printf("  hidden cut: edge %d (faces %d / %d, set %d) at %.2f %.2f %.2f\n", e, a, b, la, m2.x, m2.y, m2.z); } }
        std::printf("UV broken across %d edges that are no seams, both sides in one set:", ghost); for (auto& [l, c] : perLive) std::printf(" set %d: %d", l, c); std::printf("\n");
        const auto mc = uv::mirrorCorners(*sp, *T); int lone = 0, noMir = 0;   // (seams with no seam on their mirror edge — cut on one side only)
        for (int f = 0; f < T->nf; f++) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int e = T->ce[size_t(k)]; if (!fl[size_t(e)] || T->ef0[size_t(e)] != f) continue; const int mk = mc[size_t(k)]; if (mk < 0) { noMir++; continue; }
          const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const int mnk = mc[size_t(nk)]; if (mnk < 0) { noMir++; continue; } const int me = T->edgeOf(T->fv[size_t(mk)], T->fv[size_t(mnk)]); if (me < 0) { noMir++; continue; }
          if (!fl[size_t(me)]) { lone++; if (lone <= 4) { const Vec3 m2 = (sp->pos[size_t(T->ea[size_t(e)])] + sp->pos[size_t(T->eb[size_t(e)])]) * 0.5; std::printf("  seam on one side only: edge %d at %.2f %.2f %.2f (mirror edge %d no seam)\n", e, m2.x, m2.y, m2.z, me); } } }
        std::printf("seams with no seam on the mirror edge: %d (no mirror found: %d)\n", lone, noMir);
        { const auto pv = topo::symPairs(*sp, sp->pos); int none = 0; std::map<int, int> byG; std::vector<int> vg(sp->pos.size(), -1); for (int f = 0; f < T->nf; f++) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vg[size_t(T->fv[size_t(k)])] = size_t(f) < sp->faceGroup.size() ? sp->faceGroup[size_t(f)] : -1;
          int shown = 0; for (size_t v = 0; v < sp->pos.size(); v++) { if (v < pv.size() && pv[v] >= 0) continue; none++; byG[vg[v]]++;
            if (shown < 5) { const Vec3 q(-sp->pos[v].x, sp->pos[v].y, sp->pos[v].z); double bd = 1e300; int bw = -1; for (size_t w = 0; w < sp->pos.size(); w++) { const double d = (sp->pos[w] - q).length(); if (d < bd) { bd = d; bw = int(w); } }
              std::printf("  vertex %zu at %.3f %.3f %.3f has no mirror pair — the nearest to its mirror place: vertex %d, %.4f away\n", v, sp->pos[v].x, sp->pos[v].y, sp->pos[v].z, bw, bd); shown++; } }
          std::printf("vertices with no mirror pair: %d of %zu;", none, sp->pos.size()); for (auto& [g, c] : byG) std::printf(" group %d (%s): %d", g, g >= 0 && size_t(g) < sp->groupNames.size() ? qPrintable(sp->groupNames[size_t(g)]) : "-", c); std::printf("\n"); } }
      for (auto& [g, c] : byGroup) std::printf("  group %d (%s): %d\n", g, g >= 0 && size_t(g) < sp->groupNames.size() ? qPrintable(sp->groupNames[size_t(g)]) : "-", c);
      for (auto& p : mids) std::printf("  e.g. at %.2f %.2f %.2f\n", p.x, p.y, p.z);
      return 0; }
    return 1; }
  if (argc > 4 && QString(argv[3]) == "bigfaces") {            // (the polygons of a mesh stretched across the UV: their box over 0.2 — which, their shell, their corners, the sets they are in). args: name
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp);
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P); int n = 0; std::map<int, int> perShell;
      for (int f = 0; f < T->nf; f++) { double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { x0 = std::min(x0, double(sp->cornerUV[size_t(k)][0])); x1 = std::max(x1, double(sp->cornerUV[size_t(k)][0])); y0 = std::min(y0, double(sp->cornerUV[size_t(k)][1])); y1 = std::max(y1, double(sp->cornerUV[size_t(k)][1])); }
        if (std::max(x1 - x0, y1 - y0) < 0.2) continue; perShell[S.of[size_t(f)]]++;
        if (std::getenv("BIGLIST")) std::printf("%d,", f);
        if (n++ < 12 && !std::getenv("BIGLIST")) { QString c; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) c += QString(" (%1 %2)").arg(sp->cornerUV[size_t(k)][0], 0, 'f', 3).arg(sp->cornerUV[size_t(k)][1], 0, 'f', 3);
          const uint32_t sets = size_t(f) < sp->faceSets.size() ? sp->faceSets[size_t(f)] : 0; const int live = size_t(f) < sp->faceLive.size() ? sp->faceLive[size_t(f)] : -9;
          std::printf("face %d shell %d sets %u live %d:%s\n", f, S.of[size_t(f)], sets, live, qPrintable(c)); } }
      std::printf("%d stretched polygons;", n); for (auto& [s, c] : perShell) std::printf(" shell %d: %d", s, c);
      if (!perShell.empty()) { std::vector<int> fs; for (int f = 0; f < T->nf; f++) { if (S.of[size_t(f)] != perShell.begin()->first) continue; double a0 = 1e9, a1 = -1e9, b0 = 1e9, b1 = -1e9; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { a0 = std::min(a0, double(sp->cornerUV[size_t(k)][0])); a1 = std::max(a1, double(sp->cornerUV[size_t(k)][0])); b0 = std::min(b0, double(sp->cornerUV[size_t(k)][1])); b1 = std::max(b1, double(sp->cornerUV[size_t(k)][1])); } if (std::max(a1 - a0, b1 - b0) >= 0.2) fs.push_back(f); }   // (the stretched ones of that shell unfolded again: what it comes to)
        int ne = 0, nb = 0, nseam = 0; const auto fl = uv::seamFlags(*sp, *T); std::set<int> fset(fs.begin(), fs.end()); std::set<int> vs; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { vs.insert(T->fv[size_t(k)]); const int e = T->ce[size_t(k)]; ne++; const int a = T->ef0[size_t(e)], b = T->ef1[size_t(e)]; if (b < 0) nb++; else if (fl[size_t(e)]) nseam++; else if (!fset.count(a) || !fset.count(b)) nb++; }
        std::printf("\nthe first such shell: %zu faces, %zu vertices, corners %d: mesh border %d, seams %d", fs.size(), vs.size(), ne, nb, nseam);
        PolyMesh c = *sp; const auto r = uv::unfold(c, *T, fs, true, true); double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9; for (int k : uv::faceCorners(*T, fs)) { x0 = std::min(x0, double(c.cornerUV[size_t(k)][0])); x1 = std::max(x1, double(c.cornerUV[size_t(k)][0])); y0 = std::min(y0, double(c.cornerUV[size_t(k)][1])); y1 = std::max(y1, double(c.cornerUV[size_t(k)][1])); }
        std::set<std::pair<float, float>> pts; for (int k : uv::faceCorners(*T, fs)) pts.insert({c.cornerUV[size_t(k)][0], c.cornerUV[size_t(k)][1]});
        std::printf("\nunfolded again: done %d closed %d holes %d — box %.3f..%.3f x %.3f..%.3f, %zu distinct UV points", r.done, r.closed, r.holes, x0, x1, y0, y1, pts.size()); }
      { std::vector<int> all2; for (int f = 0; f < T->nf; f++) { double a0 = 1e9, a1 = -1e9, b0 = 1e9, b1 = -1e9; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { a0 = std::min(a0, double(sp->cornerUV[size_t(k)][0])); a1 = std::max(a1, double(sp->cornerUV[size_t(k)][0])); b0 = std::min(b0, double(sp->cornerUV[size_t(k)][1])); b1 = std::max(b1, double(sp->cornerUV[size_t(k)][1])); } if (std::max(a1 - a0, b1 - b0) >= 0.2) all2.push_back(f); }
        if (!all2.empty()) { PolyMesh c2 = *sp; uv::SymUnfold su; su.mc = uv::mirrorCorners(c2, *T); su.pairs = true; su.axis = 0.5; uv::PackOpts po; const auto r2 = uv::unfold(c2, *T, all2, true, true, nullptr, &su, &po);
          std::set<std::pair<float, float>> p2; for (int k : uv::faceCorners(*T, all2)) p2.insert({c2.cornerUV[size_t(k)][0], c2.cornerUV[size_t(k)][1]});
          std::printf("\nall %zu stretched, symmetric as the app: done %d mirrored %d symmetric %d asym %d closed %d holes %d, %zu distinct UV points", all2.size(), r2.done, r2.mirrored, r2.symmetric, r2.asym, r2.closed, r2.holes, p2.size()); } }
      std::printf("\nsets:"); for (size_t s = 0; s < sp->uvSetNames.size(); s++) std::printf(" %zu=%s", s, qPrintable(sp->uvSetNames[s])); std::printf("\n");
      return 0; }
    return 1; }
  if (argc > 6 && QString(argv[3]) == "rectpng") {            // (Rectangle's two ways on the shell of a polygon: as it is, Grid, Relax — pictures and the stretch). args: name face out-prefix
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue;
      auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp); const int f0 = std::atoi(argv[5]); const QString out = QString::fromLocal8Bit(argv[6]);
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P);
      if (f0 < 0) { std::map<int, std::array<int, 3>> by; for (int f = 0; f < T->nf; f++) { auto& c = by[S.of[size_t(f)]]; if (!c[0]) c[2] = f; c[0]++; if (T->faceSize(f) == 4) c[1]++; }   // (-1: the shells, all quads or not, a polygon of each)
        for (auto& [s2, c] : by) if (c[0] >= 20) std::printf("shell %d: %d faces, %d quads, face %d\n", s2, c[0], c[1], c[2]); continue; }
      std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (S.of[size_t(f)] == S.of[size_t(f0)]) fs.push_back(f);
      auto draw = [&](const PolyMesh& m, const QString& file) { double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { x0 = std::min(x0, double(m.cornerUV[size_t(k)][0])); x1 = std::max(x1, double(m.cornerUV[size_t(k)][0])); y0 = std::min(y0, double(m.cornerUV[size_t(k)][1])); y1 = std::max(y1, double(m.cornerUV[size_t(k)][1])); }
        const int W = 900; const double s = (W - 40) / std::max(x1 - x0, y1 - y0); QImage im(W, W, QImage::Format_RGB32); im.fill(QColor(40, 40, 40)); QPainter pa(&im); pa.setRenderHint(QPainter::Antialiasing);
        for (int f : fs) { QPolygonF pg; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) pg << QPointF(20 + (m.cornerUV[size_t(k)][0] - x0) * s, W - 20 - (m.cornerUV[size_t(k)][1] - y0) * s); pa.setPen(QPen(QColor(200, 200, 220), 1)); pa.setBrush(QColor(150, 90, 150)); pa.drawPolygon(pg); }
        pa.end(); im.save(file); };
      auto sd = [&](const PolyMesh& m) { std::unordered_map<int, int> loc; std::vector<Vec3> X; std::vector<uv::solve::P2> U; std::vector<uv::solve::Tri> tris; const uv::Points PP = uv::points(m, *T);
        auto L = [&](int k) { auto r = loc.emplace(PP.of[size_t(k)], int(X.size())); if (r.second) { X.push_back(m.pos[size_t(T->fv[size_t(k)])]); U.push_back({m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]}); } return r.first->second; };
        for (int f : fs) { std::vector<Vec3> ring; std::vector<int> li; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { ring.push_back(m.pos[size_t(T->fv[size_t(k)])]); li.push_back(L(k)); } for (auto& t : uv::solve::triangulate(ring)) tris.push_back({li[size_t(t[0])], li[size_t(t[1])], li[size_t(t[2])]}); }
        return std::pair<double, int>{uv::solve::distortion(U, X, tris), uv::solve::flips(U, X, tris)}; };
      std::printf("shell of %d: %zu faces\n", f0, fs.size()); { auto r = sd(*sp); std::printf("  as it is  stretch %.4f flips %d\n", r.first, r.second); } draw(*sp, out + "_0.png");
      if (std::getenv("RECTFREE")) { uv::unfold(*sp, *T, fs, true, true); auto r = sd(*sp); std::printf("  unfolded free first: stretch %.4f flips %d\n", r.first, r.second); draw(*sp, out + "_free.png"); }   // (from a free unfold — a decent start, as a user's own layout)
      for (int mode : {uv::RectGrid, uv::RectRelax}) { PolyMesh c = *sp; const int n = uv::rectangle(c, *T, fs, mode); auto r = sd(c); std::printf("  %s  done %d stretch %.4f flips %d\n", mode == uv::RectGrid ? "grid " : "relax", n, r.first, r.second); draw(c, out + (mode == uv::RectGrid ? "_grid.png" : "_relax.png")); }
      return 0; }
    return 1; }
  if (argc > 3 && QString(argv[3]) == "timing") {              // (what the editor's steps cost on a big mesh: the topology, the points, the shells, Rectangle on one shell, the check)
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argc > 4 ? argv[4] : "Head")) continue; QElapsedTimer t;
      auto sp = std::make_shared<PolyMesh>(*it.poly); t.start(); auto T = topo::ttopo(sp); std::printf("ttopo %lld ms (%d faces)\n", t.elapsed(), T->nf);
      t.start(); const uv::Points P = uv::points(*sp, *T); std::printf("points %lld ms\n", t.elapsed()); t.start(); const uv::Shells S = uv::shells(*sp, *T, P); std::printf("shells %lld ms (%d)\n", t.elapsed(), S.count);
      t.start(); const auto fl = uv::flipped(*sp, *T, S); std::printf("flipped %lld ms (%zu)\n", t.elapsed(), fl.size());
      for (int s = 0; s < S.count; s++) { std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (S.of[size_t(f)] == s) fs.push_back(f); if (fs.size() < 3) continue; PolyMesh m = *sp; t.start(); const int n = uv::rectangle(m, *T, fs); std::printf("rectangle shell %d (%zu faces): %lld ms -> %d\n", s, fs.size(), t.elapsed(), n); } }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "raw") {                 // (a mesh's stored UV pins / features: how many, of which kind)
    for (auto& [id, it] : sc.items) { if (it.name != QString(argc > 4 ? argv[4] : "Head")) continue; const QJsonArray pins = it.raw.value("uvPins").toArray(), feats = it.raw.value("uvFeatures").toArray();
      std::map<QString, int> kinds; for (const QJsonValue& x : pins) { const QJsonArray a = x.toArray(); kinds[a.size() >= 3 ? a[2].toString() : QString("?")]++; }
      std::printf("mesh %d '%s': %d pins, %d features\n", id, qPrintable(it.name), int(pins.size()), int(feats.size())); for (auto& [k, c] : kinds) std::printf("   pin kind '%s': %d\n", qPrintable(k), c);
      int n = 0; for (const QJsonValue& x : pins) { const QJsonArray a = x.toArray(); if (n++ < 6) std::printf("   e.g. %s\n", qPrintable(QString(QJsonDocument(a).toJson(QJsonDocument::Compact)))); } }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "partunfold") {          // (Unfold of a PART of a shell, the rest held: faces round a point of the shell — how far the rest moves, what the part looks like)
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argc > 4 ? argv[4] : "Head")) continue;
      auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp); { const uv::Points P1 = uv::points(*sp, *T); auto be = uv::borderEdges(*sp, *T, P1); std::vector<char> fl1(size_t(T->ne), 0); for (int e : be) fl1[size_t(e)] = 1; uv::setSeams(*sp, *T, fl1); } const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P); const int sh = argc > 5 ? std::atoi(argv[5]) : 0; const double rad = argc > 6 ? std::atof(argv[6]) : 0.2;
      std::vector<int> shellF; for (int f = 0; f < T->nf; f++) if (S.of[size_t(f)] == sh) shellF.push_back(f); if (shellF.empty()) continue;
      auto cen = [&](int f) { Vec3 c{0, 0, 0}; int n = 0; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const Vec3& p = sp->pos[size_t(T->fv[size_t(k)])]; c.x += p.x; c.y += p.y; c.z += p.z; n++; } return Vec3{c.x / n, c.y / n, c.z / n}; };
      const Vec3 c0 = cen(shellF[shellF.size() / 3]); std::vector<char> sel(size_t(T->nf), 0); int ns = 0; double bs = 0; for (int f : shellF) { const Vec3 c = cen(f); bs = std::max(bs, double(std::hypot(std::hypot(c.x - c0.x, c.y - c0.y), c.z - c0.z))); }
      for (int f : shellF) { const Vec3 c = cen(f); if (std::hypot(std::hypot(c.x - c0.x, c.y - c0.y), c.z - c0.z) < rad * bs) { sel[size_t(f)] = 1; ns++; } }
      std::printf("mesh '%s' shell %d: %zu faces, %d picked (radius %.2f of its size)\n", qPrintable(it.name), sh, shellF.size(), ns, rad);
      uv::Pins held; for (int f : shellF) if (!sel[size_t(f)]) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) held.insert({T->fv[size_t(k)], T->fv[size_t(k)]});
      if (std::getenv("SOFT")) {                                  // (the soft version: three rows of polygons round the zone take part, the change dying out)
        const int K = 3; const auto sf = uv::seamFlags(*sp, *T); std::vector<int> fd(size_t(T->nf), -1), q; for (int f = 0; f < T->nf; f++) if (sel[size_t(f)]) { fd[size_t(f)] = 0; q.push_back(f); }
        for (size_t h = 0; h < q.size(); h++) { const int f = q[h]; if (fd[size_t(f)] >= K) continue; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int e = T->ce[size_t(k)]; if (T->ef1[size_t(e)] < 0 || sf[size_t(e)]) continue; const int g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)]; if (g >= 0 && fd[size_t(g)] < 0) { fd[size_t(g)] = fd[size_t(f)] + 1; q.push_back(g); } } }
        uv::Pins h2; for (int f : shellF) if (fd[size_t(f)] < 0) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) h2.insert({T->fv[size_t(k)], T->fv[size_t(k)]});
        PolyMesh m2 = *sp; QElapsedTimer t2; t2.start(); const auto r2 = uv::unfold(m2, *T, shellF, true, true, &h2, nullptr, nullptr, nullptr);
        std::vector<int> vd(sp->pos.size(), 1 << 20); for (int f = 0; f < T->nf; f++) if (fd[size_t(f)] >= 0) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vd[size_t(T->fv[size_t(k)])] = std::min(vd[size_t(T->fv[size_t(k)])], fd[size_t(f)]);
        std::map<int, double> byRing; for (int f = 0; f < T->nf; f++) if (fd[size_t(f)] >= 0) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const double w = std::clamp((K - vd[size_t(T->fv[size_t(k)])] + 1.0) / (K + 1.0), 0.0, 1.0); auto& qq = m2.cornerUV[size_t(k)]; const auto& o = sp->cornerUV[size_t(k)]; qq = {float(o[0] + w * (qq[0] - o[0])), float(o[1] + w * (qq[1] - o[1]))}; const double d = std::hypot(double(qq[0]) - o[0], double(qq[1]) - o[1]); byRing[vd[size_t(T->fv[size_t(k)])]] = std::max(byRing[vd[size_t(T->fv[size_t(k)])]], d); }
        int fb2 = 0; { const uv::Points P2 = uv::points(m2, *T); const uv::Shells S2 = uv::shells(m2, *T, P2); for (int f : uv::flipped(m2, *T, S2)) if (S2.of[size_t(f)] == S2.of[size_t(shellF[0])]) fb2++; }
        std::printf("  SOFT: %d done, %lld ms, flipped %d; the largest move by distance (rows from the zone):", r2.done, t2.elapsed(), fb2); for (auto& [rg, d] : byRing) std::printf("  %d: %.4f", rg, d); std::printf("\n"); }
      PolyMesh m = *sp; QElapsedTimer t; t.start(); const auto r = uv::unfold(m, *T, shellF, true, true, &held, nullptr, nullptr, nullptr); const qint64 ms = t.elapsed();
      double mvOut = 0, mvIn = 0; for (int f : shellF) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const double d = std::hypot(double(m.cornerUV[size_t(k)][0]) - sp->cornerUV[size_t(k)][0], double(m.cornerUV[size_t(k)][1]) - sp->cornerUV[size_t(k)][1]); (sel[size_t(f)] ? mvIn : mvOut) = std::max(sel[size_t(f)] ? mvIn : mvOut, d); }
      auto box = [&](const PolyMesh& mm) { double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9; for (int f : shellF) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { x0 = std::min<double>(x0, mm.cornerUV[size_t(k)][0]); x1 = std::max<double>(x1, mm.cornerUV[size_t(k)][0]); y0 = std::min<double>(y0, mm.cornerUV[size_t(k)][1]); y1 = std::max<double>(y1, mm.cornerUV[size_t(k)][1]); } return std::array<double, 4>{x0, y0, x1, y1}; };
      const auto b0 = box(*sp), b1 = box(m); int fa = 0, fb = 0; for (int f : uv::flipped(*sp, *T, S)) if (S.of[size_t(f)] == sh) fa++; { const uv::Points P2 = uv::points(m, *T); const uv::Shells S2 = uv::shells(m, *T, P2); for (int f : uv::flipped(m, *T, S2)) if (S2.of[size_t(f)] == S2.of[size_t(shellF[0])]) fb++; }
      { int cnt2 = 0; for (int f : shellF) if (!sel[size_t(f)] && cnt2 < 400 && f % 37 == 0) { const int k = T->fo[size_t(f)]; std::printf("    face %d corner %d: d(%.4f, %.4f)  at %.4f,%.4f\n", f, k, double(m.cornerUV[size_t(k)][0]) - sp->cornerUV[size_t(k)][0], double(m.cornerUV[size_t(k)][1]) - sp->cornerUV[size_t(k)][1], sp->cornerUV[size_t(k)][0], sp->cornerUV[size_t(k)][1]); cnt2++; } }
      { double bd = 0; int bk = -1; for (int f : shellF) if (!sel[size_t(f)]) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const double d = std::hypot(double(m.cornerUV[size_t(k)][0]) - sp->cornerUV[size_t(k)][0], double(m.cornerUV[size_t(k)][1]) - sp->cornerUV[size_t(k)][1]); if (d > bd) { bd = d; bk = k; } } if (bk >= 0) std::printf("  worst corner %d (vertex %d): %.4f,%.4f -> %.4f,%.4f; held %d\n", bk, T->fv[size_t(bk)], sp->cornerUV[size_t(bk)][0], sp->cornerUV[size_t(bk)][1], m.cornerUV[size_t(bk)][0], m.cornerUV[size_t(bk)][1], int(held.count({T->fv[size_t(bk)], T->fv[size_t(bk)]}))); } std::printf("  pinned %d mended %d closed %d holes %d held %zu\n", r.pinned, r.mended, r.closed, r.holes, held.size()); std::printf("  unfold %d done, %lld ms; the rest moved at most %.5f, the picked part %.5f; shell box %.3f x %.3f -> %.3f x %.3f; flipped %d -> %d\n", r.done, ms, mvOut, mvIn, b0[2] - b0[0], b0[3] - b0[1], b1[2] - b1[0], b1[3] - b1[1], fa, fb); }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "feat") {                // (the Eye / lips feature from each border loop of a mesh, and from each loop of seams: does it take?)
    for (auto& [id, it] : sc.items) { if (!it.poly || it.name != QString(argc > 4 ? argv[4] : "Head")) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp);
      std::vector<int> border; for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] < 0) border.push_back(e);
      std::printf("mesh '%s': %d border edges\n", qPrintable(it.name), int(border.size()));
      // the loops of the border (connected components)
      std::vector<char> seen(size_t(T->ne), 0); int li = 0; for (int e0 : border) { if (seen[size_t(e0)]) continue; std::vector<int> comp, st{e0}; seen[size_t(e0)] = 1;
        while (!st.empty()) { const int e = st.back(); st.pop_back(); comp.push_back(e); for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) for (int i = T->voff[size_t(v)]; i < T->voff[size_t(v) + 1]; i++) { const int x = T->vE[size_t(i)]; if (T->ef1[size_t(x)] < 0 && !seen[size_t(x)]) { seen[size_t(x)] = 1; st.push_back(x); } } }
        const auto f = uv::featureFromEdges(*sp, *T, comp); std::printf("  border loop %d: %zu edges -> %s", li++, comp.size(), f ? "feature" : "NO feature"); if (f) std::printf(" (upper %zu pts, lower %zu pts)", f->a.size(), f->b.size()); std::printf("\n"); } }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "featunfold") {          // (each border loop of a mesh made an Eye / lips feature — open / shut / half shut — and its shell unfolded: the turned-over polygons)
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argc > 4 ? argv[4] : "Head")) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp);
      { const uv::Points P1 = uv::points(*sp, *T); auto be = uv::borderEdges(*sp, *T, P1); std::vector<char> fl1(size_t(T->ne), 0); for (int e : be) fl1[size_t(e)] = 1; uv::setSeams(*sp, *T, fl1); }
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P);
      std::vector<char> seen(size_t(T->ne), 0); int li = 0;
      for (int e0 = 0; e0 < T->ne; e0++) { if (T->ef1[size_t(e0)] >= 0 || seen[size_t(e0)]) continue; std::vector<int> comp, st{e0}; seen[size_t(e0)] = 1;
        while (!st.empty()) { const int e = st.back(); st.pop_back(); comp.push_back(e); for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) for (int i = T->voff[size_t(v)]; i < T->voff[size_t(v) + 1]; i++) { const int x = T->vE[size_t(i)]; if (T->ef1[size_t(x)] < 0 && !seen[size_t(x)]) { seen[size_t(x)] = 1; st.push_back(x); } } }
        auto f0 = uv::featureFromEdges(*sp, *T, comp); if (!f0) continue; const int v0 = T->ea[size_t(comp[0])]; const int fc = T->vF[size_t(T->vfo[size_t(v0)])]; const int sh = S.of[size_t(fc)];
        std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (S.of[size_t(f)] == sh) fs.push_back(f);
        std::printf("loop %d (%zu edges, shell of %zu faces):", li++, comp.size(), fs.size());
        for (int mode = 0; mode < 4; mode++) { PolyMesh m = *sp; uv::Feature f = *f0; f.closed = mode >= 2; f.gap = mode == 3 ? 0.03 : 0; std::vector<uv::Feature> feats; if (mode > 0) feats.push_back(f);
          uv::PackOpts po; po.w = po.h = 4096; const auto r = uv::unfold(m, *T, fs, true, true, nullptr, nullptr, &po, &feats);
          int fb = 0; { const uv::Points P2 = uv::points(m, *T); const uv::Shells S2 = uv::shells(m, *T, P2); for (int ff : uv::flipped(m, *T, S2)) if (S2.of[size_t(ff)] == S2.of[size_t(fs[0])]) fb++; }
          double vlo = 1e9, vhi = -1e9; for (int v : f.a) { for (int i = T->vfo[size_t(v)]; i < T->vfo[size_t(v) + 1]; i++) { const int ff = T->vF[size_t(i)]; if (S.of[size_t(ff)] != sh) continue; for (int k = T->fo[size_t(ff)]; k < T->fo[size_t(ff) + 1]; k++) if (T->fv[size_t(k)] == v) { vlo = std::min<double>(vlo, m.cornerUV[size_t(k)][1]); vhi = std::max<double>(vhi, m.cornerUV[size_t(k)][1]); } } }
          std::printf("  [%s: done %d flipped %d, the upper rim V spread %.5f]", mode == 0 ? "no feature" : mode == 1 ? "open" : mode == 2 ? "shut" : "half shut", r.done, fb, vhi - vlo); }
        std::printf("\n"); } }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "pinshell") {            // (each stored pin edge: the shells on its two sides and their sizes — which shell it can hold)
    for (auto& [id, it] : sc.items) { if (!it.poly || it.name != QString(argc > 4 ? argv[4] : "Head")) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp); const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P);
      std::vector<int> cnt(size_t(S.count) + 1, 0); for (int s : S.of) cnt[size_t(s)]++; int n = 0;
      for (const QJsonValue& x : it.raw.value("uvPins").toArray()) { const QJsonArray a = x.toArray(); if (a.size() < 2) continue; const int e = T->edgeOf(a[0].toInt(), a[1].toInt()); if (e < 0) continue; const int f0 = T->ef0[size_t(e)], f1 = T->ef1[size_t(e)];
        std::printf("pin %d,%d kind %s side %d: shells %d (%d faces) | %d (%d faces)\n", a[0].toInt(), a[1].toInt(), qPrintable(a.size() > 2 ? a[2].toString() : QString("?")), a.size() > 3 ? a[3].toInt() : -1, f0 >= 0 ? S.of[size_t(f0)] : -1, f0 >= 0 ? cnt[size_t(S.of[size_t(f0)])] : 0, f1 >= 0 ? S.of[size_t(f1)] : -1, f1 >= 0 ? cnt[size_t(S.of[size_t(f1)])] : 0); if (++n >= 12) break; } }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "stack") {              // (every shell of a mesh brought onto its last one: how far each point lies from the nearest point of it, texels of 4096). args: name
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp); const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P);
      std::vector<std::vector<int>> F(static_cast<size_t>(S.count)); for (int f = 0; f < T->nf; f++) F[size_t(S.of[size_t(f)])].push_back(f); int ref = 0; for (int s = 0; s < S.count; s++) if (F[size_t(s)].size() >= F[size_t(ref)].size()) ref = s; std::vector<int> all(size_t(T->nf)); std::iota(all.begin(), all.end(), 0);
      uv::PackOpts po; po.w = po.h = 4096; PolyMesh m = *sp; const int n = uv::stackOnto(m, *T, all, F[size_t(ref)][0], &po); std::printf("%s: %d shells, ref %d, moved %d\n", qPrintable(it.name), S.count, ref, n);
      for (int s = 0; s < S.count; s++) { double worst = 0, wb = 0; int nb = 0; { std::set<int> bp; for (int f : F[size_t(s)]) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) if (!uv::continuous(*sp, *T, P, T->ce[size_t(k)])) bp.insert(P.of[size_t(k)]); nb = int(bp.size()); } for (int f : F[size_t(s)]) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { double b = 1e300; for (int fr : F[size_t(ref)]) for (int j = T->fo[size_t(fr)]; j < T->fo[size_t(fr) + 1]; j++) b = std::min<double>(b, std::hypot(m.cornerUV[size_t(k)][0] - m.cornerUV[size_t(j)][0], m.cornerUV[size_t(k)][1] - m.cornerUV[size_t(j)][1])); worst = std::max(worst, b); if (!uv::continuous(*sp, *T, P, T->ce[size_t(k)])) wb = std::max(wb, b); } std::printf("  shell %d: %zu faces, %d border points, worst off the reference's points: any %.3f, border %.3f texels\n", s, F[size_t(s)].size(), nb, worst * 4096, wb * 4096); } }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "facepng") {            // (a face shell: its eyes / mouth features unfolded open, shut, half shut, and a local Unfold round the mouth — pictures and numbers). args: name shell outdir
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp);
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P); const int sh = std::atoi(argv[5]); const QString out = QString::fromLocal8Bit(argv[6]);
      std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (S.of[size_t(f)] == sh) fs.push_back(f);
      auto feats = uv::detectFeatures(*sp, *T, fs); std::printf("shell %d: %zu faces, %zu features\n", sh, fs.size(), feats.size()); for (auto& f : feats) std::printf("  feature: %zu / %zu pts\n", f.a.size(), f.b.size());
      uv::PackOpts po; po.w = po.h = 4096;
      auto dump = [&](const PolyMesh& m, const QString& name, std::array<double, 4> crop = {0, 0, 0, 0}) {
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { x0 = std::min<double>(x0, m.cornerUV[size_t(k)][0]); x1 = std::max<double>(x1, m.cornerUV[size_t(k)][0]); y0 = std::min<double>(y0, m.cornerUV[size_t(k)][1]); y1 = std::max<double>(y1, m.cornerUV[size_t(k)][1]); }
        if (crop[2] > crop[0]) { x0 = crop[0]; y0 = crop[1]; x1 = crop[2]; y1 = crop[3]; }
        const int W = 1100; const double s = (W - 20) / std::max(x1 - x0, y1 - y0); QImage im(W, W, QImage::Format_RGB32); im.fill(QColor(0x22, 0x22, 0x26)); QPainter p(&im); p.setRenderHint(QPainter::Antialiasing);
        auto pt = [&](int k) { return QPointF(10 + (m.cornerUV[size_t(k)][0] - x0) * s, W - 10 - (m.cornerUV[size_t(k)][1] - y0) * s); };
        const uv::Points Pm = uv::points(m, *T); const uv::Shells Sm = uv::shells(m, *T, Pm); std::set<int> fl; for (int f : uv::flipped(m, *T, Sm)) fl.insert(f); uv::CheckOpts co0; co0.w = co0.h = 4096; co0.pad = 0; std::set<int> ovr; for (int f : uv::check(m, *T, fs, co0).overlap) ovr.insert(f);
        for (int f : fs) { QPolygonF q; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) q << pt(k); p.setPen(QPen(QColor(0xb8, 0xb8, 0xc4), 1)); p.setBrush(fl.count(f) ? QColor(0xff, 0x30, 0x60) : ovr.count(f) ? QColor(0xf0, 0x20, 0xe0) : QColor(0x30, 0x48, 0xb0)); p.drawPolygon(q); }
        p.end(); im.save(out + "/" + name + ".png");
        uv::CheckOpts co; co.w = co.h = 4096; co.pad = 0; const auto rep = uv::check(m, *T, fs, co); std::printf("  %-14s box %.3f x %.3f  over another %zu, turned over %zu\n", qPrintable(name), x1 - x0, y1 - y0, rep.overlap.size(), rep.flipped.size());
        for (auto& ft : feats) { double lo = 1e9, hi = -1e9, ul = 1e9, uh = -1e9; for (int v : ft.a) for (int i = T->vfo[size_t(v)]; i < T->vfo[size_t(v) + 1]; i++) { const int f = T->vF[size_t(i)]; if (S.of[size_t(f)] != sh) continue; const int k = uv::cornerAt(*T, f, v); if (k < 0) continue; lo = std::min<double>(lo, m.cornerUV[size_t(k)][1]); hi = std::max<double>(hi, m.cornerUV[size_t(k)][1]); ul = std::min<double>(ul, m.cornerUV[size_t(k)][0]); uh = std::max<double>(uh, m.cornerUV[size_t(k)][0]); }
          std::printf("     rim: width %.4f, height of its upper chain %.5f\n", uh - ul, hi - lo); } };
      dump(*sp, "0_asis");
      for (int mode = 0; mode < 4; mode++) { PolyMesh m = *sp; auto fe = feats; for (auto& f : fe) { f.closed = mode >= 2; f.gap = mode == 3 ? 0.03 : 0; } if (mode == 0) fe.clear(); uv::SymUnfold su; su.mc = uv::mirrorCorners(*sp, *T); su.pairs = true; su.axis = 0.5; uv::unfold(m, *T, fs, true, true, nullptr, std::getenv("NOSYM") ? nullptr : &su, &po, &fe); dump(m, QString("%1_%2").arg(mode + 1).arg(mode == 0 ? "nofeat" : mode == 1 ? "open" : mode == 2 ? "shut" : "half"));
        if (mode >= 2) { double lo = 1e9, hi = -1e9, ul = 1e9, uh = -1e9; const auto& ft = feats[0]; for (int v : ft.a) for (int i = T->vfo[size_t(v)]; i < T->vfo[size_t(v) + 1]; i++) { const int f = T->vF[size_t(i)]; if (S.of[size_t(f)] != sh) continue; const int k = uv::cornerAt(*T, f, v); if (k < 0) continue; lo = std::min<double>(lo, m.cornerUV[size_t(k)][1]); hi = std::max<double>(hi, m.cornerUV[size_t(k)][1]); ul = std::min<double>(ul, m.cornerUV[size_t(k)][0]); uh = std::max<double>(uh, m.cornerUV[size_t(k)][0]); } const double w = uh - ul; dump(m, QString("%1_eye").arg(mode + 1), {ul - w * 0.3, lo - w * 0.65, uh + w * 0.3, hi + w * 0.65}); } }
    }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "seammap") {            // (a mesh's pieces along its seams, and how many seam edges lie between each two)
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name != QString(argv[4])) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp); const auto fl = uv::seamFlags(*sp, *T);
      std::vector<int> c(size_t(T->nf), -1); int n = 0; for (int f0 = 0; f0 < T->nf; f0++) { if (c[size_t(f0)] >= 0) continue; std::vector<int> st{f0}; c[size_t(f0)] = n; while (!st.empty()) { const int f = st.back(); st.pop_back(); for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int e = T->ce[size_t(k)]; if (T->ef1[size_t(e)] < 0 || fl[size_t(e)]) continue; const int g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)]; if (c[size_t(g)] < 0) { c[size_t(g)] = n; st.push_back(g); } } } n++; }
      std::vector<int> sz(size_t(n), 0); for (int x : c) sz[size_t(x)]++; std::map<std::pair<int, int>, int> between; int inner = 0; for (int e = 0; e < T->ne; e++) if (fl[size_t(e)] && T->ef1[size_t(e)] >= 0) { int a = c[size_t(T->ef0[size_t(e)])], b = c[size_t(T->ef1[size_t(e)])]; if (a == b) inner++; else between[{std::min(a, b), std::max(a, b)}]++; }
      const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P);
      std::printf("mesh '%s': %d faces, %zu seams stored, %d pieces, %d UV shells, %d seam edges inside one piece\n", qPrintable(it.name), T->nf, sp->seams.size(), n, S.count, inner); for (int i = 0; i < n; i++) std::printf("  piece %d: %d faces\n", i, sz[size_t(i)]); for (auto& [k, v] : between) std::printf("  between %d (%d f) and %d (%d f): %d seam edges\n", k.first, sz[size_t(k.first)], k.second, sz[size_t(k.second)], v); }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "items") {              // (what a project holds: the meshes, their polygons)
    long total = 0; int n = 0; for (auto& [id, it] : sc.items) if (it.poly) { total += it.poly->faceCount(); n++; std::printf("  %d '%s' faces %d base %d uv %d\n", id, qPrintable(it.name), it.poly->faceCount(), it.base ? it.base->faceCount() : -1, int(uv::has(*it.poly))); } std::printf("meshes %d, faces %ld, items %zu\n", n, total, sc.items.size()); return 0; }
  if (argc > 3 && QString(argv[3]) == "rect") {               // (Rectangle on each shell of the meshes named in argv[4]: how rectangular it was / is — its area to its box)
    const QString nm = argc > 4 ? QString(argv[4]) : QString("Head");
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name.indexOf(nm) < 0 || it.poly->faceCount() > 20000) continue;
      auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp); const uv::Points P = uv::points(*sp, *T); const uv::Shells S = uv::shells(*sp, *T, P);
      std::printf("mesh %d '%s': %d shells\n", id, qPrintable(it.name), S.count);
      for (int s = 0; s < S.count; s++) { std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (S.of[size_t(f)] == s) fs.push_back(f); if (fs.size() < 3) continue;
        auto ratio = [&](const PolyMesh& m, double& w, double& h) { double a = 0, x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9; for (int f : fs) { a += std::abs(uv::faceArea(m, f)); for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { x0 = std::min<double>(x0, m.cornerUV[size_t(k)][0]); x1 = std::max<double>(x1, m.cornerUV[size_t(k)][0]); y0 = std::min<double>(y0, m.cornerUV[size_t(k)][1]); y1 = std::max<double>(y1, m.cornerUV[size_t(k)][1]); } } w = x1 - x0; h = y1 - y0; return a / std::max(1e-12, w * h); };
        double w0, h0, w1, h1; const double r0 = ratio(*sp, w0, h0); PolyMesh m = *sp; const int n = uv::rectangle(m, *T, fs); const double r1 = ratio(m, w1, h1);
        if (std::getenv("RECTDBG")) { for (int pass = 0; pass < 2; pass++) { const PolyMesh& mm = pass ? m : *sp; double y0 = 1e9, y1 = -1e9; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { y0 = std::min<double>(y0, mm.cornerUV[size_t(k)][1]); y1 = std::max<double>(y1, mm.cornerUV[size_t(k)][1]); }
            // the shell's own axis-aligned slices: for the rectangle the slices go along its long side
            double xs0 = 1e9, xs1 = -1e9; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { xs0 = std::min<double>(xs0, mm.cornerUV[size_t(k)][0]); xs1 = std::max<double>(xs1, mm.cornerUV[size_t(k)][0]); }
            const bool tall = (y1 - y0) >= (xs1 - xs0); std::printf("    %s slices (%s):", pass ? "after " : "before", tall ? "along V" : "along U");
            for (int i = 0; i < 10; i++) { double lo = 1e9, hi = -1e9; const double a0 = tall ? y0 : xs0, a1 = tall ? y1 : xs1; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const double a = tall ? mm.cornerUV[size_t(k)][1] : mm.cornerUV[size_t(k)][0], b = tall ? mm.cornerUV[size_t(k)][0] : mm.cornerUV[size_t(k)][1];
                  if (a >= a0 + (a1 - a0) * i / 10.0 && a <= a0 + (a1 - a0) * (i + 1) / 10.0) { lo = std::min(lo, b); hi = std::max(hi, b); } } std::printf(" %.4f", hi > lo ? hi - lo : 0.0); } std::printf("\n"); } }
        { double sk = 0; int c = 0; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const double dx = m.cornerUV[size_t(nk)][0] - m.cornerUV[size_t(k)][0], dy = m.cornerUV[size_t(nk)][1] - m.cornerUV[size_t(k)][1];
            if (std::abs(dx) > 2 * std::abs(dy) && std::abs(dx) > w1 * 0.4) { sk += std::abs(dy) / std::abs(dx); c++; } } if (c) std::printf("    rows: %d cross edges, mean slope %.4f\n", c, sk / c); }
        std::printf("  shell %3d: %4zu faces  fill %.2f (box %.3f x %.3f) -> rectangle() %d: fill %.2f (box %.3f x %.3f)\n", s, fs.size(), r0, w0, h0, n, r1, w1, h1); } }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "packgroup") {          // (the eyelid mesh: all its shells one group, Symmetry on — packed; the box the shells take in the tile)
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.name.indexOf("Eyelid") < 0) continue;
      auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp);
      { const uv::Points P = uv::points(*sp, *T); auto be = uv::borderEdges(*sp, *T, P); std::vector<char> fl(size_t(T->ne), 0); for (int e : be) fl[size_t(e)] = 1; uv::setSeams(*sp, *T, fl); }
      const auto mc = uv::mirrorCorners(*sp, *T); std::vector<int> allF(size_t(T->nf)); for (int f = 0; f < T->nf; f++) allF[size_t(f)] = f;
      for (int g = 0; g < 2; g++) for (int sy = 0; sy < 2; sy++) { PolyMesh m = *sp; std::vector<uv::ShellAttr> at(size_t(T->nf)); for (auto& a : at) { a.group = g ? 1 : 0; a.sym = sy; }
        uv::PackOpts o; o.w = o.h = 2048; o.pad = 8; o.rotate = false; o.equalize = true; o.attr = &at; std::vector<int> mcc = mc; if (sy) { o.mc = &mcc; o.axis = 0.5; }
        const auto r = uv::packShapes(m, *T, allF, o); std::vector<int> ks(m.cornerUV.size()); for (size_t k = 0; k < ks.size(); k++) ks[k] = int(k); const auto b = uv::box(m, ks);
        std::printf("  '%s' group %d sym %d: ok %d shells %d fill %.2f%% box %.3f..%.3f x %.3f..%.3f (w %.3f h %.3f)\n", qPrintable(it.name), g, sy, r.ok, r.shells, r.fill * 100, b[0], b[2], b[1], b[3], b[2] - b[0], b[3] - b[1]); } }
    return 0; }
  if (argc > 3 && QString(argv[3]) == "shelldump") {          // (the shells of every mesh with UV as text — by UV Mat, and all the hair meshes as one set: for a packing bench outside the program). args: file
    FILE* out = std::fopen(argv[4], "w"); if (!out) return 1; int nsets = 0;
    auto dump = [&](const std::string& nm, const PolyMesh& mm, const topo::TTopo& TT, const uv::Shells& Sh, const std::vector<int>& fs) {
      std::map<int, std::vector<int>> by; for (int f : fs) by[Sh.of[size_t(f)]].push_back(f); std::fprintf(out, "set %s %zu\n", nm.c_str(), by.size()); nsets++;
      for (auto& [s, l] : by) { std::vector<std::array<float, 2>> p; std::vector<std::array<int, 3>> t; double a3 = 0; for (int f : l) { a3 += uv::faceArea3(mm, f); const int a = TT.fo[size_t(f)], e = TT.fo[size_t(f) + 1], o = int(p.size()); for (int k = a; k < e; k++) p.push_back(mm.cornerUV[size_t(k)]); for (int k = 1; k + 1 < e - a; k++) t.push_back({o, o + k, o + k + 1}); }
        std::fprintf(out, "shell %zu %zu %g\n", p.size(), t.size(), a3); for (auto& q : p) std::fprintf(out, "%.6f %.6f ", q[0], q[1]); std::fprintf(out, "\n"); for (auto& q : t) std::fprintf(out, "%d %d %d ", q[0], q[1], q[2]); std::fprintf(out, "\n"); } };
    PolyMesh J; bool jf = false;
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly)) continue; auto sp = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(sp); const uv::Points P = uv::points(*sp, *T); const uv::Shells Sh = uv::shells(*sp, *T, P);
      std::map<int, std::vector<int>> mats; for (int f = 0; f < T->nf; f++) mats[sp->faceMat.empty() ? -1 : sp->faceMat[size_t(f)]].push_back(f); const std::string base = QString(it.name).replace(' ', '_').toStdString();
      for (auto& [m, fs] : mats) dump(base + "#" + std::to_string(m), *sp, *T, Sh, fs);
      if (it.poly->faceCount() < 20000 && it.name.indexOf("Hair") >= 0) { const PolyMesh& B = *it.poly; const int v0 = int(J.pos.size()), c0 = int(J.faceVerts.size()); J.pos.insert(J.pos.end(), B.pos.begin(), B.pos.end()); if (J.faceStart.empty()) J.faceStart.push_back(0); for (int f = 0; f < B.faceCount(); f++) J.faceStart.push_back(c0 + B.faceStart[size_t(f) + 1]); for (int v : B.faceVerts) J.faceVerts.push_back(v0 + v); J.cornerUV.insert(J.cornerUV.end(), B.cornerUV.begin(), B.cornerUV.end()); jf = true; } }
    if (jf) { auto sp = std::make_shared<PolyMesh>(J); auto T = topo::ttopo(sp); const uv::Points P = uv::points(*sp, *T); const uv::Shells Sh = uv::shells(*sp, *T, P); std::vector<int> all(size_t(T->nf)); for (int f = 0; f < T->nf; f++) all[size_t(f)] = f; dump("ALLHAIR", *sp, *T, Sh, all); }
    std::fclose(out); std::printf("%d sets\n", nsets); return 0; }
  if (argc > 3 && QString(argv[3]) == "packbench") {          // (every hair mesh with UV packed into one tile, as Pack all of several meshes: the fill by the options)
    PolyMesh J; std::vector<std::string> names; int nm = 0;
    for (auto& [id, it] : sc.items) { if (!it.poly || !uv::has(*it.poly) || it.poly->faceCount() >= 20000 || it.name.indexOf("Hair") < 0) continue; const PolyMesh& B = *it.poly; nm++;
      const int v0 = int(J.pos.size()), c0 = int(J.faceVerts.size()); J.pos.insert(J.pos.end(), B.pos.begin(), B.pos.end()); if (J.faceStart.empty()) J.faceStart.push_back(0);
      for (int f = 0; f < B.faceCount(); f++) J.faceStart.push_back(c0 + B.faceStart[size_t(f) + 1]);
      for (int v : B.faceVerts) J.faceVerts.push_back(v0 + v); J.cornerUV.insert(J.cornerUV.end(), B.cornerUV.begin(), B.cornerUV.end()); }
    auto sp = std::make_shared<PolyMesh>(J); auto T = topo::ttopo(sp);
    { const uv::Points P = uv::points(*sp, *T); auto be = uv::borderEdges(*sp, *T, P); std::vector<char> fl(size_t(T->ne), 0); for (int e : be) fl[size_t(e)] = 1; uv::setSeams(*sp, *T, fl); }
    const uv::Points P0 = uv::points(*sp, *T); const uv::Shells S0 = uv::shells(*sp, *T, P0);
    std::printf("packbench: %d hair meshes, %d faces, %d shells\n", nm, T->nf, S0.count);
    std::vector<int> allF(size_t(T->nf)); for (int f = 0; f < T->nf; f++) allF[size_t(f)] = f;
    const int passesList[] = {1, 3, 6, 12}; QElapsedTimer t;
    for (int eq = 0; eq < 2; eq++) for (int rot = 0; rot < 2; rot++) for (int ps : passesList) {
      PolyMesh m = *sp; uv::PackOpts o; o.w = o.h = 2048; o.pad = 8; o.rotate = rot; o.equalize = eq; o.passes = ps; t.start(); const auto r = uv::packShapes(m, *T, allF, o);
      std::printf("  equalize %d rotate %d passes %2d: ok %d fill %5.2f%% scale %.4f  %6lld ms\n", eq, rot, ps, r.ok, r.fill * 100, r.scale, t.elapsed()); std::fflush(stdout); }
    return 0; }
  for (auto& [id, it] : sc.items) {
    if (!it.poly || !uv::has(*it.poly) || it.poly->faceCount() < minFaces) continue;
    std::printf("mesh %d '%s': %zu verts, %d faces\n", id, qPrintable(it.name), it.poly->pos.size(), it.poly->faceCount());
    auto base = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(base);
    { const uv::Points P = uv::points(*base, *T); auto be = uv::borderEdges(*base, *T, P); std::vector<char> fl(size_t(T->ne), 0); for (int e : be) fl[size_t(e)] = 1; uv::setSeams(*base, *T, fl); }
    QElapsedTimer t;
    if (argc > 3 && QString(argv[3]) == "repack") {            // (the mesh's own UV packed again: symmetric, all one group, equal density — overlaps counted)
      PolyMesh m = *base; std::vector<int> fs; for (int f = 0; f < T->nf; f++) fs.push_back(f);
      const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P);
      { uv::CheckOpts co; co.pad = 0; const auto c = uv::check(m, *T, fs, co); { uv::CheckOpts c8; c8.pad = 8; const auto q = uv::check(m, *T, fs, c8); std::printf("  saved UV: padding violations (8px) %zu, overlap %zu\n", q.padding.size(), q.overlap.size()); } std::map<int, int> per; for (int f : c.overlap) per[S.of[size_t(f)]]++;
        std::printf("  saved UV: overlap %zu, flipped %zu; by shell:", c.overlap.size(), c.flipped.size()); for (auto& [s, n] : per) std::printf(" %d:%d", s, n); std::printf("\n");
        for (int s = 0; s < S.count; s++) { double x0 = 9, y0 = 9, x1 = -9, y1 = -9; int nf = 0; for (int f = 0; f < T->nf; f++) if (S.of[size_t(f)] == s) { nf++; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { x0 = std::min<double>(x0, m.cornerUV[size_t(k)][0]); x1 = std::max<double>(x1, m.cornerUV[size_t(k)][0]); y0 = std::min<double>(y0, m.cornerUV[size_t(k)][1]); y1 = std::max<double>(y1, m.cornerUV[size_t(k)][1]); } }
          std::printf("    shell %d: %d faces, box %.3f..%.3f x %.3f..%.3f\n", s, nf, x0, x1, y0, y1); } }
      { int nh = 0, nv = 0, bad = 0; double worst = 0; for (const QJsonValue& x : it.raw.value("uvPins").toArray()) { const QJsonArray q = x.toArray(); if (q.size() < 3 || q[0].toInt() == q[1].toInt()) continue;
          const QString kd = q[2].toString(); const int a = q[0].toInt(), b = q[1].toInt(), e = T->edgeOf(a, b); if (e < 0) continue;
          for (int f : {T->ef0[size_t(e)], T->ef1[size_t(e)]}) { if (f < 0) continue; if (q.size() >= 4 && q[3].toInt() != f) continue; const int k0 = uv::cornerAt(*T, f, a), k1 = uv::cornerAt(*T, f, b); if (k0 < 0 || k1 < 0) continue;
            const double du = m.cornerUV[size_t(k1)][0] - m.cornerUV[size_t(k0)][0], dv = m.cornerUV[size_t(k1)][1] - m.cornerUV[size_t(k0)][1]; const double off = kd == "h" ? std::abs(dv) : kd == "v" ? std::abs(du) : 0; if (kd == "h") nh++; else if (kd == "v") nv++; if (off > 1e-4) bad++; worst = std::max(worst, off); } }
        std::printf("  constraints: %d horizontal, %d vertical edge sides; off by more than 1e-4: %d (worst %.5f)\n", nh, nv, bad, worst); }
      for (int g = 0; g < 4; g++) {
        PolyMesh a = m; std::vector<uv::ShellAttr> at(size_t(T->nf)); if (g & 1) for (auto& x : at) x.group = 1;
        uv::PackOpts o; o.attr = &at; o.equalize = true; o.rotate = std::getenv("ROT") != nullptr; auto mc = uv::mirrorCorners(a, *T); if (g & 2) { o.mc = &mc; o.axis = 0.5; }
        auto r = uv::packShapes(a, *T, fs, o); uv::CheckOpts co; co.pad = 0; const auto c = uv::check(a, *T, fs, co);
        uv::CheckOpts c8; c8.pad = 8; const auto q = uv::check(a, *T, fs, c8);
        std::printf("  shells %d, group %d, sym %d: ok %d fill %.1f%% pairs %d split %d overlap %zu padding8 %zu\n", S.count, g & 1, (g >> 1) & 1, int(r.ok), r.fill * 100, r.pairs, r.split, c.overlap.size(), q.padding.size()); }
      continue; }
    if (argc > 3 && QString(argv[3]) == "attr") {              // (Pack with each shell's settings: stacks, lock, no turn, scale, group)
      auto matFaces = [&](int mat) { std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (base->faceMat.empty() || base->faceMat[size_t(f)] == mat) fs.push_back(f); return fs; };
      auto shellsOf = [&](const PolyMesh& m, const std::vector<int>& fs) { const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P); std::map<int, std::vector<int>> by; for (int f : fs) by[S.of[size_t(f)]].push_back(f); std::vector<std::vector<int>> o; for (auto& [s, v] : by) o.push_back(v); return o; };
      auto chk = [&](const PolyMesh& m, const std::vector<int>& fs) { uv::CheckOpts o; o.pad = 8; return uv::check(m, *T, fs, o); };
      auto tdOf = [&](const PolyMesh& m, const std::vector<int>& sfs) { double au = 0, a3 = 0; for (int f : sfs) { au += std::abs(uv::faceArea(m, f)); a3 += uv::faceArea3(m, f); } return std::sqrt(au / a3); };
      { const auto fs = matFaces(3); PolyMesh m = *base; const auto c0 = chk(m, fs); uv::PackOpts o; auto r = uv::packShapes(m, *T, fs, o); const auto c1 = chk(m, fs);
        std::printf("  eyes, own UV: stacks %d, overlap %zu → packed (stacks kept %d): stacks %d, overlap %zu, padding %zu, fill %.1f%%\n", c0.stacks, c0.overlap.size(), r.stacked, c1.stacks, c1.overlap.size(), c1.padding.size(), r.fill * 100); }
      const auto fs = matFaces(0); PolyMesh u = *base; { uv::SymUnfold su; su.mc = uv::mirrorCorners(*base, *T); su.pairs = true; uv::PackOpts po; uv::unfold(u, *T, fs, false, true, nullptr, &su, &po); }
      auto sh = shellsOf(u, fs); std::sort(sh.begin(), sh.end(), [](auto& a, auto& b) { return a.size() > b.size(); }); const auto& face = sh[0];
      { PolyMesh m = u; std::vector<uv::ShellAttr> at(size_t(T->nf)); for (int f : face) at[size_t(f)].lock = true;
        const auto before = uv::box(m, uv::faceCorners(*T, face)); uv::PackOpts o; o.attr = &at; auto r = uv::packShapes(m, *T, fs, o); const auto after = uv::box(m, uv::faceCorners(*T, face)); const auto c = chk(m, fs);
        std::printf("  head, the face locked: moved %.2e; packed %d (locked %d), overlap %zu, padding %zu, fill %.1f%%\n", std::abs(after[0] - before[0]) + std::abs(after[1] - before[1]), r.shells, r.locked, c.overlap.size(), c.padding.size(), r.fill * 100); }
      { PolyMesh m = u; std::vector<uv::ShellAttr> at(size_t(T->nf)); for (auto& a : at) a.noTurn = true; uv::PackOpts o; o.attr = &at; uv::packShapes(m, *T, fs, o);
        int turned = 0; for (auto& s : sh) { const int f = s[0], k0 = T->fo[size_t(f)], k1 = k0 + 1; const double a0 = std::atan2(u.cornerUV[size_t(k1)][1] - u.cornerUV[size_t(k0)][1], u.cornerUV[size_t(k1)][0] - u.cornerUV[size_t(k0)][0]), a1 = std::atan2(m.cornerUV[size_t(k1)][1] - m.cornerUV[size_t(k0)][1], m.cornerUV[size_t(k1)][0] - m.cornerUV[size_t(k0)][0]);
          if (std::abs(std::remainder(a1 - a0, 2 * M_PI)) > 1e-3) turned++; }
        std::printf("  head, no turn on all: %d of %zu shells turned\n", turned, sh.size()); }
      { PolyMesh m = u; std::vector<uv::ShellAttr> at(size_t(T->nf)); for (int f : face) at[size_t(f)].scale = 2; uv::PackOpts o; o.attr = &at; uv::packShapes(m, *T, fs, o);
        std::printf("  head, the face ×2: its texel density to the next shell's %.3f (2 wanted; before %.3f)\n", tdOf(m, face) / tdOf(m, sh[1]), tdOf(u, face) / tdOf(u, sh[1])); }
      { auto spread = [&](const PolyMesh& m, const std::vector<std::vector<int>>& g) { double d = 0; std::vector<std::array<double, 2>> c; for (auto& s : g) { const auto b = uv::box(m, uv::faceCorners(*T, s)); c.push_back({(b[0] + b[2]) / 2, (b[1] + b[3]) / 2}); }
          for (size_t i = 0; i < c.size(); i++) for (size_t j = i + 1; j < c.size(); j++) d += std::hypot(c[i][0] - c[j][0], c[i][1] - c[j][1]); return d / std::max<size_t>(1, c.size() * (c.size() - 1) / 2); };
        std::vector<std::vector<int>> grp; for (size_t i = sh.size() - 1; grp.size() < 6 && i > 0; i--) grp.push_back(sh[i]);   // (the six smallest)
        PolyMesh a = u, b = u; uv::PackOpts o; uv::packShapes(a, *T, fs, o); std::vector<uv::ShellAttr> at(size_t(T->nf)); for (auto& s : grp) for (int f : s) at[size_t(f)].group = 1; o.attr = &at; uv::packShapes(b, *T, fs, o);
        std::printf("  head, six small shells grouped: their mean distance apart %.3f (ungrouped %.3f)\n", spread(b, grp), spread(a, grp)); }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "auto") {              // (Auto seams: each UV Mat's seams cleared — its closed pieces opened by them, unfolded)
      std::map<int, std::vector<int>> byMat; for (int f = 0; f < T->nf; f++) byMat[base->faceMat.empty() ? -1 : base->faceMat[size_t(f)]].push_back(f);
      for (auto& [mat, fs] : byMat) {
        PolyMesh m = *base; m.seams.clear(); uv::PackOpts po;
        t.start(); auto r0 = uv::unfold(m, *T, fs, false, true, nullptr, nullptr, &po); const qint64 m0 = t.elapsed();
        std::printf("  mat %d (%zu polygons): no seams — %d unfolded, %d closed (%lld ms)\n", mat, fs.size(), r0.done, r0.closed, m0);
        if (!r0.closed) continue;
        t.start(); const auto es = uv::autoSeams(m, *T, r0.closedFaces); const qint64 m1 = t.elapsed(); std::vector<char> fl(size_t(T->ne), 0); for (int e : es) fl[size_t(e)] = 1; uv::setSeams(m, *T, fl);
        t.start(); auto r1 = uv::unfold(m, *T, r0.closedFaces, false, true, nullptr, nullptr, &po); const qint64 m2 = t.elapsed();
        int flips = 0; for (int f : r0.closedFaces) if (uv::faceArea(m, f) < 0) flips++;
        const auto a = uv::angleStretch(m, *T); double sa = 0; int bad = 0; for (int f : r0.closedFaces) { sa += a[size_t(f)]; if (a[size_t(f)] > 1.5f) bad++; }
        uv::CheckOpts co; const auto ck = uv::check(m, *T, r0.closedFaces, co);
        std::printf("    auto seams: %zu edges (%lld ms) → %d unfolded, %d still closed, %d turned over, overlap %zu, angle stretch mean %.3f, %d over 1.5 of %zu (%lld ms)\n",
                    es.size(), m1, r1.done, r1.closed, flips, ck.overlap.size(), sa / std::max<size_t>(1, r0.closedFaces.size()), bad, r0.closedFaces.size(), m2);
      }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "tangle") {            // (each shell unfolded on its own, the ones still lying over themselves named)
      PolyMesh m = *base; const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P); int bad = 0, n = 0;
      std::vector<std::vector<int>> by(size_t(S.count)); for (int f = 0; f < T->nf; f++) by[size_t(S.of[size_t(f)])].push_back(f);
      for (auto& fs : by) { if (fs.size() < 20) continue; n++; uv::PackOpts po; auto r = uv::unfold(m, *T, fs, false, true, nullptr, nullptr, &po, nullptr);
        uv::CheckOpts o; o.w = 2048; o.h = 2048; o.pad = 0; const auto rep = uv::check(m, *T, fs, o); if (!rep.overlap.empty() || !rep.flipped.empty()) { bad++; std::printf("  shell of %zu polygons: overlap %zu flipped %zu (mended %d untangled %d)\n", fs.size(), rep.overlap.size(), rep.flipped.size(), r.mended, r.untangled);
          if (rep.overlap.size() <= 5) for (int f : rep.overlap) { std::printf("    face %d: %d corners, UV area %.3e, 3D area %.3e:", f, T->fo[size_t(f) + 1] - T->fo[size_t(f)], uv::faceArea(m, f), uv::faceArea3(m, f)); for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) std::printf(" (%.4f,%.4f)", m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]); std::printf("\n"); } } }
      std::printf("  %d of %d shells still lie over themselves\n", bad, n); continue;
    }
    if (argc > 3 && QString(argv[3]) == "check") {          // (the checks: own UV; symmetric unfold + pack; the old box pack on a 2:1)
      const auto mc = uv::mirrorCorners(*base, *T); std::map<int, std::vector<int>> byMat; for (int f = 0; f < T->nf; f++) byMat[base->faceMat.empty() ? -1 : base->faceMat[size_t(f)]].push_back(f);
      auto show = [&](const char* what, const PolyMesh& m, const std::vector<int>& fs, int W, int H) {
        uv::CheckOpts o; o.w = W; o.h = H; o.pad = 8; o.mc = &mc; t.start(); const auto r = uv::check(m, *T, fs, o); const qint64 ms = t.elapsed();
        std::printf("    %-26s flipped %4zu  overlap %5zu (stacks %d)  outside %5zu  padding %5zu  density %5zu (ref %.2f)  asym %5zu   %lld ms\n", what, r.flipped.size(), r.overlap.size(), r.stacks, r.outside.size(), r.padding.size(), r.density.size(), r.tdRef, r.asym.size(), ms); };
      for (auto& [mat, fs] : byMat) {
        std::printf("  mat %d (%zu polygons)\n", mat, fs.size());
        show("own UV", *base, fs, 2048, 2048);
        PolyMesh m = *base; { uv::SymUnfold su; su.mc = mc; su.pairs = true; uv::PackOpts po; uv::unfold(m, *T, fs, false, true, nullptr, &su, &po); } show("sym unfold + pack 2048", m, fs, 2048, 2048);
        PolyMesh b = m; uv::pack(b, *T, fs, 8.0 / 2048, true); show("old box pack, 2048x1024", b, fs, 2048, 1024);
        PolyMesh c = m; { const uv::Points P = uv::points(c, *T); const uv::Shells S = uv::shells(c, *T, P); int s0 = S.of[size_t(fs[0])];   // (one shell dragged over the middle: overlaps)
          std::vector<int> ks; for (int f : fs) if (S.of[size_t(f)] == s0) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) ks.push_back(k);
          const auto bx = uv::box(c, ks); uv::transform(c, ks, [&](const uv::UV& q) { return uv::UV{float(q[0] - (bx[0] + bx[2]) / 2 + 0.5), float(q[1] - (bx[1] + bx[3]) / 2 + 0.5)}; }); }
        show("one shell dragged to middle", c, fs, 2048, 2048);
      }
      { PolyMesh m = *base; t.start(); const auto a = uv::angleStretch(m, *T); const qint64 ms = t.elapsed(); int bad = 0; for (float x : a) if (x > 1.5f) bad++; std::printf("  angle stretch: %d of %zu polygons over 1.5 (%lld ms)\n", bad, a.size(), ms); }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "align") {             // (Grid on a UV Mat's shells; Stack of mirror pairs)
      std::vector<int> fs; const int mat = argc > 4 ? std::atoi(argv[4]) : 2; for (int f = 0; f < T->nf; f++) if (base->faceMat.empty() || base->faceMat[size_t(f)] == mat) fs.push_back(f);
      PolyMesh u0 = *base; { uv::SymUnfold su; su.mc = uv::mirrorCorners(*base, *T); su.pairs = true; uv::unfold(u0, *T, fs, true, true, nullptr, &su); }
      line("unfolded (sym)", score(u0, *T), 0);
      { PolyMesh m = u0; t.start(); const int n = uv::rectify(m, *T, fs); const qint64 ms = t.elapsed(); line("Grid", score(m, *T), ms);
        // (how level / upright the quads' sides lie: per quad side the smaller of |du|, |dv| over its length — 0 for a grid)
        double worst = 0, sum = 0; int cnt = 0, quads = 0; for (int f : fs) { if (T->faceSize(f) != 4) continue; quads++;
          for (int j = 0; j < 4; j++) { const auto& a = m.cornerUV[size_t(T->fo[size_t(f)] + j)]; const auto& b = m.cornerUV[size_t(T->fo[size_t(f)] + (j + 1) % 4)];
            const double du = std::abs(b[0] - a[0]), dv = std::abs(b[1] - a[1]), L = std::hypot(du, dv); if (L <= 0) continue; const double off = std::min(du, dv) / L; worst = std::max(worst, off); sum += off; cnt++; } }
        int offQuads = 0; for (int f : fs) { if (T->faceSize(f) != 4) continue; double o = 0; for (int j = 0; j < 4; j++) { const auto& a = m.cornerUV[size_t(T->fo[size_t(f)] + j)]; const auto& b = m.cornerUV[size_t(T->fo[size_t(f)] + (j + 1) % 4)]; const double du = std::abs(b[0] - a[0]), dv = std::abs(b[1] - a[1]), L = std::hypot(du, dv); if (L > 0) o = std::max(o, std::min(du, dv) / L); } if (o > 1e-3) offQuads++; }
        std::printf("    (%d shells; quads %d: sides off level/upright mean %.2e, worst %.2e; quads not on the grid %d)\n", n, quads, cnt ? sum / cnt : 0, worst, offQuads);
        // (the shells all of quads: how many came out an exact grid — every side level or upright within 0.1%)
        const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P); std::map<int, std::pair<bool, double>> st;
        for (int f : fs) { auto& s = st.emplace(S.of[size_t(f)], std::make_pair(true, 0.0)).first->second; if (T->faceSize(f) != 4) { s.first = false; continue; }
          for (int j = 0; j < 4; j++) { const auto& a = m.cornerUV[size_t(T->fo[size_t(f)] + j)]; const auto& b = m.cornerUV[size_t(T->fo[size_t(f)] + (j + 1) % 4)]; const double du = std::abs(b[0] - a[0]), dv = std::abs(b[1] - a[1]), L = std::hypot(du, dv); if (L > 0) s.second = std::max(s.second, std::min(du, dv) / L); } }
        int allQ = 0, exact = 0, near = 0; for (auto& [s, v] : st) if (v.first) { allQ++; if (v.second < 1e-3) exact++; else if (v.second < 0.05) near++; }
        std::printf("    (shells all of quads: %d — an exact grid %d, within 5%% %d)\n", allQ, exact, near); }
      { PolyMesh m = u0; const auto mc = uv::mirrorCorners(m, *T); const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P);
        // (the mirror pairs: the −X shell stacked onto its +X twin — how far each corner is from its twin's)
        std::map<int, int> to; for (int f : fs) { const int k = T->fo[size_t(f)]; if (mc[size_t(k)] >= 0) to[S.of[size_t(f)]] = S.of[size_t(T->cf[size_t(mc[size_t(k)])])]; }
        int pairs = 0; double worst = 0, size = 0; t.start();
        for (auto& [a, b] : to) { if (a == b || a > b) continue; int fa = -1, fb = -1; for (int f : fs) { if (fa < 0 && S.of[size_t(f)] == a) fa = f; if (fb < 0 && S.of[size_t(f)] == b) fb = f; }
          if (fa < 0 || fb < 0) continue; uv::stackOnto(m, *T, {fa}, fb); pairs++; }
        const qint64 ms = t.elapsed();
        for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int p = mc[size_t(k)]; if (p < 0) continue; const int sa = S.of[size_t(f)], sb = S.of[size_t(T->cf[size_t(p)])]; if (sa == sb) continue;
          worst = std::max(worst, double(std::hypot(m.cornerUV[size_t(k)][0] - m.cornerUV[size_t(p)][0], m.cornerUV[size_t(k)][1] - m.cornerUV[size_t(p)][1]))); }
        const auto bb = uv::box(m, uv::faceCorners(*T, fs)); size = std::max(bb[2] - bb[0], bb[3] - bb[1]);
        std::printf("    Stack mirror pairs: %d stacked in %lld ms; corners off their twins at most %.2e (of the layout's size %.3f)\n", pairs, ms, worst, size); }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "face") {               // (the face: its holes found, unfolded symmetric — no features, open, closed)
      const auto mc = uv::mirrorCorners(*base, *T); std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (base->faceMat.empty() || base->faceMat[size_t(f)] == (argc > 4 ? std::atoi(argv[4]) : 0)) fs.push_back(f);
      auto feats = uv::detectFeatures(*base, *T, fs);
      std::printf("  %zu polygons, %zu features (holes):\n", fs.size(), feats.size());
      for (auto& f : feats) std::printf("    corners %d %d: upper %zu, lower %zu vertices; middle at x %.2f y %.2f\n", f.a.front(), f.a.back(), f.a.size(), f.b.size(),
        (base->pos[size_t(f.a.front())].x + base->pos[size_t(f.a.back())].x) / 2, (base->pos[size_t(f.a.front())].y + base->pos[size_t(f.a.back())].y) / 2);
      for (int mode = 0; mode < 3; mode++) {
        PolyMesh m = *base; uv::SymUnfold su; su.mc = mc; su.pairs = true; auto fe = feats; for (auto& f : fe) f.closed = mode == 2;
        t.start(); const auto u = uv::unfold(m, *T, fs, true, true, nullptr, &su, nullptr, mode ? &fe : nullptr); const qint64 ms = t.elapsed();
        // (each pair's points: how far off one vertical (open) / one point (closed); the corners off level) — over the corners of the faces
        double du = 0, dd = 0, lv = 0; int n = 0;
        const uv::Points Pq = uv::points(m, *T); const uv::Shells Sq = uv::shells(m, *T, Pq); int inShell = -1;
        auto uvOf = [&](int v) { for (int f : fs) if (Sq.of[size_t(f)] == inShell) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) if (T->fv[size_t(k)] == v) return m.cornerUV[size_t(k)]; return uv::UV{0, 0}; };
        for (auto& f : fe) { std::map<int, int> cnt; std::set<int> fv(f.a.begin(), f.a.end());   // (the shell the feature lies in: the most of its points)
          for (int x : fs) for (int k = T->fo[size_t(x)]; k < T->fo[size_t(x) + 1]; k++) if (fv.count(T->fv[size_t(k)])) cnt[Sq.of[size_t(x)]]++;
          inShell = -1; int bc = 0; for (auto& [s, c] : cnt) if (c > bc) { bc = c; inShell = s; }
          const auto ft = uv::featureTies({f}, [&] { std::vector<int> id(m.pos.size()); std::iota(id.begin(), id.end(), 0); return id; }(), m.pos);
          double fu = 0, fd = 0;
          for (auto& p : ft.pairs) { const auto a = uvOf(p[1]), b = uvOf(p[0]); fu = std::max(fu, double(std::abs(a[0] - b[0]))); fd = std::max(fd, double(std::hypot(a[0] - b[0], a[1] - b[1]))); n++; }
          du = std::max(du, fu); dd = std::max(dd, fd); lv = std::max(lv, double(std::abs(uvOf(f.a.front())[1] - uvOf(f.a.back())[1])));
          std::printf("        feature %d-%d: %zu pairs, off one vertical %.1e, apart %.1e, corners off level %.1e\n", f.a.front(), f.a.back(), ft.pairs.size(), fu, fd, double(std::abs(uvOf(f.a.front())[1] - uvOf(f.a.back())[1]))); }
        line(mode == 0 ? "symmetric, no features" : mode == 1 ? "features open" : "features closed", score(m, *T), ms);
        { // (the triangles of these polygons: turned over for real, against flat — shut lips' own have no area)
          std::vector<double> so(size_t(Sq.count), 0); auto ar = [&](int a, int b, int c) { const auto &p = m.cornerUV[size_t(a)], &q = m.cornerUV[size_t(b)], &r = m.cornerUV[size_t(c)]; return (double(q[0]) - p[0]) * (double(r[1]) - p[1]) - (double(r[0]) - p[0]) * (double(q[1]) - p[1]); };
          for (int f : fs) for (int k = T->fo[size_t(f)] + 1; k + 1 < T->fo[size_t(f) + 1]; k++) so[size_t(Sq.of[size_t(f)])] += ar(T->fo[size_t(f)], k, k + 1);
          int inv = 0, flat = 0; for (int f : fs) for (int k = T->fo[size_t(f)] + 1; k + 1 < T->fo[size_t(f) + 1]; k++) { const double a = ar(T->fo[size_t(f)], k, k + 1) * (so[size_t(Sq.of[size_t(f)])] < 0 ? -1 : 1); if (a < -1e-12) inv++; else if (a < 1e-12) flat++; }
          std::printf("      (fan triangles of these polygons: turned over %d, flat %d)\n", inv, flat); }
        std::printf("      (symmetric %d, featured %d; pairs %d: off one vertical %.1e, apart %.1e; corners off level %.1e)\n", u.symmetric, u.featured, n, du, dd, lv);
        if (argc > 5) { QString p = QString::fromLocal8Bit(argv[5]) + QString("_%1.obj").arg(mode); FILE* o = std::fopen(qPrintable(p), "w");   // (the UV as an OBJ to look at)
          if (o) { for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) std::fprintf(o, "v %f %f 0\n", m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]);
            int c = 1; for (int f : fs) { std::fprintf(o, "f"); for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) std::fprintf(o, " %d", c++); std::fprintf(o, "\n"); } std::fclose(o); } }
      }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "pack") {              // (Pack: each UV Mat on its own tile — the old boxes, by shape, symmetric)
      const int W = argc > 4 ? std::atoi(argv[4]) : 2048, H = argc > 5 ? std::atoi(argv[5]) : 2048; const double pad = argc > 6 ? std::atof(argv[6]) : 8;
      const auto mc = uv::mirrorCorners(*base, *T); std::map<int, std::vector<int>> byMat; for (int f = 0; f < T->nf; f++) byMat[base->faceMat.empty() ? -1 : base->faceMat[size_t(f)]].push_back(f);
      std::printf("  texture %d x %d, padding %.0f px; %zu UV Mats\n", W, H, pad, byMat.size());
      for (auto& [mat, fs] : byMat) {
        std::printf("  mat %d '%s': %zu polygons\n", mat, mat >= 0 && size_t(mat) < base->matNames.size() ? qPrintable(base->matNames[size_t(mat)]) : "-", fs.size());
        { PolyMesh m = *base; t.start(); const double f = uv::pack(m, *T, fs, pad / std::max(W, H), true); const qint64 ms = t.elapsed();
          double a = 0; for (int x : fs) a += std::abs(uv::faceArea(m, x)); (void)f; packLine("boxes (old)", m, *T, fs, W, H, pad, a, ms); }
        { PolyMesh m = *base; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed(); packLine(pr.ok ? "by shape" : "by shape FAILED", m, *T, fs, W, H, pad, pr.fill, ms); }
        { PolyMesh m = *base; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; o.passes = 6; o.final = true; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed(); packLine(pr.ok ? "by shape FINAL" : "by shape FINAL FAILED", m, *T, fs, W, H, pad, pr.fill, ms); }
        { PolyMesh m = *base; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; o.rotate = false; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed(); packLine("no turns, quick", m, *T, fs, W, H, pad, pr.fill, ms); }
        { PolyMesh m = *base; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; o.rotate = false; o.passes = 6; o.final = true; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed(); packLine("no turns, FINAL", m, *T, fs, W, H, pad, pr.fill, ms); }
        PolyMesh u0 = *base; { uv::SymUnfold su; su.mc = mc; su.pairs = true; su.axis = 0.5; uv::unfold(u0, *T, fs, true, true, nullptr, &su); }   // (the same symmetric unfold for both)
        { PolyMesh m = u0; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed(); packLine(pr.ok ? "unfold, by shape" : "unfold, by shape FAILED", m, *T, fs, W, H, pad, pr.fill, ms); }
        { PolyMesh m = u0; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; o.mc = &mc; o.passes = 6; o.final = true; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed(); packLine(pr.ok ? "unfold, symmetric FINAL" : "unfold, symmetric FINAL FAILED", m, *T, fs, W, H, pad, pr.fill, ms); std::printf("      (final: centred %d off %d pairs %d split %d free %d)\n", pr.centred, pr.offAxis, pr.pairs, pr.split, pr.free); }
        { PolyMesh m = u0; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; o.mc = &mc; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed();
          packLine(pr.ok ? "unfold, symmetric" : "unfold, symmetric FAILED", m, *T, fs, W, H, pad, pr.fill, ms);
          // (the mirror, over the shells whose every corner's mirror lies in one shell: a pair — about U = axis, one across the plane — on the axis)
          const uv::Points Pp = uv::points(m, *T); const uv::Shells Sh = uv::shells(m, *T, Pp); std::vector<int> to(size_t(Sh.count), -2); std::set<int> in(fs.begin(), fs.end());
          for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int p = mc[size_t(k)]; const int g = p >= 0 && in.count(T->cf[size_t(p)]) ? Sh.of[size_t(T->cf[size_t(p)])] : -1; int& x = to[size_t(Sh.of[size_t(f)])]; if (x == -2) x = g; else if (x != g) x = -1; }
          double worst = 0, cw = 0; int n = 0, cn = 0; std::vector<double> mid(size_t(Sh.count), 0); std::vector<int> mn(size_t(Sh.count), 0);
          for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int s = Sh.of[size_t(f)], p = mc[size_t(k)]; if (to[size_t(s)] == s && p >= 0) { mid[size_t(s)] += (m.cornerUV[size_t(k)][0] + m.cornerUV[size_t(p)][0]) / 2.0; mn[size_t(s)]++; } }
          int onAxis = 0; for (int s = 0; s < Sh.count; s++) if (mn[size_t(s)] && std::abs(mid[size_t(s)] / mn[size_t(s)] - pr.axis) < 1e-5) onAxis++;
          for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int s = Sh.of[size_t(f)], g = to[size_t(s)], p = mc[size_t(k)]; if (g < 0 || p < 0) continue;
            if (g == s) { cw = std::max(cw, std::abs((m.cornerUV[size_t(k)][0] + m.cornerUV[size_t(p)][0]) / 2.0 - mid[size_t(s)] / mn[size_t(s)])); cn++; }   // (each about its own middle line)
            else if (to[size_t(g)] == s) { worst = std::max(worst, double(std::hypot(m.cornerUV[size_t(p)][0] - (2 * pr.axis - m.cornerUV[size_t(k)][0]), m.cornerUV[size_t(p)][1] - m.cornerUV[size_t(k)][1]))); n++; } }
          std::printf("      (centred %d (off the axis %d; on it found %d), pairs %d (split %d), free %d; pairs off the mirror %.1e over %d corners; across the plane off its own middle %.1e over %d)\n", pr.centred, pr.offAxis, onAxis, pr.pairs, pr.split, pr.free, worst, n, cw, cn); }
      }
      continue;
    }
    line("own UV", score(*base, *T), 0);
    { PolyMesh m = *base; t.start(); auto u = uv::unfold(m, *T, {}, false, false, nullptr); const qint64 ms = t.elapsed(); line("Unfold", score(m, *T), ms); if (u.closed) std::printf("    (closed shells left: %d)\n", u.closed); }
    { PolyMesh m = *base; t.start(); uv::unfold(m, *T, {}, false, true, nullptr); const qint64 ms = t.elapsed(); line("Unfold + optimize", score(m, *T), ms); }
    { PolyMesh m = *base; t.start(); uv::optimize(m, *T, {}, nullptr); const qint64 ms = t.elapsed(); line("Optimize own UV", score(m, *T), ms); }
    // pins: every 40th inner edge; their ends must not move (Unfold + optimize, Optimize)
    uv::Pins pins; for (int e = 0; e < T->ne; e += 40) if (T->ef1[size_t(e)] >= 0) pins.insert({T->ea[size_t(e)], T->eb[size_t(e)]});
    auto moved = [&](const PolyMesh& m) { double d = 0; for (int f = 0; f < T->nf; f++) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) {
        const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const int a = T->fv[size_t(k)], b = T->fv[size_t(nk)];
        if (pins.count({std::min(a, b), std::max(a, b)})) d = std::max(d, double(std::hypot(m.cornerUV[size_t(k)][0] - base->cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1] - base->cornerUV[size_t(k)][1]))); }
      return d; };
    { PolyMesh m = *base; t.start(); std::vector<int> fs(size_t(T->nf)); for (int f = 0; f < T->nf; f++) fs[size_t(f)] = f; uv::unfold(m, *T, fs, true, true, &pins); const qint64 ms = t.elapsed(); line("Unfold pinned", score(m, *T), ms); std::printf("    (pinned points moved at most %.2e)\n", moved(m)); }
    { PolyMesh m = *base; t.start(); uv::optimize(m, *T, {}, &pins); const qint64 ms = t.elapsed(); line("Optimize pinned", score(m, *T), ms); std::printf("    (pinned points moved at most %.2e)\n", moved(m)); }
    // symmetry: Unfold symmetric (pairs on, in place — no packing) and Symmetrize (from +X): how far each corner is from its
    // mirror's place (a shell across the middle — about its own middle; a pair — about U = 0.5), in parts of the UV's size
    const auto mc = uv::mirrorCorners(*base, *T); int withMirror = 0; for (int k : mc) if (k >= 0) withMirror++;
    std::printf("    corners with a mirror: %d of %zu\n", withMirror, mc.size());
    if (!withMirror) continue;
    auto symErr = [&](const PolyMesh& m, double& selfErr, double& pairErr, const std::vector<int>& skip = {}) {   // (skip: polygons whose shells are left out — not unfolded)
      std::vector<char> skipF(size_t(T->nf), 0); for (int f : skip) skipF[size_t(f)] = 1;
      const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P);
      std::vector<double> mid(size_t(S.count), 0); std::vector<int> cnt(size_t(S.count), 0);
      for (size_t k = 0; k < mc.size(); k++) { const int p = mc[k]; if (p < 0) continue; const int s = S.of[size_t(T->cf[k])]; if (S.of[size_t(T->cf[size_t(p)])] == s) { mid[size_t(s)] += (m.cornerUV[k][0] + m.cornerUV[size_t(p)][0]) / 2.0; cnt[size_t(s)]++; } }
      std::vector<int> allK(mc.size()); for (size_t k = 0; k < mc.size(); k++) allK[k] = int(k);
      const auto b = uv::box(m, allK); const double size = std::max({b[2] - b[0], b[3] - b[1], 1e-12});
      // (only shells whose every corner's mirror lies in one shell — itself, or its pair)
      std::vector<int> to(size_t(S.count), -2);
      for (size_t k = 0; k < mc.size(); k++) { const int s = S.of[size_t(T->cf[k])]; const int g = mc[k] >= 0 && !skipF[size_t(T->cf[k])] && !skipF[size_t(T->cf[size_t(mc[k])])] ? S.of[size_t(T->cf[size_t(mc[k])])] : -1; int& x = to[size_t(s)]; if (x == -2) x = g; else if (x != g) x = -1; }
      selfErr = pairErr = 0;
      for (size_t k = 0; k < mc.size(); k++) { const int p = mc[k]; if (p < 0) continue; const int s = S.of[size_t(T->cf[k])]; if (to[size_t(s)] < 0) continue; const bool self = S.of[size_t(T->cf[size_t(p)])] == s;
        const double a = self ? mid[size_t(s)] / cnt[size_t(s)] : 0.5;
        const double e = std::hypot(m.cornerUV[size_t(p)][0] - (2 * a - m.cornerUV[k][0]), m.cornerUV[size_t(p)][1] - m.cornerUV[k][1]) / size;
        if (e > (self ? selfErr : pairErr) && std::getenv("UVDBG") && e > 1e-3) std::printf("      worst so far: k %zu (x %.5f, v %d) p %d (x %.5f, v %d) self %d err %.3f uvk %.4f %.4f uvp %.4f %.4f a %.4f\n", k, m.pos[size_t(T->fv[k])].x, T->fv[k], p, m.pos[size_t(T->fv[size_t(p)])].x, T->fv[size_t(p)], int(self), e, m.cornerUV[k][0], m.cornerUV[k][1], m.cornerUV[size_t(p)][0], m.cornerUV[size_t(p)][1], a);
        (self ? selfErr : pairErr) = std::max(self ? selfErr : pairErr, e); }
    };
    std::vector<int> fs(size_t(T->nf)); for (int f = 0; f < T->nf; f++) fs[size_t(f)] = f;
    { PolyMesh m = *base; uv::SymUnfold su; su.mc = mc; su.pairs = true; su.axis = 0.5;
      t.start(); auto u = uv::unfold(m, *T, fs, true, true, nullptr, &su); const qint64 ms = t.elapsed(); line("Unfold symmetric", score(m, *T), ms);
      std::vector<int> skip = u.closedFaces; skip.insert(skip.end(), u.asymFaces.begin(), u.asymFaces.end()); double se, pe; symErr(m, se, pe, skip); std::printf("    (symmetric %d, mirrored %d, seams not symmetric %d; off the mirror: across the middle %.2e, pairs %.2e)\n", u.symmetric, u.mirrored, u.asym, se, pe);
      // the −X side spoilt (each UV point shifted its own way), then Symmetrize from +X — symmetric again?
      PolyMesh w = m; const auto b = uv::box(w, std::vector<int>(fs.size(), 0)); (void)b;
      { const uv::Points P = uv::points(w, *T); for (int p = 0; p < P.count(); p++) { if (!(w.pos[size_t(P.vert[size_t(p)])].x < -1e-3)) continue;
          const float du = 0.003f * float(std::sin(p * 1.7)), dv = 0.003f * float(std::cos(p * 2.3)); for (int i = P.off[size_t(p)]; i < P.off[size_t(p) + 1]; i++) { auto& q = w.cornerUV[size_t(P.list[size_t(i)])]; q[0] += du; q[1] += dv; } } }
      symErr(w, se, pe, skip); std::printf("    (the −X side spoilt: off the mirror %.2e / %.2e)\n", se, pe); line("  spoilt", score(w, *T), 0);
      const double eps = 1e-4 * w.bounds().size().length(); std::vector<char> sk(size_t(T->nf), 0); for (int f : skip) sk[size_t(f)] = 1;   // (the shells it unfolded only)
      std::vector<int> from; for (size_t k = 0; k < mc.size(); k++) if (w.pos[size_t(T->fv[k])].x > -eps && !sk[size_t(T->cf[k])] && (mc[k] < 0 || !sk[size_t(T->cf[size_t(mc[k])])])) from.push_back(int(k));
      const int n = uv::symmetrize(w, *T, mc, from, 0.5); symErr(w, se, pe, skip);
      line("Symmetrize from +X", score(w, *T), 0); std::printf("    (%d corners set; off the mirror: across the middle %.2e, pairs %.2e)\n", n, se, pe);
      { double worst = 0; size_t wk = 0; for (size_t k = 0; k < mc.size(); k++) { if (sk[size_t(T->cf[k])]) continue; const double d = std::hypot(w.cornerUV[k][0] - m.cornerUV[k][0], w.cornerUV[k][1] - m.cornerUV[k][1]); if (d > worst) { worst = d; wk = k; } }        std::vector<char> inFrom(mc.size(), 0); for (int k : from) inFrom[size_t(k)] = 1; const int p = mc[wk];        std::printf("    (back to the symmetric unfold: worst %.2e at corner %zu: x %.5f, in from %d, mirror %d (x %.5f, in from %d, same vertex %d))\n", worst, wk, w.pos[size_t(T->fv[wk])].x, int(inFrom[wk]), p, p >= 0 ? w.pos[size_t(T->fv[size_t(p)])].x : 0.0, p >= 0 ? int(inFrom[size_t(p)]) : -1, p >= 0 ? int(T->fv[wk] == T->fv[size_t(p)]) : -1); } }
  }
  return 0;
}
