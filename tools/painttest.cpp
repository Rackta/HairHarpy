// console check of the Paint room's core (no GPU): hm_painttest — layers, brush, ABR, baking on synthetic data
#include "paint/Abr.h"
#include <QFile>
#include <QFileInfo>
#include "paint/Bake.h"
#include "paint/LayerFx.h"
#include <QJsonArray>
#include "paint/Brush.h"
#include "paint/PaintDoc.h"
#include "paint/Psd.h"
#include "paint/Region.h"
#include "scene/Scene.h"
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace hm;
using namespace hm::paint;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL  %s  (line %d)\n", #c, __LINE__); fails++; } else std::printf("ok    %s\n", #c); } while (0)

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static long long memMB() { PROCESS_MEMORY_COUNTERS pm{}; GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm); return (long long)(pm.WorkingSetSize >> 20); }
static long long peakMB() { PROCESS_MEMORY_COUNTERS pm{}; GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm); return (long long)(pm.PeakWorkingSetSize >> 20); }
#else
static long long memMB() { return 0; }
static long long peakMB() { return 0; }
#endif
static void be16(QByteArray& b, int v) { b.append(char((v >> 8) & 255)); b.append(char(v & 255)); }
static void be32(QByteArray& b, int v) { be16(b, (v >> 16) & 0xffff); be16(b, v & 0xffff); }

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QGuiApplication app(argc, argv);
  QGuiApplication::setOrganizationName("Catarackta"); QGuiApplication::setApplicationName("HairHarpy");
  if (argc > 2 && QByteArray(argv[1]) == "--import") {   // hm_painttest --import a.abr b.abr: the brushes into the user's own library (the same folder the program reads)
    BrushLibrary lib; lib.load(); const size_t before = lib.brushes.size(); lib.brushes.resize(size_t(lib.builtin));   // (the library made anew from the files given)
    for (int i = 2; i < argc; i++) { QString err; const int n = lib.importAbr(QString::fromLocal8Bit(argv[i]), &err); std::printf("%s: %s\n", argv[i], n < 0 ? err.toLocal8Bit().constData() : (QString::number(n) + " brushes").toLocal8Bit().constData()); }
    std::printf("library: %d -> %d brushes, folder %s\n", int(before), int(lib.brushes.size()), BrushLibrary::folder().toLocal8Bit().constData()); return 0; }

  if (argc > 2 && QByteArray(argv[1]) == "--sets") {     // hm_painttest --sets "a.abr=Set name" …: the user's brushes that came from each file put into that set (presets; nothing else changes)
    BrushLibrary lib; lib.load(); int moved = 0;
    for (int i = 2; i < argc; i++) {
      const QString arg = QString::fromLocal8Bit(argv[i]); const int eq = arg.lastIndexOf('='); if (eq < 0) continue;
      const QString file = arg.left(eq), set = arg.mid(eq + 1), base = QFileInfo(file).completeBaseName();
      QFile f(file); if (!f.open(QIODevice::ReadOnly)) { std::printf("%s: can't open\n", file.toLocal8Bit().constData()); continue; }
      QString err; const auto list = readAbr(f.readAll(), &err); int n = 0;
      for (size_t k = size_t(lib.builtin); k < lib.brushes.size(); k++) { Brush& b = lib.brushes[k]; if (!b.set.isEmpty()) continue;
        bool from = b.name.startsWith(base + " ") && b.name.mid(base.size() + 1).toInt() > 0;   // (an unnamed one: "<file> N")
        for (const AbrBrush& a : list) if (!a.name.isEmpty() && a.name == b.name) from = true;
        if (from) { b.set = set; n++; } }
      std::printf("%s -> %s: %d brushes\n", base.toLocal8Bit().constData(), set.toLocal8Bit().constData(), n); moved += n; }
    lib.save(); std::printf("sets: %s (%d moved)\n", lib.sets().join(" | ").toLocal8Bit().constData(), moved); return 0; }

  if (argc > 1 && QByteArray(argv[1]) == "--bench") {    // hm_painttest --bench [size] [layers] [dir]: a heavy texture set timed — a stroke, the composed map, the save
    const int N = argc > 2 ? atoi(argv[2]) : 8192, NL = argc > 3 ? atoi(argv[3]) : 12; const QString dir = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString();
    QElapsedTimer t; t.start(); Document d(N, N); d.addFill(Channel::Color, QColor(128, 128, 128)); std::vector<int> ids;
    for (int i = 0; i < NL; i++) { ids.push_back(d.addPaint(Channel::Color)); Brush b; b.size = float(N) / 16; b.flow = 1; b.hardness = 0.4f; Stroke s(d.layer(ids.back())->image, b, QColor::fromHsv(i * 29 % 360, 200, 220));
      s.begin(QPointF(N * 0.1, N * (0.1 + 0.8 * i / std::max(1, NL - 1)))); s.move(QPointF(N * 0.9, N * (0.1 + 0.8 * i / std::max(1, NL - 1)))); }
    d.layer(ids[size_t(NL / 2)])->blend = Blend::Multiply; d.addMask(ids[0]);
    std::printf("%d x %d, %d paint layers: made in %lld ms, %lld MB\n", N, N, NL, t.elapsed(), memMB());
    Layer* L = d.layer(ids.back()); Brush b; b.size = 60; b.flow = 0.8f;
    const QImage was = L->image.copy(); const qint64 m0 = memMB(); qint64 tu = 0;
    t.restart(); { Stroke s(L->image, b, Qt::red); const qint64 tc = t.elapsed(); s.begin(QPointF(N * 0.3, N * 0.3)); QRect all;
      t.restart(); qint64 worst = 0, comp = 0, area = 0; int steps = 0;
      for (int i = 1; i <= 200; i++) { QElapsedTimer s1; s1.start(); s.move(QPointF(N * 0.3 + i * 4, N * 0.3 + 40 * std::sin(i * 0.1))); const QRect r = s.takeDirty(); all = all.united(r);
        QElapsedTimer c1; c1.start(); if (!r.isNull()) (void)d.compose(Channel::Color, r); comp += c1.nsecsElapsed(); area += qint64(r.width()) * r.height(); worst = std::max(worst, s1.elapsed()); steps++; }
      comp /= 1000000; std::printf("  (the parts composed: %lld px in all)\n", area);
      const qint64 ts = t.elapsed(); t.restart(); const auto tiles = s.takeBefore(); tu = t.elapsed(); qint64 kb = 0; for (auto& x : tiles) kb += x.img.sizeInBytes() >> 10;
      std::printf("a stroke (brush 60 px, 200 moves): begun in %lld ms, %lld ms in all (the composed part %lld ms), worst move %lld ms; its undo step: %zu tiles, %lld KB, taken in %lld ms; +%lld MB while it went\n", tc, ts, comp, worst, tiles.size(), kb, tu, memMB() - m0);
      // (the tiles put back: the layer as it was, to the pixel)
      QImage back = L->image; for (auto& x : tiles) for (int y = 0; y < x.img.height(); y++) std::memcpy(back.scanLine(x.at.y() + y) + x.at.x() * 4, x.img.constScanLine(y), size_t(x.img.width()) * 4);
      CHECK(back == was); CHECK(L->image != was); }
    t.restart(); QImage c = d.compose(Channel::Color); std::printf("the whole map composed: %lld ms, %lld MB at its peak\n", t.elapsed(), peakMB()); c = {};
    if (!dir.isEmpty()) { t.restart(); const bool ok = d.saveTo(dir); const qint64 t1 = t.elapsed(); t.restart(); d.saveTo(dir); std::printf("saved (%d): %lld ms; saved again, nothing changed: %lld ms\n", int(ok), t1, t.elapsed());
      Document e; e.fromJson(d.toJson()); t.restart(); e.loadFrom(dir); std::printf("loaded: %lld ms\n", t.elapsed());
      CHECK(e.layer(ids.back())->image == d.layer(ids.back())->image); CHECK(e.layer(ids[0])->mask == d.layer(ids[0])->mask); }
    std::printf("peak %lld MB\n", peakMB()); return fails ? 1 : 0; }

  // layers
  { Document d(64, 64); const int a = d.addPaint(Channel::Color); d.layer(a)->image.fill(QColor(200, 100, 50));
    const int f = d.addFill(Channel::Color, QColor(128, 128, 128)); d.layer(f)->blend = Blend::Multiply;
    QImage c = d.compose(Channel::Color); const QRgb p = c.pixel(10, 10);
    CHECK(std::abs(qRed(p) - 100) <= 1 && std::abs(qGreen(p) - 50) <= 1);
    d.addAdjust(AdjustKind::Invert); c = d.compose(Channel::Color); CHECK(std::abs(qRed(c.pixel(3, 3)) - 155) <= 1);
    d.addMask(a, false); c = d.compose(Channel::Color); CHECK(qAlpha(c.pixel(3, 3)) == 255);   // (the Fill below the mask'd layer still covers)
    const int h = d.addPaint(Channel::Height); Brush b; b.mode = BrushMode::Raise; b.size = 20; b.flow = 1; b.strength = 1;
    Stroke s(d.layer(h)->image, b); CHECK(s.valid()); s.begin(QPointF(32, 32)); s.move(QPointF(40, 32));
    QImage hm_ = d.compose(Channel::Height); const int v = reinterpret_cast<const quint16*>(hm_.constScanLine(32))[34]; CHECK(v > 40000);
    CHECK(reinterpret_cast<const quint16*>(hm_.constScanLine(2))[2] == 32768);
    const int fl = d.addPaint(Channel::Flow); Brush fb; fb.mode = BrushMode::Flow; fb.size = 16; fb.flow = 1; Stroke fs(d.layer(fl)->image, fb); fs.begin(QPointF(8, 32)); fs.move(QPointF(50, 32));
    QImage fm = d.compose(Channel::Flow); const uchar* q = fm.constScanLine(32) + 30 * 4; CHECK(q[0] > 200 && std::abs(q[1] - 128) < 20 && q[3] > 200);   // (flowing right: +x)
    { const int ml = d.addPaint(Channel::Color); d.addMask(ml, true); QImage& mk = d.layer(ml)->mask; Brush mb; mb.mode = BrushMode::MaskReveal; mb.size = 16; mb.flow = 1; mb.hardness = 1;   // (a mask painted as Photoshop: black hides, gray half)
      { Stroke ms(mk, mb, Qt::black); ms.maskInk = true; ms.begin(QPointF(20, 20)); ms.move(QPointF(21, 20)); } CHECK(qGray(mk.pixel(20, 20)) < 20);
      { Stroke ms(mk, mb, QColor(128, 128, 128)); ms.maskInk = true; ms.begin(QPointF(20, 20)); ms.move(QPointF(21, 20)); } CHECK(std::abs(qGray(mk.pixel(20, 20)) - 128) < 20);
      { Stroke ms(mk, mb, Qt::white); ms.maskInk = true; ms.begin(QPointF(20, 20)); ms.move(QPointF(21, 20)); } CHECK(qGray(mk.pixel(20, 20)) > 235); d.remove(ml); }
    { QImage im(260, 160, QImage::Format_ARGB32); im.fill(Qt::transparent); Brush cb; cb.tipKind = TipKind::Cylinder; cb.orient = 1; cb.followStroke = true; cb.size = 60; cb.spacing = 0.01f; cb.flow = 1; cb.pressureSize = false;   // (along the stroke: a hand that slows and shakes at the end leaves no spun blob)
      { Stroke st(im, cb, Qt::white); st.begin(QPointF(20, 80)); for (int x = 24; x <= 200; x += 4) st.move(QPointF(x, 80));
        unsigned r = 7; for (int i = 0; i < 60; i++) { r = r * 1103515245u + 12345u; const double jx = int((r >> 16) % 11) / 10.0 - 0.5; r = r * 1103515245u + 12345u; const double jy = int((r >> 16) % 11) / 10.0 - 0.5; st.move(QPointF(200 + jx, 80 + jy)); } }
      int ext = 0; for (int y = 0; y < im.height(); y++) for (int x = 150; x < 250; x++) if (qAlpha(im.pixel(x, y)) > 128) ext = std::max(ext, std::abs(y - 80));
      std::printf("      cylinder along the stroke, shaky end: %d px from the middle (size 60)\n", ext); CHECK(ext <= 22); }   // (the old way: 30 — the spun blob)
    { QImage m(96, 96, QImage::Format_Grayscale8); m.fill(0);   // (a staircase edge of 8-texel steps, as a fill along polygon rows leaves it: smoothed, the edge kept crisp)
      for (int y = 0; y < 96; y++) for (int x = 0; x < 8 * (y / 8) + 4; x++) m.scanLine(y)[x] = 255;
      const QImage e = maskEdge(m, EdgeOp::Smooth, 8);
      int worstRamp = 0; double devWas = 0, devNow = 0; int rows = 0;
      for (int y = 16; y < 80; y++) { int ramp = 0; double edgeWas = 0, edgeNow = 0; for (int x = 0; x < 96; x++) { const int v = e.constScanLine(y)[x]; if (v > 32 && v < 223) ramp++; edgeNow += v / 255.0; edgeWas += m.constScanLine(y)[x] / 255.0; }
        worstRamp = std::max(worstRamp, ramp); devWas += std::abs(edgeWas - (y + 0.5)); devNow += std::abs(edgeNow - (y + 0.5)); rows++; }
      std::printf("      staircase smoothed: mean distance from the straight edge %.2f -> %.2f texels, widest ramp %d texels\n", devWas / rows, devNow / rows, worstRamp);
      CHECK(devNow < devWas * 0.5 && worstRamp <= 3); }
    Document d2; CHECK(d2.fromJson(d.toJson()) && d2.layers().size() == d.layers().size());
    int n = int(d.layers().size()); d.remove(a); CHECK(int(d.layers().size()) == n - 1);
  }
  // the maps: each its own layers; a gray map over its ground; an adjustment only in the map it was made in
  { Document d(32, 32); CHECK(maps().size() == size_t(Channel::Count)); CHECK(channelFromKey("outline") == Channel::Outline && formatOf(Channel::Rim) == QImage::Format_ARGB32 && formatOf(Channel::Height) == QImage::Format_Grayscale16);
    const int c = d.addPaint(Channel::Color); d.layer(c)->image.fill(QColor(200, 40, 40)); const int o = d.addPaint(Channel::Outline);
    Brush b; b.size = 12; b.flow = 1; b.hardness = 1; b.pressureSize = false; { Stroke s(d.layer(o)->image, b, QColor(0, 0, 0)); CHECK(s.valid()); s.begin(QPointF(8, 8)); }
    QImage fo = d.flatten(Channel::Outline); CHECK(fo.format() == QImage::Format_Grayscale8 && fo.constScanLine(8)[8] < 10 && fo.constScanLine(28)[28] == 255);   // (painted black; elsewhere the ground — white: the full line)
    CHECK(d.flatten(Channel::Specular).constScanLine(4)[4] == 0 && d.flatten(Channel::ShadowMask).constScanLine(4)[4] == 128);
    CHECK(qRed(d.compose(Channel::Color).pixel(8, 8)) == 200);                                  // (the outline's paint did not come into the colour)
    d.addAdjust(AdjustKind::Invert, {}, -1, Channel::Outline); CHECK(d.flatten(Channel::Outline).constScanLine(8)[8] > 245 && qRed(d.compose(Channel::Color).pixel(8, 8)) == 200);
    CHECK(d.hasLayers(Channel::Outline) && !d.hasLayers(Channel::Rim)); }
  // the UV sets: one list of layers, each set its own pixels and size; saved, read back, the files of a deleted layer gone
  { const QString dir = QDir::tempPath() + "/hm_painttest_sets"; QDir(dir).removeRecursively();
    Document d(64, 64); const int a = d.addPaint(Channel::Color, "Lips"), bq = d.addPaint(Channel::Color, "Body paint"); d.layer(a)->image.fill(QColor(255, 0, 0)); d.addMask(a);
    d.setActiveSet("Body Set", 128, 128); CHECK(d.width() == 128 && d.layer(a)->image.isNull() && d.layer(a)->mask.isNull() && d.layers().size() == 2);   // (Lips: nothing in the body's set)
    CHECK(d.compose(Channel::Color).size() == QSize(128, 128) && qAlpha(d.compose(Channel::Color).pixel(5, 5)) == 0);
    d.ensurePixels(bq); d.layer(bq)->image.fill(QColor(0, 255, 0)); CHECK(d.layer(bq)->image.size() == QSize(128, 128));
    d.setActiveSet("", 64, 64); CHECK(qRed(d.compose(Channel::Color).pixel(5, 5)) == 255 && d.layer(bq)->image.size() == QSize(64, 64) && !d.layer(a)->mask.isNull());
    CHECK(d.saveTo(dir)); CHECK(QFileInfo::exists(dir + "/layer_" + QString::number(bq) + "@Body%20Set.png") && QFileInfo::exists(dir + "/mask_" + QString::number(a) + ".png"));
    Document e; e.fromJson(d.toJson()); CHECK(e.loadFrom(dir)); CHECK(e.activeSet().isEmpty() && e.sets().size() == 2 && qRed(e.compose(Channel::Color).pixel(5, 5)) == 255);
    e.setActiveSet("Body Set", 128, 128); CHECK(qGreen(e.compose(Channel::Color).pixel(5, 5)) == 255 && e.layer(a)->image.isNull());   // (read from the folder when first shown)
    e.remove(bq); CHECK(e.saveTo(dir)); CHECK(!QFileInfo::exists(dir + "/layer_" + QString::number(bq) + "@Body%20Set.png") && !QFileInfo::exists(dir + "/layer_" + QString::number(bq) + ".png") && QFileInfo::exists(dir + "/layer_" + QString::number(a) + ".png"));
    QDir(dir).removeRecursively(); }
  // the normal map: the bump alone; the bake through its mask; both
  { QImage h(32, 32, QImage::Format_Grayscale16); for (int y = 0; y < 32; y++) { quint16* p = reinterpret_cast<quint16*>(h.scanLine(y)); for (int x = 0; x < 32; x++) p[x] = quint16(32768 + (x - 16) * 600); }   // (a slope along x)
    QImage bk(32, 32, QImage::Format_RGB888); bk.fill(QColor(128, 220, 180)); QImage mk(32, 32, QImage::Format_Grayscale8); mk.fill(0); for (int y = 0; y < 32; y++) for (int x = 16; x < 32; x++) mk.scanLine(y)[x] = 255;
    const QImage n1 = finalNormal({}, {}, h, 1, false, QSize(32, 32)); CHECK(!n1.isNull() && n1.constScanLine(16)[16 * 3] < 120 && std::abs(n1.constScanLine(16)[16 * 3 + 1] - 128) < 3);
    const QImage n2 = finalNormal(bk, mk, {}, 1, false, QSize(32, 32)); const uchar* l = n2.constScanLine(8) + 4 * 3; const uchar* r = n2.constScanLine(8) + 24 * 3;
    CHECK(std::abs(l[0] - 128) < 2 && std::abs(l[1] - 128) < 2 && l[2] > 250);                 // (outside the mask: flat)
    CHECK(r[1] > 190 && r[2] < 230);                                                           // (inside: the bake's tilt)
    const QImage n3 = finalNormal(bk, {}, h, 1, false, QSize(64, 64)); CHECK(n3.size() == QSize(64, 64) && n3.constScanLine(20)[20 * 3 + 1] > 170 && n3.constScanLine(20)[20 * 3] < 126);   // (no mask: the whole bake; the bump over it)
    CHECK(finalNormal({}, {}, {}, 1, false, QSize(8, 8)).isNull()); }
  // ABR: a v1 file with one sampled brush
  { QByteArray b; be16(b, 1); be16(b, 1); be16(b, 2); QByteArray body; be32(body, 0); be16(body, 25); body.append(char(1));
    for (int i = 0; i < 4; i++) be16(body, 0); be32(body, 0); be32(body, 0); be32(body, 8); be32(body, 8); be16(body, 8); body.append(char(0));
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { const double r = std::hypot(x - 3.5, y - 3.5); body.append(char(r < 3.5 ? 255 - int(r * 60) : 0)); }
    be32(b, body.size()); b += body;
    QString err; auto list = readAbr(b, &err); CHECK(list.size() == 1 && list[0].tip.width() == 8 && list[0].tip.constScanLine(4)[4] > 200 && list[0].tip.constScanLine(0)[0] == 0); CHECK(std::fabs(list[0].spacing - 0.25f) < 1e-4f);
    // v6: the 8BIM samp block, one RLE sample
    QByteArray c; be16(c, 6); be16(c, 1); c += "8BIM"; c += "samp"; QByteArray samp, one; one.append(char(4)); one += "abcd"; one += QByteArray(10, '\0');
    be32(one, 0); be32(one, 0); be32(one, 4); be32(one, 4); be16(one, 8); one.append(char(1));
    for (int y = 0; y < 4; y++) be16(one, 2); for (int y = 0; y < 4; y++) { one.append(char(-3)); one.append(char(0)); }   // (PackBits: 4 × 0 per row)
    while (one.size() % 4) one.append(char(0));
    be32(samp, one.size()); samp += one; be32(c, samp.size()); c += samp;
    list = readAbr(c, &err); CHECK(list.size() == 1 && list[0].tip.width() == 4);
    CHECK(readAbr(QByteArray("junk junk"), &err).empty());
  }
  // layer effects: a square on a flat surface, the light from above; then the surface cut down the middle (a UV seam)
  { const int W = 64; std::vector<float> P(size_t(W) * W * 3), N(size_t(W) * W * 3);
    auto build = [&](bool seam) { for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) { const size_t i = (size_t(y) * W + x) * 3; P[i] = float(x) + (seam && x >= 32 ? 500.f : 0.f); P[i + 1] = float(-y); P[i + 2] = 0; N[i] = 0; N[i + 1] = 0; N[i + 2] = 1; } };
    QImage a(W, W, QImage::Format_Grayscale8); a.fill(0); for (int y = 16; y < 48; y++) for (int x = 16; x < 48; x++) a.scanLine(y)[x] = 255;
    float L[3]; fxLight(0, 30, L); build(false); auto F = fxFrame(W, W, P.data(), N.data(), L);
    CHECK(F->ly[size_t(32) * W + 32] < -100);                                                  // (toward the light: up the picture)
    QJsonObject st = fxDefaults("stroke"); st.insert("size", 4); const QImage s1 = fxRender(a, *F, st);
    CHECK(qAlpha(s1.pixel(18, 32)) > 250 && qAlpha(s1.pixel(30, 32)) == 0 && qAlpha(s1.pixel(10, 32)) == 0);   // (inside 4 texels of the edge only)
    st.insert("pos", "outside"); const QImage s2 = fxRender(a, *F, st); CHECK(qAlpha(s2.pixel(13, 32)) > 250 && qAlpha(s2.pixel(18, 32)) == 0);
    QJsonObject bv = fxDefaults("bevel"); bv.insert("size", 6); const QImage b1 = fxRender(a, *F, bv);
    const QRgb top = b1.pixel(32, 17), bot = b1.pixel(32, 46); std::printf("      bevel: top %d alpha %d / bottom %d alpha %d\n", qRed(top), qAlpha(top), qRed(bot), qAlpha(bot));
    CHECK(qRed(top) > 200 && qAlpha(top) > 100 && qRed(bot) < 50 && qAlpha(bot) > 100);       // (the top edge lit, the bottom one dark)
    QJsonObject in = fxDefaults("inner"); const QImage i1 = fxRender(a, *F, in); std::printf("      inner: top %d bottom %d\n", qAlpha(i1.pixel(32, 17)), qAlpha(i1.pixel(32, 46)));
    CHECK(qAlpha(i1.pixel(32, 17)) > qAlpha(i1.pixel(32, 46)) + 40);                            // (the shadow under the edge facing the light)
    st.insert("pos", "inside"); st.insert("grad", true); st.insert("c1", "#ffffff"); st.insert("c2", "#000000"); const QImage g1 = fxRender(a, *F, st);
    CHECK(qRed(g1.pixel(32, 17)) > 200 && qRed(g1.pixel(32, 46)) < 60);                         // (the gradient on the stroke: light above, dark below)
    build(true); auto F2 = fxFrame(W, W, P.data(), N.data(), L); st.insert("grad", false); const QImage s3 = fxRender(a, *F2, st);
    std::printf("      seam: x31 alpha %d, x32 alpha %d (no stroke along the cut)\n", qAlpha(s3.pixel(31, 32)), qAlpha(s3.pixel(32, 32)));
    CHECK(qAlpha(s3.pixel(31, 32)) == 0 && qAlpha(s3.pixel(32, 32)) == 0 && qAlpha(s3.pixel(18, 32)) > 250); }
  { const int W = 4096; std::vector<float> P(size_t(W) * W * 3), N(size_t(W) * W * 3, 0.f); for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) { const size_t i = (size_t(y) * W + x) * 3; P[i] = float(x); P[i + 1] = float(-y); N[i + 2] = 1; }
    QImage a(W, W, QImage::Format_Grayscale8); a.fill(0); for (int y = 1000; y < 3000; y++) for (int x = 800; x < 3200; x++) a.scanLine(y)[x] = 255; float L[3]; fxLight(30, 55, L);
    QElapsedTimer t; t.start(); auto F = fxFrame(W, W, P.data(), N.data(), L); const qint64 tf = t.restart(); const QImage r = fxRender(a, *F, fxDefaults("stroke")); std::printf("      4096 texture: frame %lld ms, stroke %lld ms\n", tf, t.elapsed()); CHECK(!r.isNull()); }
  // the shine: a flat surface facing +Z, the light from above-front; seen from the mirror direction — the full highlight, from the front — a faint one
  { const int W = 16; std::vector<float> P(size_t(W) * W * 3), N(size_t(W) * W * 3, 0.f); for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) { const size_t i = (size_t(y) * W + x) * 3; P[i] = float(x); P[i + 1] = float(-y); N[i + 2] = 1; }
    QImage a(W, W, QImage::Format_Grayscale8); a.fill(255); float L[3]; fxLight(0, 55, L); auto F = fxFrame(W, W, P.data(), N.data(), L);
    QJsonObject sh = fxDefaults("shine"); sh.insert("az", 0); sh.insert("el", 55); sh.insert("metal", 1.0); sh.insert("c1", "#ffcc00"); sh.insert("rough", 0.3); sh.insert("power", 1.0);
    sh.insert("vd", QJsonArray{0, -L[1], L[2]}); const QImage m1 = fxRender(a, *F, sh);
    sh.insert("vd", QJsonArray{0, 0, 1}); const QImage m2 = fxRender(a, *F, sh);
    std::printf("      shine: mirror view alpha %d (colour %d,%d,%d), front view alpha %d\n", qAlpha(m1.pixel(8, 8)), qRed(m1.pixel(8, 8)), qGreen(m1.pixel(8, 8)), qBlue(m1.pixel(8, 8)), qAlpha(m2.pixel(8, 8)));
    CHECK(qAlpha(m1.pixel(8, 8)) > 240 && qAlpha(m2.pixel(8, 8)) < 40 && qBlue(m1.pixel(8, 8)) < 20 && qRed(m1.pixel(8, 8)) > 240); }
  // bake: a flat quad against a bumped grid of the same footprint
  { TriMesh low, high;
    low.pos = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}; low.tri = {{0, 1, 2}, {0, 2, 3}};
    const V3 up{0, 0, 1}; low.nrm = {{up, up, up}, {up, up, up}}; low.uv = {{V2{0, 0}, V2{1, 0}, V2{1, 1}}, {V2{0, 0}, V2{1, 1}, V2{0, 1}}};
    const int G = 16;
    for (int j = 0; j <= G; j++) for (int i = 0; i <= G; i++) { const float x = float(i) / G, y = float(j) / G, r = std::hypot(x - 0.5f, y - 0.5f); high.pos.push_back({x, y, 0.1f * std::exp(-r * r * 30)}); }
    for (int j = 0; j < G; j++) for (int i = 0; i < G; i++) { const int a = j * (G + 1) + i, b = a + 1, c = a + G + 1, d = c + 1; high.tri.push_back({a, b, d}); high.tri.push_back({a, d, c}); }
    std::vector<V3> vn(high.pos.size()); for (auto& t : high.tri) { const V3 &p = high.pos[t[0]], &q = high.pos[t[1]], &r = high.pos[t[2]]; const V3 e1{q.x - p.x, q.y - p.y, q.z - p.z}, e2{r.x - p.x, r.y - p.y, r.z - p.z};
      const V3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x}; for (int k : t) { vn[k].x += n.x; vn[k].y += n.y; vn[k].z += n.z; } }
    for (auto& v : vn) { const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); v = {v.x / l, v.y / l, v.z / l}; }
    for (auto& t : high.tri) high.nrm.push_back({vn[t[0]], vn[t[1]], vn[t[2]]});
    BakeSettings s; s.size = 64; s.cage = 0.1f; s.back = 0.1f; s.aoRays = 8; s.position = true; s.curvature = true; s.padding = 2;
    BakeResult R = bake(low, high, s); std::printf("      %s\n", R.log.toLocal8Bit().constData());
    CHECK(R.hit > 3000);
    { BakeSettings sp = s; sp.mask = QImage(8, 8, QImage::Format_Grayscale8); sp.mask.fill(0); for (int y = 0; y < 8; y++) for (int x = 0; x < 4; x++) sp.mask.scanLine(y)[x] = 255;   // (a partial bake: the left half asked for)
      const BakeResult Rp = bake(low, high, sp); std::printf("      partial: %s\n", Rp.log.toLocal8Bit().constData()); CHECK(Rp.hit > 1000 && Rp.hit < R.hit * 0.62);
      const uchar* off = Rp.normal.constScanLine(24) + 48 * 3; CHECK(off[0] == 128 && off[1] == 128 && off[2] == 255); }   // (outside the mask: nothing cast — flat)
    const uchar* flat = R.normal.constScanLine(4) + 4 * 3; CHECK(std::abs(flat[0] - 128) < 6 && std::abs(flat[1] - 128) < 6 && flat[2] > 250);
    const uchar* slope = R.normal.constScanLine(24) + 32 * 3; CHECK(std::abs(slope[1] - 128) > 5 || std::abs(slope[0] - 128) > 5);   // (the bump's flank tilts the normal)
    CHECK(reinterpret_cast<const quint16*>(R.height.constScanLine(32))[32] > 32768 + 500);
    CHECK(!R.ao.isNull() && !R.position.isNull() && !R.curvature.isNull());
    CHECK(R.colour.isNull());                                                                  // (no vertex colours asked for, none given)
    { TriMesh hc = high; for (const V3& p : hc.pos) hc.col.push_back(p.x < 0.5f ? V3{1, 0, 0} : V3{0, 0, 1});   // (an ID polypaint: the left half red, the right blue)
      BakeSettings sc = s; sc.colour = true; const BakeResult Rc = bake(low, hc, sc); CHECK(!Rc.colour.isNull());
      const uchar* L = Rc.colour.constScanLine(32) + 8 * 3; const uchar* Rr = Rc.colour.constScanLine(32) + 56 * 3;
      std::printf("      ID map: left %d,%d,%d right %d,%d,%d\n", L[0], L[1], L[2], Rr[0], Rr[1], Rr[2]);
      CHECK(L[0] > 250 && L[2] < 5 && Rr[2] > 250 && Rr[0] < 5); }
    QImage n = heightToNormal(R.height, 1); CHECK(!n.isNull());
  }
  // ---- the Paint room update (2026-11-14): the pressure curve and its minimum, the made alphas, the strand's direction, the wet mix ----
  {
    Curve lin; CHECK(std::abs(lin.at(0.5f) - 0.5f) < 1e-4f);
    Curve soft; soft.pts = {{0, 0}, {0.5, 0.2}, {1, 1}};                       // (a soft start: a light touch does little)
    float last = -1; bool mono = true; for (int i = 0; i <= 20; i++) { const float v = soft.at(i / 20.f); if (v < last - 1e-5f) mono = false; last = v; }
    CHECK(mono && soft.at(0.5f) < 0.25f && soft.at(1) > 0.99f);
    CHECK(Curve::fromJson(soft.toJson()).pts.size() == 3);
    // the size at no pressure: the brush's minimum (3D-Coat's 15 %), not a point
    auto width = [](const Brush& b, float pressure) { QImage img(256, 64, QImage::Format_ARGB32); img.fill(0); Stroke st(img, b, Qt::white); st.begin(QPointF(128, 32), pressure);
      int w = 0; for (int x = 0; x < 256; x++) if (qAlpha(img.pixel(x, 32)) > 8) w++; return w; };
    Brush b; b.size = 40; b.hardness = 1; b.tip = {}; b.flow = 1; b.pressureSize = true; b.pSizeMin = 0.15f;
    const int w0 = width(b, 0), w1 = width(b, 1); std::printf("      dab width at pressure 0: %d px, at 1: %d px\n", w0, w1);
    CHECK(w1 >= 36 && w0 >= 4 && w0 <= 9);
    // the cylinder: bright along its middle line, falling to its sides; at angle 0 it runs along x
    const QImage cyl = proceduralTip(TipKind::Cylinder, 0.6f, 0.25f, 0, 64);
    CHECK(cyl.pixelColor(32, 32).value() > 240 && cyl.pixelColor(32, 4).value() < cyl.pixelColor(32, 20).value() && cyl.pixelColor(20, 32).value() > 240);
    // along the strand: the band turned by the strand's angle — a strand going down (90°) makes a vertical band from a still dab
    { Brush c; c.size = 40; c.tipKind = TipKind::Cylinder; c.orient = 2; c.flow = 1; c.pressureSize = false;
      QImage img(64, 64, QImage::Format_ARGB32); img.fill(0); Stroke st(img, c, Qt::white); st.strandAngle = [](const QPointF&) { return 90.f; }; st.begin(QPointF(32, 32), 1);
      CHECK(qAlpha(img.pixel(32, 14)) > qAlpha(img.pixel(14, 32)) + 60); }
    // wet: a dab over red takes on red
    { Brush wb; wb.size = 30; wb.hardness = 1; wb.flow = 1; wb.wet = 1; wb.load = 1; wb.pressureSize = false;
      QImage img(64, 64, QImage::Format_ARGB32); img.fill(QColor(255, 0, 0)); Stroke st(img, wb, QColor(0, 0, 255)); st.begin(QPointF(32, 32), 1);
      const QColor c = img.pixelColor(32, 32); std::printf("      wet dab over red with blue: %d,%d,%d\n", c.red(), c.green(), c.blue()); CHECK(c.red() > 200 && c.blue() < 60); }
    // Shift's blur: softenK 0 leaves the picture as it was
    { Brush bb; bb.size = 30; bb.mode = BrushMode::Blur; bb.flow = 1; bb.pressureSize = false; QImage img(64, 64, QImage::Format_ARGB32); img.fill(Qt::black);
      for (int y = 0; y < 64; y++) for (int x = 32; x < 64; x++) img.setPixelColor(x, y, Qt::white);
      const QImage before = img; Stroke st(img, bb, Qt::white); st.softenK = 0; st.begin(QPointF(32, 32), 1); st.move(QPointF(32, 40), 1); CHECK(img == before); }
  }
  // ---- the freeze mask's operations and the effects over a region (paint/Region) ----
  {
    std::printf("-- region --\n");
    // an erase held back by half: the colour stays, half as dense — no darker texel, no single holes
    { QImage before(8, 1, QImage::Format_ARGB32); before.fill(QColor(200, 120, 40, 255)); QImage after = before; after.fill(qRgba(0, 0, 0, 0));
      uchar hold[8] = {0, 64, 128, 128, 128, 192, 255, 128}; keepTexels(after, before, 0, 0, 7, hold);
      const QRgb a = after.pixel(0, 0), h = after.pixel(2, 0), f = after.pixel(6, 0);
      std::printf("      erase under a mask of 0 / 128 / 255: alpha %d / %d / %d, colour at 128: %d,%d,%d\n", qAlpha(a), qAlpha(h), qAlpha(f), qRed(h), qGreen(h), qBlue(h));
      CHECK(qAlpha(a) == 0); CHECK(std::abs(qAlpha(h) - 128) <= 1); CHECK(qRed(h) == 200 && qGreen(h) == 120 && qBlue(h) == 40); CHECK(f == before.pixel(6, 0));
      CHECK(after.pixel(2, 0) == after.pixel(3, 0) && after.pixel(3, 0) == after.pixel(4, 0)); }   // (the same mask — the same texel: nothing random)
    // paint over an empty layer held back by half: the new colour at half density, not mixed with black
    { QImage before(4, 1, QImage::Format_ARGB32); before.fill(qRgba(0, 0, 0, 0)); QImage after = before; after.fill(QColor(10, 200, 30, 255));
      uchar hold[4] = {128, 128, 128, 128}; keepTexels(after, before, 0, 0, 3, hold); const QRgb p = after.pixel(1, 0);
      CHECK(qRed(p) == 10 && qGreen(p) == 200 && qBlue(p) == 30 && std::abs(qAlpha(p) - 127) <= 1); }
    // to 100 %: a soft blob becomes solid, its edge as wide as asked
    { QImage m(128, 128, QImage::Format_Grayscale8); m.fill(0);
      for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) { const double d = std::hypot(x - 64.0, y - 64.0); m.scanLine(y)[x] = uchar(std::clamp(int(255 - d * 6), 0, 255)); }   // (a cone: 255 in the middle, 0 from 42 px out)
      const QImage hard = maskSolid(m, 1, 0, 0), soft = maskSolid(m, 1, 0, 4), half = maskSolid(m, 128, 0, 0);
      int mid = 0; for (int x = 0; x < 128; x++) { const int v = hard.constScanLine(64)[x]; if (v != 0 && v != 255) mid++; } CHECK(mid == 0);
      CHECK(hard.constScanLine(64)[64 + 40] == 255 && hard.constScanLine(64)[64 + 44] == 0);
      CHECK(half.constScanLine(64)[64 + 20] == 255 && half.constScanLine(64)[64 + 23] == 0);
      int ramp = 0; for (int x = 64; x < 128; x++) { const int v = soft.constScanLine(64)[x]; if (v > 8 && v < 247) ramp++; } std::printf("      edge asked 4 px: %d texels between free and frozen\n", ramp); CHECK(ramp >= 2 && ramp <= 6);
      // a staircase: a hard diagonal edge smoothed keeps its place, loses its single-texel steps
      QImage st(64, 64, QImage::Format_Grayscale8); st.fill(0); for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) if (x < (y / 8) * 8 + 8) st.scanLine(y)[x] = 255;   // (steps of 8)
      const QImage sm = maskSolid(st, 1, 4, 0); int cornerBefore = st.constScanLine(8)[15], cornerAfter = sm.constScanLine(8)[15]; CHECK(cornerBefore == 255 && cornerAfter == 0);   // (a step's outer corner: rounded off)
      CHECK(sm.constScanLine(12)[4] == 255 && sm.constScanLine(12)[30] == 0); }                                                                 // (the body and the free side: where they were)
    // a closed contour filled: the inside, not the outside; an open one — everything up to the island's edge
    { QImage m(100, 100, QImage::Format_Grayscale8); m.fill(0);
      for (int i = 20; i <= 60; i++) { m.scanLine(20)[i] = 255; m.scanLine(60)[i] = 255; m.scanLine(i)[20] = 255; m.scanLine(i)[60] = 255; }   // (a square's outline)
      QRect box; const QImage in = floodFree(m, QPoint(40, 40), 128, nullptr, 0, &box);
      CHECK(!in.isNull() && in.constScanLine(40)[40] == 255 && in.constScanLine(40)[80] == 0 && in.constScanLine(21)[21] == 255 && in.constScanLine(20)[20] == 0);
      CHECK(box == QRect(QPoint(21, 21), QPoint(59, 59)));
      CHECK(floodFree(m, QPoint(20, 30), 128, nullptr).isNull());   // (a click on the line itself)
      IslandMap im; im.ids = QImage(100, 100, QImage::Format_RGB32); im.ids.fill(0); for (int y = 10; y < 90; y++) for (int x = 10; x < 70; x++) im.ids.setPixel(x, y, 1); for (int y = 10; y < 90; y++) for (int x = 72; x < 95; x++) im.ids.setPixel(x, y, 2);
      const QImage outside = floodFree(m, QPoint(65, 40), 128, &im, 0);
      CHECK(!outside.isNull() && outside.constScanLine(40)[65] == 255 && outside.constScanLine(40)[40] == 0 && outside.constScanLine(40)[80] == 0 && outside.constScanLine(5)[5] == 0);   // (stays in its island, out of the contour)
      CHECK(floodFree(m, QPoint(5, 5), 128, &im).isNull()); }       // (off the islands)
    // the effects over a region
    { QImage layer(32, 32, QImage::Format_ARGB32); layer.fill(QColor(200, 60, 60, 255)); for (int y = 0; y < 32; y++) for (int x = 16; x < 32; x++) layer.setPixelColor(x, y, QColor(60, 60, 200, 255));
      QImage cov(32, 32, QImage::Format_Grayscale8); cov.fill(0); for (int y = 8; y < 24; y++) for (int x = 8; x < 24; x++) cov.scanLine(y)[x] = 255;
      auto run = [&](RegionOp op, const QColor& ink, float k) { QImage l = layer; applyRegion(l, layer, cov, QPoint(0, 0), l.rect(), op, ink, k); return l; };
      const QImage e = run(RegionOp::Erase, Qt::white, 1); CHECK(qAlpha(e.pixel(12, 12)) == 0 && qAlpha(e.pixel(2, 2)) == 255);
      const QImage p = run(RegionOp::Paint, QColor(0, 255, 0, 255), 0.5f); CHECK(std::abs(qGreen(p.pixel(12, 12)) - 158) <= 3 && p.pixel(2, 2) == layer.pixel(2, 2));
      const QImage hu = run(RegionOp::Hue, QColor(0, 200, 0), 1); const QColor hc = hu.pixelColor(12, 12); std::printf("      red recoloured green: %d,%d,%d\n", hc.red(), hc.green(), hc.blue()); CHECK(hc.green() > hc.red() + 60 && hc.green() > hc.blue() + 60);
      CHECK(std::abs(hc.lightnessF() - layer.pixelColor(12, 12).lightnessF()) < 0.03);   // (the lightness kept)
      const QImage li = run(RegionOp::Lighten, Qt::white, 0.5f), da = run(RegionOp::Darken, Qt::white, 0.5f); CHECK(li.pixelColor(12, 12).lightnessF() > layer.pixelColor(12, 12).lightnessF() + 0.1 && da.pixelColor(12, 12).lightnessF() < layer.pixelColor(12, 12).lightnessF() - 0.1);
      const QImage de = run(RegionOp::Desaturate, Qt::white, 1); const QColor dc = de.pixelColor(12, 12); CHECK(std::abs(dc.red() - dc.green()) <= 1 && std::abs(dc.green() - dc.blue()) <= 1);
      const QImage bl = run(RegionOp::Blur, Qt::white, 1); const QColor bc = bl.pixelColor(16, 16); CHECK(bc.red() > 90 && bc.red() < 170 && bl.pixel(2, 2) == layer.pixel(2, 2));   // (the border of the two colours run together)
      const QImage sh = run(RegionOp::Sharpen, Qt::white, 1); CHECK(sh.pixelColor(15, 16).red() >= layer.pixelColor(15, 16).red() && sh.pixelColor(16, 16).red() <= layer.pixelColor(16, 16).red()); }
    // a layered PSD written and read back: the names, the blends, the opacity, the eyes, the pixels
    { const int W = 300, H = 200; std::vector<PsdLayer> ls(3);
      ls[0].name = "Sleeves"; ls[0].image = QImage(W, H, QImage::Format_ARGB32); ls[0].image.fill(0); for (int y = 20; y < 120; y++) for (int x = 30; x < 200; x++) ls[0].image.setPixelColor(x, y, QColor(x % 256, y, 90, 255));
      ls[1].name = QString::fromUtf8("Свет Over_1"); ls[1].blend = Blend::Overlay; ls[1].opacity = 0.5f; ls[1].image = QImage(W, H, QImage::Format_ARGB32); ls[1].image.fill(QColor(128, 128, 128, 255));
      ls[2].name = "Empty"; ls[2].visible = false; ls[2].blend = Blend::Color; ls[2].image = QImage(W, H, QImage::Format_ARGB32); ls[2].image.fill(0);
      const QString p = QDir::temp().filePath("hm_painttest_layers.psd"); QString err; QImage merged(W, H, QImage::Format_ARGB32); merged.fill(QColor(10, 20, 30, 255));
      CHECK(writePsd(p, W, H, ls, merged, &err)); if (!err.isEmpty()) std::printf("      %s\n", err.toUtf8().constData());
      int rw = 0, rh = 0; std::vector<PsdLayer> back; CHECK(readPsd(p, &rw, &rh, &back, &err)); if (!err.isEmpty()) std::printf("      %s\n", err.toUtf8().constData());
      std::printf("      psd: %lld bytes, %dx%d, %d layers\n", (long long)QFileInfo(p).size(), rw, rh, int(back.size()));
      CHECK(rw == W && rh == H && back.size() == 3);
      if (back.size() == 3) { CHECK(back[0].name == "Sleeves" && back[1].name == ls[1].name && back[2].name == "Empty");
        CHECK(back[1].blend == Blend::Overlay && back[2].blend == Blend::Color && back[0].blend == Blend::Normal);
        CHECK(std::abs(back[1].opacity - 0.5f) < 0.01f && back[0].visible && !back[2].visible);
        CHECK(back[0].image == ls[0].image); CHECK(back[1].image == ls[1].image); CHECK(back[2].image == ls[2].image); }
      std::printf("      psd file: %s\n", p.toUtf8().constData()); }
    // the tiles kept before a change
    { QImage img(600, 300, QImage::Format_ARGB32); img.fill(QColor(1, 2, 3)); TileSnap ts; ts.start(img); CHECK(ts.empty());
      ts.reach(img, QRect(250, 10, 20, 20)); img.fill(QColor(9, 9, 9)); ts.reach(img, QRect(250, 10, 20, 20));   // (the second reach: nothing copied again)
      const auto tl = ts.tiles(); CHECK(tl.size() == 2 && tl[0].img.pixelColor(0, 0) == QColor(1, 2, 3) && ts.image().pixelColor(255, 15) == QColor(1, 2, 3)); }
    // Shift's blur along the strand: across a sharp up-and-down border, a strand running up and down keeps it sharp, one running
    // across runs it together (the author's "a smooth passage down the lock", 16.11)
    { auto blurAcross = [](float strandDeg) { QImage img(64, 64, QImage::Format_ARGB32); for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) img.setPixelColor(x, y, x < 32 ? QColor(20, 20, 20) : QColor(230, 230, 230));
        Brush b; b.mode = BrushMode::Blur; b.size = 24; b.hardness = 1; b.opacity = 1; b.flow = 1; Stroke s(img, b); s.blurAlong = true; s.strandAngle = [strandDeg](const QPointF&) { return strandDeg; };
        s.begin(QPointF(32, 10)); s.move(QPointF(32, 54)); s.end(); return img; };
      const QImage up = blurAcross(90), across = blurAcross(0);
      std::printf("      blur along: up-and-down strand %d|%d, across %d|%d\n", qRed(up.pixel(29, 32)), qRed(up.pixel(34, 32)), qRed(across.pixel(29, 32)), qRed(across.pixel(34, 32)));
      CHECK(qRed(up.pixel(29, 32)) < 40 && qRed(up.pixel(34, 32)) > 210);                          // (the border kept)
      CHECK(qRed(across.pixel(29, 32)) > 60 && qRed(across.pixel(34, 32)) < 190); }                  // (run together)
    // Shift as oil: from the dark half across into the light one, the dark is carried well into the light — further than the blur's
    // reach — and fades as it mixes
    { QImage img(96, 32, QImage::Format_ARGB32); for (int y = 0; y < 32; y++) for (int x = 0; x < 96; x++) img.setPixelColor(x, y, x < 24 ? QColor(20, 20, 20) : QColor(230, 230, 230));
      Brush b; b.mode = BrushMode::Smudge; b.size = 12; b.hardness = 1; b.opacity = 1; b.flow = 1; Stroke s(img, b); s.oilCarry = true; s.softenK = 0.8f;
      s.begin(QPointF(10, 16)); s.move(QPointF(80, 16)); s.end();
      std::printf("      oil: 30 → %d, 50 → %d, 75 → %d\n", qRed(img.pixel(30, 16)), qRed(img.pixel(50, 16)), qRed(img.pixel(75, 16)));
      CHECK(qRed(img.pixel(30, 16)) < 120 && qRed(img.pixel(50, 16)) < 200 && qRed(img.pixel(50, 16)) > qRed(img.pixel(30, 16)) && qRed(img.pixel(30, 2)) == 230); }
    // the live layers: a gradient between two points and colour spheres over a known surface; made an ordinary layer — the same look
    { Document d(32, 32); std::vector<float> P(32 * 32 * 3), N(32 * 32 * 3, 0.f);
      for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { const size_t q = (size_t(y) * 32 + x) * 3; P[q] = float(x); P[q + 1] = float(31 - y); P[q + 2] = 0; if (x < 30) N[q + 2] = 1; }   // (a plane facing z; the two right columns: no surface)
      d.positionsOf = [&](const QString&, int w, int h, const float*& p, const float*& n) { if (w != 32 || h != 32) return false; p = P.data(); n = N.data(); return true; };
      int rev = 1; d.geomRevOf = [&](const QString&) { return qint64(rev); };
      const int g = d.addFill(Channel::Color, Qt::white); Layer* G = d.layer(g); G->gradLive = true; G->g0[1] = 31; G->g1[1] = 0; G->grad = {{0.f, QColor(255, 0, 0)}, {1.f, QColor(0, 0, 255)}};
      QImage c = d.compose(Channel::Color); CHECK(qRed(c.pixel(5, 0)) > 240 && qBlue(c.pixel(5, 31)) > 240 && qAlpha(c.pixel(31, 5)) == 0);   // (red at the top, blue at the bottom, nothing where no surface lies)
      G->grad = {{0.f, QColor(0, 255, 0)}, {1.f, QColor(0, 255, 0)}}; c = d.compose(Channel::Color); CHECK(qGreen(c.pixel(5, 16)) > 240);       // (the colours changed: shown at once)
      Document e; CHECK(e.fromJson(d.toJson()) && e.layers().back().gradLive && std::abs(e.layers().back().g0[1] - 31) < 1e-4f);
      const int sp = d.addFill(Channel::Color, Qt::white); Layer* S = d.layer(sp); S->gradLive = true; S->liveKind = "spheres";
      Layer::Sphere a; a.c[0] = 8; a.c[1] = 23; a.r = 6; a.hard = 0.5f; a.col = QColor(250, 250, 0); S->spheres.push_back(a);
      c = d.compose(Channel::Color); const QRgb at = c.pixel(8, 31 - 23), off = c.pixel(25, 31 - 5);
      std::printf("      sphere: centre %d,%d,%d · far %d,%d,%d\n", qRed(at), qGreen(at), qBlue(at), qRed(off), qGreen(off), qBlue(off));
      CHECK(qRed(at) > 240 && qGreen(at) > 240 && qBlue(at) < 20 && qRed(off) < 20 && qGreen(off) > 240);   // (yellow in the sphere, the gradient's green beyond it)
      Document f; CHECK(f.fromJson(d.toJson()) && f.layers().back().spheres.size() == 1 && f.layers().back().liveKind == "spheres");
      { Document k(32, 32); k.positionsOf = d.positionsOf; k.geomRevOf = d.geomRevOf;   // (a curve gradient: along an L-shaped curve drawn on the plane — down the left side, then along the bottom; the mask keeps it to the strand)
        const int cg = k.addFill(Channel::Color, Qt::white); Layer* C = k.layer(cg); C->gradLive = true; C->liveKind = "curve"; C->curve = {{4, 28, 0}, {4, 4, 0}, {28, 4, 0}};
        C->grad = {{0.f, QColor(0, 0, 0)}, {1.f, QColor(255, 255, 255)}};
        C->mask = QImage(32, 32, QImage::Format_Grayscale8); C->mask.fill(0); for (int y = 0; y < 32; y++) for (int x = 0; x < 8; x++) C->mask.scanLine(y)[x] = 255; for (int y = 24; y < 32; y++) for (int x = 0; x < 30; x++) C->mask.scanLine(y)[x] = 255;
        QImage kc = k.compose(Channel::Color); const int top = qRed(kc.pixel(4, 31 - 27)), corner = qRed(kc.pixel(4, 31 - 4)), end = qRed(kc.pixel(27, 31 - 4)), outside = qAlpha(kc.pixel(20, 31 - 20));
        std::printf("      curve gradient: start %d · corner %d · end %d · off the strand alpha %d\n", top, corner, end, outside);
        CHECK(top < 20 && std::abs(corner - 128) < 25 && end > 230 && outside == 0);   // (black at the curve's start, half way at the bend, white at its end — nothing off the mask)
        Document kj; CHECK(kj.fromJson(k.toJson()) && kj.layers().back().liveKind == "curve" && kj.layers().back().curve.size() == 3); }
      CHECK(d.rasterize(sp) && d.layer(sp)->kind == LayerKind::Paint && !d.layer(sp)->gradLive); const QImage c2 = d.compose(Channel::Color);
      CHECK(c2.pixel(8, 31 - 23) == at && c2.pixel(25, 31 - 5) == off); }                            // (an ordinary layer now: the look kept)
  }
  // the Stamp: a copy from an offset (a red square carried 40 texels right) and a mirrored copy (a transform that flips x); the patch:
  // a spot over two colours filled from round it — each half from its own side — and nothing taken from another island
  { QImage img(96, 64, QImage::Format_ARGB32); img.fill(qRgba(40, 40, 200, 255)); for (int y = 20; y < 40; y++) for (int x = 10; x < 30; x++) img.setPixel(x, y, qRgba(230, 20, 20, 255));
    Brush b; b.size = 16; b.hardness = 1; b.flow = 1; b.opacity = 1; b.spacing = 0.1f; b.pressureSize = false; b.mode = BrushMode::Clone;
    { QImage t = img; Stroke s(t, b); s.cloneSrc = img; s.cloneAt = [](const QPointF&) { return std::optional<QTransform>(QTransform::fromTranslate(-40, 0)); };
      s.begin(QPointF(60, 30)); s.move(QPointF(66, 30)); const QRgb at = t.pixel(62, 30), off = t.pixel(80, 30);
      std::printf("      stamp from an anchor: under the brush %d,%d,%d · beside it %d,%d,%d\n", qRed(at), qGreen(at), qBlue(at), qRed(off), qGreen(off), qBlue(off));
      CHECK(qRed(at) > 220 && qBlue(at) < 40 && qBlue(off) > 190); }
    { QImage t = img; Stroke s(t, b); s.cloneSrc = img; s.cloneAt = [](const QPointF&) { return std::optional<QTransform>(QTransform(-1, 0, 0, 1, 96, 0)); };   // (x → 96 − x: the left square seen at the right)
      s.begin(QPointF(76, 30)); const QRgb at = t.pixel(76, 30); CHECK(qRed(at) > 220 && qBlue(at) < 40); }
    QImage two(96, 64, QImage::Format_ARGB32); for (int y = 0; y < 64; y++) for (int x = 0; x < 96; x++) two.setPixel(x, y, y < 32 ? qRgba(250, 250, 250, 255) : qRgba(20, 20, 20, 255));
    for (int y = 26; y < 38; y++) for (int x = 40; x < 56; x++) two.setPixel(x, y, qRgba(255, 0, 255, 255));   // (a magenta blot across the border)
    QImage cov(96, 64, QImage::Format_Grayscale8); cov.fill(0); for (int y = 24; y < 40; y++) for (int x = 38; x < 58; x++) cov.scanLine(y)[x] = 255;
    QImage t = two; CHECK(healRegion(t, two, cov, QRect(38, 24, 20, 16), 16));
    const QRgb up = t.pixel(47, 26), dn = t.pixel(47, 37);
    std::printf("      patch: above the border %d,%d,%d · below it %d,%d,%d\n", qRed(up), qGreen(up), qBlue(up), qRed(dn), qGreen(dn), qBlue(dn));
    CHECK(qRed(up) > 150 && qGreen(up) > 150 && qRed(dn) < 110 && std::abs(qRed(up) - qGreen(up)) < 20 && std::abs(qRed(dn) - qGreen(dn)) < 20);   // (no magenta left: light above, dark below)
    IslandMap im; im.ids = QImage(96, 64, QImage::Format_RGB32); for (int y = 0; y < 64; y++) for (int x = 0; x < 96; x++) reinterpret_cast<quint32*>(im.ids.scanLine(y))[x] = x < 60 ? 1u : 2u;
    QImage red = two; for (int y = 0; y < 64; y++) for (int x = 60; x < 96; x++) red.setPixel(x, y, qRgba(255, 0, 0, 255));   // (the next island red: it must lend nothing)
    QImage t2 = red; CHECK(healRegion(t2, red, cov, QRect(38, 24, 20, 16), 24, &im)); const QRgb e = t2.pixel(57, 30);
    std::printf("      patch beside another island: %d,%d,%d\n", qRed(e), qGreen(e), qBlue(e)); CHECK(std::abs(qRed(e) - qGreen(e)) < 30); }
  // the object shadow (19.11, recording Cloth_2: the hair's shadow on the shoulders): a floor 64×64 facing up, a square card 1 above its
  // middle; the light straight down — the shadow under the card, light round it; softer light — a wider, softer edge; a texel turned
  // away from the light is left alone (its own shade, not a cast shadow)
  { std::printf("  object shadow\n"); const int W = 64; std::vector<float> P(size_t(W) * W * 3), N(size_t(W) * W * 3, 0.f);
    for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) { const size_t i = (size_t(y) * W + x) * 3; P[i] = x * 0.1f; P[i + 2] = y * 0.1f; N[i + 1] = 1; }
    for (int x = 0; x < 4; x++) N[(size_t(32) * W + x) * 3 + 1] = -1;    // (a row-start turned down)
    TriMesh card; card.pos = {{2.2f, 1, 2.2f}, {4.2f, 1, 2.2f}, {4.2f, 1, 4.2f}, {2.2f, 1, 4.2f}}; card.tri = {{0, 1, 2}, {0, 2, 3}};
    const auto F = shadowFrame(P.data(), N.data(), W, W); float L[3]; fxLight(0, 90, L);
    QElapsedTimer tm; tm.start(); const QImage hard = castShadow(*F, card, L, 0, 1); const qint64 ms = tm.elapsed();
    const QImage soft = castShadow(*F, card, L, 12, 64);
    auto at = [](const QImage& im, int x, int y) { return int(im.constScanLine(y)[x]); };
    std::printf("      hard: middle %d, outside %d, edge %d · soft: middle %d, just outside %d, edge %d (%lld ms)\n", at(hard, 32, 32), at(hard, 10, 10), at(hard, 22, 32), at(soft, 32, 32), at(soft, 21, 32), at(soft, 22, 32), ms);
    CHECK(at(hard, 32, 32) == 255 && at(hard, 10, 10) == 0 && at(hard, 1, 32) == 0);
    CHECK(at(soft, 32, 32) > 240 && at(soft, 21, 32) > 0 && at(soft, 21, 32) < at(soft, 22, 32) && at(soft, 22, 32) < 200 && at(hard, 21, 32) == 0 && at(soft, 10, 10) == 0);   // (0.1 outside the card: partly lit only with the soft light)
    // (a big map: worked out on the coarser grid and spread back — the same picture)
    const int B = 2048; std::vector<float> P2(size_t(B) * B * 3), N2(size_t(B) * B * 3, 0.f);
    for (int y = 0; y < B; y++) for (int x = 0; x < B; x++) { const size_t i = (size_t(y) * B + x) * 3; P2[i] = x * (6.4f / B); P2[i + 2] = y * (6.4f / B); N2[i + 1] = 1; }
    tm.restart(); const auto F2 = shadowFrame(P2.data(), N2.data(), B, B); const QImage big = castShadow(*F2, card, L, 3, 16);
    std::printf("      2048²: step %d, %lld ms · middle %d, outside %d\n", F2->step, tm.elapsed(), at(big, B / 2, B / 2), at(big, B / 8, B / 8));
    CHECK(F2->step == 2 && at(big, B / 2, B / 2) > 240 && at(big, B / 8, B / 8) == 0); }
  std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails ? 1 : 0;
}
