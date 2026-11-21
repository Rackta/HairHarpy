#include "paint/Abr.h"
#include <algorithm>
#include <cstring>

namespace hm::paint {

namespace {
struct Rd {
  const uchar* d; qint64 n; qint64 p = 0; bool bad = false;
  Rd(const QByteArray& b) : d(reinterpret_cast<const uchar*>(b.constData())), n(b.size()) {}
  bool has(qint64 k) const { return p >= 0 && p + k <= n; }
  int u8() { if (!has(1)) { bad = true; return 0; } return d[p++]; }
  int i16() { if (!has(2)) { bad = true; return 0; } const int v = qint16((d[p] << 8) | d[p + 1]); p += 2; return v; }
  int u16() { return i16() & 0xffff; }
  qint32 i32() { if (!has(4)) { bad = true; return 0; } const qint32 v = qint32((quint32(d[p]) << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3]); p += 4; return v; }
  void skip(qint64 k) { p += k; if (p > n || p < 0) { bad = true; p = std::clamp<qint64>(p, 0, n); } }
};

// a Photoshop-style PackBits row of `want` bytes from the reader (limited to len bytes)
bool unpackRow(Rd& r, int len, uchar* out, int want) {
  const qint64 end = r.p + len; int o = 0;
  while (r.p < end && o < want && !r.bad) {
    const int n = qint8(r.u8());
    if (n >= 0) { for (int i = 0; i <= n && o < want; i++) out[o++] = uchar(r.u8()); }
    else if (n != -128) { const uchar v = uchar(r.u8()); for (int i = 0; i < 1 - n && o < want; i++) out[o++] = v; }
  }
  r.p = end; return o == want;
}

// the image part: bounds, depth, compression, the data. Returns a null image when it doesn't hold together
QImage readSample(Rd& r, qint64 limit, int t, int l, int b, int rt, int depth, int comp) {
  const int w = rt - l, h = b - t, bytes = depth / 8;
  if (w < 1 || h < 1 || w > 8192 || h > 8192 || (depth != 8 && depth != 16) || (comp != 0 && comp != 1)) return {};
  QImage img(w, h, QImage::Format_Grayscale8);
  std::vector<uchar> row(size_t(w) * bytes);
  if (comp == 0) {
    if (r.p + qint64(w) * h * bytes > limit) return {};
    for (int y = 0; y < h; y++) { for (size_t i = 0; i < row.size(); i++) row[i] = uchar(r.u8()); uchar* o = img.scanLine(y); for (int x = 0; x < w; x++) o[x] = row[size_t(x) * bytes]; }
  } else {
    if (r.p + qint64(h) * 2 > limit) return {};
    std::vector<int> lens(h); for (int y = 0; y < h; y++) lens[y] = r.u16();
    for (int y = 0; y < h; y++) {
      if (r.p + lens[y] > limit || !unpackRow(r, lens[y], row.data(), int(row.size()))) return {};
      uchar* o = img.scanLine(y); for (int x = 0; x < w; x++) o[x] = row[size_t(x) * bytes];
    }
  }
  if (r.bad) return {};
  // (a tip is stored one way or the other by the file's age: a tip whose corners are full is the inverse of a round one)
  auto corner = [&](int cx, int cy) { int s = 0, n = 0; for (int y = cy; y < std::min(cy + 3, h); y++) for (int x = cx; x < std::min(cx + 3, w); x++) { s += img.constScanLine(y)[x]; n++; } return n ? s / n : 0; };
  const int cs = (corner(0, 0) + corner(std::max(0, w - 3), 0) + corner(0, std::max(0, h - 3)) + corner(std::max(0, w - 3), std::max(0, h - 3))) / 4;
  qint64 sum = 0; for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) sum += img.constScanLine(y)[x];
  if (cs > 200 && sum / (qint64(w) * h) > 128) img.invertPixels();
  return img;
}

void readV12(Rd& r, int version, std::vector<AbrBrush>& out) {
  const int count = r.i16();
  for (int i = 0; i < count && !r.bad; i++) {
    const int type = r.i16(); const qint32 size = r.i32(); const qint64 end = r.p + size; if (size < 0 || end > r.n) return;
    if (type == 2) {
      r.i32(); const int spacing = r.i16(); QString name;
      if (version == 2) { const int len = r.i32(); if (len > 0 && len < 512 && r.has(len * 2)) { for (int k = 0; k < len; k++) { const int c = r.u16(); if (c) name += QChar(c); } } else r.bad = true; }
      r.u8();                                   // (antialiasing)
      for (int k = 0; k < 4; k++) r.i16();     // (the bounds as 16 bit)
      const int t = r.i32(), l = r.i32(), b = r.i32(), rt = r.i32(), depth = r.i16(), comp = r.u8();
      QImage tip = r.bad ? QImage() : readSample(r, end, t, l, b, rt, depth, comp);
      if (!tip.isNull()) { AbrBrush a; a.name = name; a.tip = tip; a.spacing = spacing > 0 ? std::min(spacing, 999) / 100.f : -1.f; out.push_back(a); }
    }
    r.p = end;
  }
}

void readSamples(Rd& r, qint64 end, int sub, std::vector<AbrBrush>& out) {
  while (r.p + 4 <= end && !r.bad) {
    const qint32 len = r.i32(); if (len <= 0) break;
    const qint64 next = r.p + ((len + 3) & ~3); if (next > end + 4) break;
    const int idLen = r.u8(); QString id = r.has(idLen) ? QString::fromLatin1(reinterpret_cast<const char*>(r.d + r.p), idLen) : QString(); r.skip(idLen);
    const qint64 base = r.p; QImage tip;
    const int guesses[] = {sub == 1 ? 10 : 264, 10, 264};   // (what lies between the id and the bounds differs by the file's sub-version)
    for (int g : guesses) {
      r.p = base + g; r.bad = false; if (!r.has(22)) continue;
      const int t = r.i32(), l = r.i32(), b = r.i32(), rt = r.i32(), depth = r.i16(), comp = r.u8();
      tip = readSample(r, next, t, l, b, rt, depth, comp); r.bad = false;
      if (!tip.isNull()) break;
    }
    if (!tip.isNull()) { AbrBrush a; a.tip = tip; a.id = id; out.push_back(a); }
    r.p = next; r.bad = false;
  }
}
// ---- the "desc" block: Photoshop's descriptor (names, classes, typed values), read into a tree ----
struct DV { QString t, s; double n = 0; std::vector<DV> l; std::vector<std::pair<QString, DV>> o; };

QString dkey(Rd& r) { int n = r.i32(); if (n == 0) n = 4; if (n < 0 || n > 4096 || !r.has(n)) { r.bad = true; return {}; } QString s = QString::fromLatin1(reinterpret_cast<const char*>(r.d + r.p), n); r.skip(n); return s; }
QString dunicode(Rd& r) { const int n = r.i32(); if (n < 0 || n > 100000 || !r.has(qint64(n) * 2)) { r.bad = true; return {}; } QString s; for (int i = 0; i < n; i++) { const int c = r.u16(); if (c) s += QChar(c); } return s; }
double ddouble(Rd& r) { if (!r.has(8)) { r.bad = true; return 0; } quint64 v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | r.d[r.p + i]; r.p += 8; double d; std::memcpy(&d, &v, 8); return d; }
QString dtype(Rd& r) { if (!r.has(4)) { r.bad = true; return {}; } QString s = QString::fromLatin1(reinterpret_cast<const char*>(r.d + r.p), 4); r.skip(4); return s; }

DV dval(Rd& r, const QString& type, int depth);
DV ddesc(Rd& r, int depth) {
  DV d; d.t = "Objc"; if (depth > 24) { r.bad = true; return d; }
  dunicode(r); d.s = dkey(r); const int n = r.i32(); if (n < 0 || n > 100000) { r.bad = true; return d; }
  for (int i = 0; i < n && !r.bad; i++) { const QString key = dkey(r); const QString t = dtype(r); d.o.push_back({key, dval(r, t, depth + 1)}); }
  return d;
}
DV dval(Rd& r, const QString& t, int depth) {
  DV v; v.t = t;
  if (t == "Objc" || t == "GlbO") return ddesc(r, depth);
  if (t == "VlLs") { const int n = r.i32(); if (n < 0 || n > 100000) { r.bad = true; return v; } for (int i = 0; i < n && !r.bad; i++) { const QString it = dtype(r); v.l.push_back(dval(r, it, depth + 1)); } }
  else if (t == "doub") v.n = ddouble(r);
  else if (t == "UntF") { r.skip(4); v.n = ddouble(r); }
  else if (t == "long") v.n = r.i32();
  else if (t == "comp") r.skip(8);
  else if (t == "bool") v.n = r.u8();
  else if (t == "TEXT") v.s = dunicode(r);
  else if (t == "enum") { dkey(r); v.s = dkey(r); }
  else if (t == "tdta" || t == "alis") { const int n = r.i32(); if (n < 0) r.bad = true; else r.skip(n); }
  else if (t == "type" || t == "GlbC") { dunicode(r); dkey(r); }
  else r.bad = true;
  return v;
}
const DV* dget(const DV* d, const char* key) { if (!d) return nullptr; for (auto& [k, v] : d->o) if (k == QLatin1String(key)) return &v; return nullptr; }

// a computed brush's tip when it is not a circle: an ellipse by the roundness, soft by the hardness
QImage ellipseTip(float round, float hard) {
  const int W = 128, H = std::max(4, int(W * std::clamp(round, 0.02f, 1.f))); QImage im(W, H, QImage::Format_Grayscale8);
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { const float u = (x + 0.5f) / W * 2 - 1, v = (y + 0.5f) / H * 2 - 1, d = std::sqrt(u * u + v * v);
    float a = d >= 1 ? 0.f : d <= hard ? 1.f : 1 - (d - hard) / std::max(1e-3f, 1 - hard); a = a * a * (3 - 2 * a); im.scanLine(y)[x] = uchar(a * 255 + 0.5f); }
  return im;
}

std::vector<AbrBrush> readDesc(Rd& r) {
  std::vector<AbrBrush> out; r.i32();   // (the descriptor's version)
  const DV root = ddesc(r, 0); const DV* list = dget(&root, "Brsh"); if (!list) return out;
  for (const DV& item : list->l) {
    AbrBrush a; if (const DV* n = dget(&item, "Nm  ")) a.name = n->s;
    const DV* b = dget(&item, "Brsh");
    if (b) {
      if (auto* v = dget(b, "Dmtr")) a.diameter = float(v->n);
      if (auto* v = dget(b, "Hrdn")) a.hardness = float(v->n / 100);
      if (auto* v = dget(b, "Angl")) a.angle = float(v->n);
      if (auto* v = dget(b, "Rndn")) a.roundness = float(v->n / 100);
      if (auto* v = dget(b, "Spcn")) a.spacing = float(v->n / 100);
      if (auto* v = dget(b, "Intr")) if (v->n == 0) a.spacing = 0.05f;   // (spacing switched off: a smooth stroke)
      if (auto* v = dget(b, "sampledData")) a.id = v->s;
    }
    for (const DV* host : {&item, b}) {   // (the dynamics sit beside the tip's record or in it)
      if (!host) continue;
      if (auto* s = dget(host, "szVr")) { if (auto* j = dget(s, "jitter")) a.sizeJitter = std::clamp(float(j->n / 100), 0.f, 1.f); if (auto* t = dget(s, "bVTy")) a.pressureSize = t->n == 2; }
      if (auto* s = dget(host, "angleDynamics")) if (auto* j = dget(s, "jitter")) a.angleJitter = std::clamp(float(j->n / 100), 0.f, 1.f);
      const DV* use = dget(host, "useScatter"); if (use && use->n != 0) if (auto* s = dget(host, "scatterDynamics")) if (auto* j = dget(s, "jitter")) a.scatter = std::clamp(float(j->n / 100), 0.f, 3.f);
    }
    if (a.id.isEmpty() && a.name.isEmpty() && a.diameter < 0) continue;
    out.push_back(a);
  }
  return out;
}

}  // namespace

std::vector<AbrBrush> readAbr(const QByteArray& data, QString* error) {
  std::vector<AbrBrush> out; Rd r(data);
  const int version = r.i16();
  if (r.bad) { if (error) *error = "an empty file"; return out; }
  if (version == 1 || version == 2) readV12(r, version, out);
  else if (version >= 6 && version <= 10) {
    const int sub = r.i16(); std::vector<AbrBrush> samples, desc;
    while (r.has(12) && !r.bad) {
      char tag[4]; std::memcpy(tag, r.d + r.p, 4); r.skip(4);
      if (std::memcmp(tag, "8BIM", 4) != 0) break;
      char type[4]; std::memcpy(type, r.d + r.p, 4); r.skip(4);
      const qint32 len = r.i32(); const qint64 end = r.p + len; if (len < 0 || end > r.n) break;
      if (std::memcmp(type, "samp", 4) == 0) readSamples(r, end, sub, samples);
      else if (std::memcmp(type, "desc", 4) == 0) { Rd d(data); d.n = end; d.p = r.p; desc = readDesc(d); }
      r.p = end; r.bad = false;
    }
    // the descriptor's brushes in the file's order: a sampled one takes its tip from "samp" (by the id), a computed one is made
    std::vector<char> used(samples.size(), 0);
    for (AbrBrush a : desc) {
      if (!a.id.isEmpty()) { bool found = false; for (size_t i = 0; i < samples.size(); i++) if (samples[i].id == a.id) { a.tip = samples[i].tip; used[i] = 1; found = true; break; } if (!found) continue; }
      else { a.tip = (a.roundness < 0.99f) ? ellipseTip(a.roundness, std::clamp(a.hardness, 0.f, 1.f)) : QImage(); a.hardness = std::clamp(a.hardness, 0.f, 1.f); }
      out.push_back(a);
    }
    for (size_t i = 0; i < samples.size(); i++) if (!used[i]) out.push_back(samples[i]);   // (a tip the descriptor does not name)
  } else { if (error) *error = QString("an unknown ABR version %1").arg(version); return out; }
  if (out.empty() && error && error->isEmpty()) *error = "the file holds no brushes this reader understands";
  return out;
}

}  // namespace hm::paint
