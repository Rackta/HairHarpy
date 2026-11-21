#include "paint/Psd.h"
#include <QFile>
#include <QSaveFile>
#include <algorithm>
#include <cstring>

namespace hm::paint {

namespace {
struct ModeKey { Blend b; const char* key; };
const ModeKey kModes[] = {{Blend::Normal, "norm"}, {Blend::Darken, "dark"}, {Blend::Multiply, "mul "}, {Blend::ColorBurn, "idiv"}, {Blend::LinearBurn, "lbrn"}, {Blend::Lighten, "lite"},
  {Blend::Screen, "scrn"}, {Blend::ColorDodge, "div "}, {Blend::Add, "lddg"}, {Blend::Overlay, "over"}, {Blend::SoftLight, "sLit"}, {Blend::HardLight, "hLit"}, {Blend::VividLight, "vLit"},
  {Blend::LinearLight, "lLit"}, {Blend::PinLight, "pLit"}, {Blend::Difference, "diff"}, {Blend::Exclusion, "smud"}, {Blend::Subtract, "fsub"}, {Blend::Divide, "fdiv"},
  {Blend::Hue, "hue "}, {Blend::Saturation, "sat "}, {Blend::Color, "colr"}, {Blend::Luminosity, "lum "}};
const char* keyOf(Blend b) { for (const ModeKey& m : kModes) if (m.b == b) return m.key; return "norm"; }
Blend blendOf(const char k[4]) { for (const ModeKey& m : kModes) if (!std::memcmp(m.key, k, 4)) return m.b; return Blend::Normal; }

// ---- writing ----
struct Out {
  QByteArray b;
  void u8(int v) { b.append(char(v & 255)); }
  void u16(int v) { u8(v >> 8); u8(v); }
  void u32(quint32 v) { u8(int(v >> 24)); u8(int(v >> 16)); u8(int(v >> 8)); u8(int(v)); }
  void raw(const char* p, int n) { b.append(p, n); }
  void set32(int at, quint32 v) { b[at] = char(v >> 24); b[at + 1] = char(v >> 16); b[at + 2] = char(v >> 8); b[at + 3] = char(v); }
};
// one row packed (PackBits): runs of the same byte, stretches of different ones
void packRow(const uchar* s, int n, QByteArray& o) {
  int i = 0;
  while (i < n) {
    int run = 1; while (i + run < n && run < 128 && s[i + run] == s[i]) run++;
    if (run >= 3) { o.append(char(257 - run)); o.append(char(s[i])); i += run; continue; }
    int j = i, lit = 0;
    while (j < n && lit < 128) { int r = 1; while (j + r < n && r < 3 && s[j + r] == s[j]) r++; if (r >= 3) break; j++; lit++; }
    o.append(char(lit - 1)); o.append(reinterpret_cast<const char*>(s + i), lit); i += lit;
  }
}
// a channel of a picture (ARGB32: 0 red, 1 green, 2 blue, 3 alpha) inside a rectangle, packed row by row: the rows' lengths, then the rows
QByteArray packChannel(const QImage& im, const QRect& r, int ch) {
  QByteArray counts, data; std::vector<uchar> row(size_t(std::max(1, r.width())));
  for (int y = r.top(); y <= r.bottom(); y++) { const QRgb* p = reinterpret_cast<const QRgb*>(im.constScanLine(y)) + r.left();
    for (int x = 0; x < r.width(); x++) row[size_t(x)] = uchar(ch == 0 ? qRed(p[x]) : ch == 1 ? qGreen(p[x]) : ch == 2 ? qBlue(p[x]) : qAlpha(p[x]));
    const int before = data.size(); packRow(row.data(), r.width(), data); const int n = data.size() - before; counts.append(char(n >> 8)); counts.append(char(n)); }
  return counts + data;
}
QRect paintBox(const QImage& im) {
  int x0 = im.width(), y0 = im.height(), x1 = -1, y1 = -1;
  for (int y = 0; y < im.height(); y++) { const QRgb* p = reinterpret_cast<const QRgb*>(im.constScanLine(y)); int l = -1, r = -1;
    for (int x = 0; x < im.width(); x++) if (qAlpha(p[x])) { if (l < 0) l = x; r = x; }
    if (l >= 0) { x0 = std::min(x0, l); x1 = std::max(x1, r); y0 = std::min(y0, y); y1 = std::max(y1, y); } }
  return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}
}  // namespace

bool writePsd(const QString& path, int w, int h, const std::vector<PsdLayer>& layers, const QImage& mergedIn, QString* error, const std::vector<PsdAlpha>& alphas) {
  auto fail = [&](const QString& e) { if (error) *error = e; return false; };
  if (w <= 0 || h <= 0 || w > 30000 || h > 30000) return fail(QStringLiteral("A PSD holds up to 30000 × 30000 pixels"));
  Out o; o.raw("8BPS", 4); o.u16(1); for (int i = 0; i < 6; i++) o.u8(0); o.u16(3 + int(alphas.size())); o.u32(quint32(h)); o.u32(quint32(w)); o.u16(8); o.u16(3);
  o.u32(0);                                                      // (no colour table)
  // image resources: the alpha channels' names (1045, Unicode, each counted with its closing zero). The merged picture is written as
  // R, G, B and these channels only: a fourth "transparency" plane would show in Photoshop as a stray alpha channel
  if (alphas.empty()) o.u32(0);
  else { const int irAt = o.b.size(); o.u32(0);
    o.raw("8BIM", 4); o.u16(1045); o.u16(0); const int lenAt = o.b.size(); o.u32(0);
    for (const PsdAlpha& a : alphas) { o.u32(quint32(a.name.size() + 1)); for (const QChar c : a.name) o.u16(c.unicode()); o.u16(0); }
    const int n = o.b.size() - lenAt - 4; o.set32(lenAt, quint32(n)); if (n % 2) o.u8(0);
    o.set32(irAt, quint32(o.b.size() - irAt - 4)); }
  const int lmAt = o.b.size(); o.u32(0);                         // (the layers' and masks' section: its length, filled in below)
  const int liAt = o.b.size(); o.u32(0);                         // (the layers' own part)
  o.u16(int(layers.size()));
  struct Ch { QByteArray d[4]; QRect r; }; std::vector<Ch> chans(layers.size());
  for (size_t i = 0; i < layers.size(); i++) {
    const PsdLayer& l = layers[i]; const QImage im = l.image.isNull() ? QImage() : l.image.convertToFormat(QImage::Format_ARGB32);
    Ch& c = chans[i]; c.r = im.isNull() ? QRect() : paintBox(im);
    if (!c.r.isEmpty()) for (int k = 0; k < 4; k++) { QByteArray d; d.append(char(0)); d.append(char(1)); d += packChannel(im, c.r, k); c.d[k] = d; }   // (compression 1: packed)
    else for (int k = 0; k < 4; k++) { c.d[k] = QByteArray(2, 0); }                                                                                    // (an empty layer: nothing after the compression's word)
    o.u32(quint32(c.r.isEmpty() ? 0 : c.r.top())); o.u32(quint32(c.r.isEmpty() ? 0 : c.r.left())); o.u32(quint32(c.r.isEmpty() ? 0 : c.r.bottom() + 1)); o.u32(quint32(c.r.isEmpty() ? 0 : c.r.right() + 1));
    o.u16(4);
    o.u16(0xFFFF); o.u32(quint32(c.d[3].size()));               // (−1: the transparency)
    for (int k = 0; k < 3; k++) { o.u16(k); o.u32(quint32(c.d[k].size())); }
    o.raw("8BIM", 4); o.raw(keyOf(l.blend), 4); o.u8(std::clamp(int(l.opacity * 255 + 0.5f), 0, 255)); o.u8(0); o.u8(l.visible ? 0 : 2); o.u8(0);
    const int exAt = o.b.size(); o.u32(0);
    o.u32(0); o.u32(0);                                          // (no layer mask, no blending ranges)
    QByteArray nm = l.name.toLatin1().left(255); for (char& ch : nm) if (uchar(ch) < 32) ch = '_';
    o.u8(nm.size()); o.raw(nm.constData(), nm.size()); while ((nm.size() + 1) % 4) { o.u8(0); nm.append('\0'); }   // (the name, padded to four bytes)
    { o.raw("8BIM", 4); o.raw("luni", 4); const int n = l.name.size(); const int len = 4 + n * 2 + ((n * 2) % 4 ? 2 : 0); o.u32(quint32(len)); o.u32(quint32(n));   // (the name in full: Unicode)
      for (const QChar ch : l.name) o.u16(ch.unicode()); if ((n * 2) % 4) o.u16(0); }
    o.set32(exAt, quint32(o.b.size() - exAt - 4));
  }
  for (const Ch& c : chans) { o.b += c.d[3]; for (int k = 0; k < 3; k++) o.b += c.d[k]; }   // (the channels in the order the records name them)
  if ((o.b.size() - liAt - 4) % 2) o.u8(0);
  o.set32(liAt, quint32(o.b.size() - liAt - 4));
  o.u32(0);                                                      // (no global layer mask)
  o.set32(lmAt, quint32(o.b.size() - lmAt - 4));
  // the merged picture: all four channels packed, every row's length first
  QImage merged = mergedIn.isNull() ? QImage(w, h, QImage::Format_ARGB32) : mergedIn.convertToFormat(QImage::Format_ARGB32); if (mergedIn.isNull()) merged.fill(0);
  if (merged.size() != QSize(w, h)) merged = merged.scaled(w, h);
  o.u16(1); QByteArray counts, data; const QRect all(0, 0, w, h);
  for (int k = 0; k < 3; k++) { const QByteArray p = packChannel(merged, all, k); counts += p.left(h * 2); data += p.mid(h * 2); }
  for (const PsdAlpha& a : alphas) {                             // (each alpha channel: its gray as a channel of the merged data)
    QImage g = a.gray.isNull() ? QImage(w, h, QImage::Format_Grayscale8) : a.gray.convertToFormat(QImage::Format_Grayscale8); if (a.gray.isNull()) g.fill(0);
    if (g.size() != QSize(w, h)) g = g.scaled(w, h);
    QImage carrier(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; y++) { const uchar* s = g.constScanLine(y); QRgb* d = reinterpret_cast<QRgb*>(carrier.scanLine(y)); for (int x = 0; x < w; x++) d[x] = qRgba(s[x], 0, 0, 255); }
    const QByteArray p = packChannel(carrier, all, 0); counts += p.left(h * 2); data += p.mid(h * 2); }
  o.b += counts; o.b += data;
  QSaveFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(o.b) != o.b.size() || !f.commit()) return fail(QStringLiteral("The file can't be written: %1").arg(path));
  return true;
}

// ---- reading ----
namespace {
struct In {
  const QByteArray& b; qint64 at = 0; bool bad = false;
  explicit In(const QByteArray& x) : b(x) {}
  bool need(qint64 n) { if (at + n > b.size() || n < 0) { bad = true; return false; } return true; }
  int u8() { if (!need(1)) return 0; return uchar(b[at++]); }
  int u16() { const int a = u8(), c = u8(); return (a << 8) | c; }
  int s16() { const int v = u16(); return v >= 0x8000 ? v - 0x10000 : v; }
  quint32 u32() { const quint32 a = quint32(u16()); return (a << 16) | quint32(u16()); }
  qint32 s32() { return qint32(u32()); }
  void skip(qint64 n) { if (need(n)) at += n; }
};
// a channel's plane read into out (w × h bytes): raw, or packed row by row
bool readPlane(In& in, int w, int h, std::vector<uchar>& out) {
  if (w > 30000 || h > 30000) return false;                    // (a damaged record: no gigabytes asked for by its numbers)
  out.assign(size_t(std::max(0, w)) * size_t(std::max(0, h)), 0); const int comp = in.u16();
  if (w <= 0 || h <= 0) return !in.bad;
  if (comp == 0) { if (!in.need(qint64(w) * h)) return false; std::memcpy(out.data(), in.b.constData() + in.at, size_t(w) * size_t(h)); in.at += qint64(w) * h; return true; }
  if (comp != 1) return false;                                  // (ZIP: not written into 8-bit layers by Photoshop)
  std::vector<int> len(static_cast<size_t>(h)); for (int y = 0; y < h; y++) len[size_t(y)] = in.u16();
  for (int y = 0; y < h; y++) { if (!in.need(len[size_t(y)])) return false; const uchar* s = reinterpret_cast<const uchar*>(in.b.constData()) + in.at; const int n = len[size_t(y)]; uchar* d = out.data() + size_t(y) * size_t(w); int x = 0, i = 0;
    while (i < n && x < w) { const int c = qint8(s[i++]); if (c >= 0) { const int k = std::min(c + 1, std::min(w - x, n - i)); std::memcpy(d + x, s + i, size_t(k)); x += k; i += c + 1; }
      else if (c != -128) { const int k = std::min(1 - c, w - x); if (i < n) std::memset(d + x, s[i], size_t(k)); x += k; i++; } }
    in.at += n; }
  return !in.bad;
}
}  // namespace

bool readPsd(const QString& path, int* wOut, int* hOut, std::vector<PsdLayer>* layers, QString* error) {
  auto fail = [&](const QString& e) { if (error) *error = e; return false; };
  QFile f(path); if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("The file can't be read: %1").arg(path));
  const QByteArray data = f.readAll(); In in(data);
  if (data.size() < 26 || std::memcmp(data.constData(), "8BPS", 4)) return fail(QStringLiteral("Not a Photoshop file"));
  in.skip(4); const int ver = in.u16(); in.skip(6); in.u16(); const int h = int(in.u32()), w = int(in.u32()), depth = in.u16(), mode = in.u16();
  if (ver != 1) return fail(QStringLiteral("A large document (PSB) is not read — save it as PSD"));
  if (w <= 0 || h <= 0 || w > 30000 || h > 30000) return fail(QStringLiteral("The file's size is not read: %1 × %2").arg(w).arg(h));
  if (depth != 8 || mode != 3) return fail(QStringLiteral("Only 8 bits a channel, RGB is read — the file is %1 bits, colour mode %2 (Image → Mode → RGB Color, 8 Bits/Channel)").arg(depth).arg(mode));
  in.skip(in.u32()); in.skip(in.u32());                          // (the colour table, the image resources)
  const quint32 lmLen = in.u32(); const qint64 lmEnd = in.at + lmLen;
  if (wOut) *wOut = w; if (hOut) *hOut = h; if (layers) layers->clear();
  if (lmLen < 8 || in.bad) return !in.bad || fail(QStringLiteral("The file is cut short"));   // (no layers: a flat picture)
  in.u32(); const int n = std::abs(in.s16());
  struct Rec { QRect r; std::vector<std::pair<int, quint32>> ch; PsdLayer l; }; std::vector<Rec> recs(static_cast<size_t>(n));
  for (Rec& rc : recs) {
    const int top = in.s32(), left = in.s32(), bottom = in.s32(), right = in.s32(); rc.r = QRect(left, top, right - left, bottom - top);
    const int nc = in.u16(); for (int i = 0; i < nc; i++) { const int id = in.s16(); const quint32 len = in.u32(); rc.ch.push_back({id, len}); }
    in.skip(4); char key[4] = {0, 0, 0, 0}; if (in.need(4)) { std::memcpy(key, data.constData() + in.at, 4); in.at += 4; }
    rc.l.blend = blendOf(key); rc.l.opacity = in.u8() / 255.f; in.u8(); const int flags = in.u8(); in.u8(); rc.l.visible = !(flags & 2);
    const quint32 exLen = in.u32(); const qint64 exEnd = in.at + exLen;
    in.skip(in.u32()); in.skip(in.u32());                        // (the layer's mask record, the blending ranges)
    { const int nl = in.u8(); if (in.need(nl)) { rc.l.name = QString::fromLatin1(data.constData() + in.at, nl); in.at += nl; } in.skip((4 - (nl + 1) % 4) % 4); }
    while (!in.bad && in.at + 12 <= exEnd) {                    // (the extra blocks: the name in Unicode; a group's marker; an adjustment's settings)
      if (!in.need(12)) break; in.skip(4); char k[5] = {0, 0, 0, 0, 0}; std::memcpy(k, data.constData() + in.at, 4); in.at += 4; const quint32 len = in.u32(); const qint64 end = in.at + len;
      if (!std::memcmp(k, "luni", 4)) { const quint32 cn = in.u32(); QString s; for (quint32 i = 0; i < cn && in.at + 2 <= end; i++) s += QChar(in.u16()); if (!s.isEmpty()) rc.l.name = s; }
      else if (!std::memcmp(k, "lsct", 4) || !std::memcmp(k, "lsdk", 4)) rc.l.pixels = false;                                                                  // (a group opened or closed)
      else if (!std::memcmp(k, "grdm", 4) || !std::memcmp(k, "hue2", 4) || !std::memcmp(k, "levl", 4) || !std::memcmp(k, "curv", 4) || !std::memcmp(k, "brit", 4)
               || !std::memcmp(k, "SoCo", 4) || !std::memcmp(k, "blnc", 4) || !std::memcmp(k, "vibA", 4) || !std::memcmp(k, "nvrt", 4) || !std::memcmp(k, "post", 4) || !std::memcmp(k, "thrs", 4)) rc.l.pixels = false;   // (an adjustment or a fill of Photoshop's own)
      in.at = std::min<qint64>(end + (len % 2), exEnd); }
    in.at = exEnd;
  }
  if (in.bad) return fail(QStringLiteral("The file's layers can't be read"));
  for (Rec& rc : recs) {
    const int lw = rc.r.width(), lh = rc.r.height(); std::vector<uchar> plane[4]; bool have[4] = {false, false, false, false};
    for (const auto& [id, len] : rc.ch) { const qint64 end = in.at + len; const int slot = id == -1 ? 3 : (id >= 0 && id <= 2) ? id : -1;
      if (slot >= 0 && len >= 2) { if (!readPlane(in, lw, lh, plane[slot])) return fail(QStringLiteral("A layer of the file is packed in a way that is not read (save it with Maximize Compatibility, 8 bits)")); have[slot] = true; }
      in.at = end; if (in.at > data.size()) return fail(QStringLiteral("The file is cut short")); }
    if (!layers) continue;
    PsdLayer l = rc.l; l.image = QImage(w, h, QImage::Format_ARGB32); if (l.image.isNull()) return fail(QStringLiteral("Not enough memory for the file's layers (%1 × %2 each)").arg(w).arg(h)); l.image.fill(0);
    if (l.pixels && lw > 0 && lh > 0 && have[0] && have[1] && have[2]) {
      const QRect on = rc.r.intersected(QRect(0, 0, w, h));
      for (int y = on.top(); y <= on.bottom(); y++) { QRgb* d = reinterpret_cast<QRgb*>(l.image.scanLine(y)); const size_t row = size_t(y - rc.r.top()) * size_t(lw);
        for (int x = on.left(); x <= on.right(); x++) { const size_t i = row + size_t(x - rc.r.left()); d[x] = qRgba(plane[0][i], plane[1][i], plane[2][i], have[3] ? plane[3][i] : 255); } } }
    layers->push_back(std::move(l));
  }
  (void)lmEnd;
  return true;
}

}  // namespace hm::paint
