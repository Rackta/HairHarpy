#include "scene/Looks.h"
#include <mutex>
#include <QCryptographicHash>
#include <QDateTime>
#include <QSet>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSettings>
#include <map>

namespace hm::looks {

namespace {
const QRegularExpression IMG_RE("\\.(png|jpe?g|webp|bmp|gif|avif|tga)$", QRegularExpression::CaseInsensitiveOption);
QString mimeOf(const QString& name) {
  QString e = QFileInfo(name).suffix().toLower();
  return e == "jpg" || e == "jpeg" ? "image/jpeg" : e == "webp" ? "image/webp" : e == "bmp" ? "image/bmp" : e == "gif" ? "image/gif" : e == "avif" ? "image/avif" : e == "tga" ? "image/x-tga" : "image/png";
}
QString hexOf(unsigned c) { return QString("#%1").arg(c, 6, 16, QChar('0')); }
// a kept image's identity: its length and a hash of its ends (hashing a whole 50 MB image on every redraw would be slow)
QString dataKey(const QString& d) { return "d:" + QString::number(d.size()) + ":" + QString::number(qHash(QStringView(d).left(8192))) + ":" + QString::number(qHash(QStringView(d).right(8192))); }

TexDir g_dir;
std::map<QString, size_t> g_byPath, g_byName;
int g_rev = 0;
bool g_loaded = false;

std::vector<TexFile> scan(const QString& root) {
  std::vector<TexFile> out; QDir r(root);
  std::function<void(const QString&, int)> walk = [&](const QString& dir, int depth) {
    QDir d(dir);
    for (const QFileInfo& fi : d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      if (out.size() >= 3000) return;
      if (fi.isFile() && IMG_RE.match(fi.fileName()).hasMatch()) out.push_back({r.relativeFilePath(fi.absoluteFilePath()), fi.fileName(), fi.absoluteFilePath(), fi.lastModified().toMSecsSinceEpoch(), fi.size()});
      else if (fi.isDir() && depth < 4 && !fi.fileName().startsWith('.')) walk(fi.absoluteFilePath(), depth + 1);
    }
  };
  if (!root.isEmpty() && r.exists()) walk(root, 0);
  std::sort(out.begin(), out.end(), [](const TexFile& a, const TexFile& b) { return a.path.compare(b.path, Qt::CaseInsensitive) < 0; });
  return out;
}
void setFiles(const QString& root, std::vector<TexFile> files) {
  g_dir.root = root; g_dir.name = root.isEmpty() ? QString() : QFileInfo(root).fileName(); g_dir.files = std::move(files);
  g_byPath.clear(); g_byName.clear();
  for (size_t i = 0; i < g_dir.files.size(); i++) { g_byPath.emplace(g_dir.files[i].path.toLower(), i); g_byName.emplace(g_dir.files[i].name.toLower(), i); }
  g_rev++;
}
void ensureLoaded() {                                        // (the folder of the last time, back at the start)
  if (g_loaded) return; g_loaded = true;
  QString root = QSettings().value("textures/folder").toString();
  if (!root.isEmpty() && QDir(root).exists()) setFiles(root, scan(root));
}
}  // namespace

// TGA (Qt has no reader of its own here): true colour / grey, plain or RLE, 8 / 16 / 24 / 32 bits, either origin
static QImage decodeTga(const QByteArray& b) {
  if (b.size() < 18) return {};
  const uchar* d = reinterpret_cast<const uchar*>(b.constData());
  int idLen = d[0], cmap = d[1], type = d[2], w = d[12] | d[13] << 8, h = d[14] | d[15] << 8, bpp = d[16], desc = d[17];
  if (cmap || !(type == 2 || type == 3 || type == 10 || type == 11) || w <= 0 || h <= 0 || !(bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) return {};
  const int Bp = bpp / 8; const bool rle = type >= 9; size_t p = 18 + size_t(idLen) + size_t(d[5] | d[6] << 8) * ((d[7] + 7) / 8);
  QImage img(w, h, QImage::Format_ARGB32);
  std::vector<uint32_t> px(size_t(w) * size_t(h));
  auto read = [&](size_t at) -> uint32_t {
    if (at + size_t(Bp) > size_t(b.size())) return 0xff000000;
    const uchar* q = d + at;
    switch (Bp) {
      case 1: return 0xff000000u | q[0] << 16 | q[0] << 8 | q[0];
      case 2: { int v = q[0] | q[1] << 8; int r = (v >> 10) & 31, g = (v >> 5) & 31, bl = v & 31; return 0xff000000u | (r * 255 / 31) << 16 | (g * 255 / 31) << 8 | (bl * 255 / 31); }
      case 3: return 0xff000000u | q[2] << 16 | q[1] << 8 | q[0];
      default: return uint32_t(q[3]) << 24 | q[2] << 16 | q[1] << 8 | q[0];
    }
  };
  size_t n = 0, total = px.size();
  if (!rle) { for (; n < total; n++, p += size_t(Bp)) px[n] = read(p); }
  else while (n < total && p < size_t(b.size())) {
    int c = d[p++], cnt = (c & 0x7f) + 1;
    if (c & 0x80) { uint32_t v = read(p); p += size_t(Bp); for (int i = 0; i < cnt && n < total; i++) px[n++] = v; }
    else for (int i = 0; i < cnt && n < total; i++, p += size_t(Bp)) px[n++] = read(p);
  }
  const bool top = desc & 0x20, rtl = desc & 0x10;
  for (int y = 0; y < h; y++) { uint32_t* row = reinterpret_cast<uint32_t*>(img.scanLine(top ? y : h - 1 - y));
    for (int x = 0; x < w; x++) row[rtl ? w - 1 - x : x] = px[size_t(y) * size_t(w) + size_t(x)]; }
  return img;
}
QImage decodeImage(const QByteArray& bytes) {
  QImage img; img.loadFromData(bytes);
  return img.isNull() ? decodeTga(bytes) : img;
}
QImage loadImage(const QString& file) {
  QImage img(file); if (!img.isNull()) return img;
  QFile f(file); return f.open(QIODevice::ReadOnly) ? decodeTga(f.readAll()) : QImage();
}

const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
// (kept per geometry: the materials in use are read off every polygon — a heavy mesh's millions, at every panel / outliner
// rebuild otherwise)
std::vector<TexSet> setsOf(const Item& m) {
  const PolyMesh* B = baseOf(m); if (!B) return {};
  struct C { const PolyMesh* B; int nf; size_t nm, fm; std::vector<TexSet> sets; };
  static std::vector<C> cache; static std::mutex mx; std::lock_guard<std::mutex> lk(mx);
  for (auto& c : cache) if (c.B == B && c.nf == B->faceCount() && c.nm == B->matNames.size() && c.fm == B->faceMat.size()) return c.sets;
  if (cache.size() > 64) cache.erase(cache.begin());
  cache.push_back({B, B->faceCount(), B->matNames.size(), B->faceMat.size(), texSets(*B)});
  return cache.back().sets;
}
bool hasUV(const Item& m) { const PolyMesh* B = baseOf(m); return B && !B->cornerUV.empty(); }

QJsonObject norm(const QJsonValue& look, const std::vector<TexSet>& sets) {
  QJsonObject L = look.toObject(); QJsonArray old = L.value("sets").toArray(), out;
  for (size_t i = 0; i < sets.size(); i++) {
    QJsonObject o;
    if (int(i) < old.size() && (old[int(i)].toObject().value("name").toString() == sets[i].name || old.size() == int(sets.size()))) o = old[int(i)].toObject();
    else for (const QJsonValue& x : old) if (x.toObject().value("name").toString() == sets[i].name) { o = x.toObject(); break; }
    QJsonObject maps, om = o.value("maps").toObject();
    for (const char* k : SLOT_KEY) if (om.contains(k)) maps.insert(k, om.value(k));
    QJsonObject s{{"name", sets[i].name}, {"on", o.value("on").toBool(true)}, {"maps", maps}};
    if (o.value("color").isString() && !o.value("color").toString().isEmpty()) s.insert("color", o.value("color"));
    if (o.contains("alpha") && !o.value("alpha").isNull()) s.insert("alpha", o.value("alpha").toBool());
    out.append(s);
  }
  return {{"tex", L.value("tex").toBool()}, {"flat", L.value("flat").toBool()}, {"sets", out}};
}
QJsonObject lookOf(const Item& m) { return norm(m.raw.value("look"), setsOf(m)); }
bool shown(const Item& m) { return m.raw.value("look").toObject().value("tex").toBool() && hasUV(m); }

QByteArray dataUrlBytes(const QString& url) {
  int c = url.indexOf(','); if (!url.startsWith("data:") || c < 0) return {};
  return url.left(c).contains(";base64") ? QByteArray::fromBase64(QStringView(url).mid(c + 1).toLatin1()) : QByteArray::fromPercentEncoding(url.mid(c + 1).toUtf8());
}
QJsonObject keptImage(const QString& file) {
  QFile f(file); QByteArray b = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  return {{"name", QFileInfo(file).fileName()}, {"data", "data:" + mimeOf(file) + ";base64," + QString::fromLatin1(b.toBase64())}};
}

MapSrc resolve(const Item& m, int set, int slot) {
  MapSrc r; auto sets = setsOf(m); if (set < 0 || size_t(set) >= sets.size()) return r;
  QJsonObject ls = norm(m.raw.value("look"), sets).value("sets").toArray()[set].toObject();
  QJsonObject maps = ls.value("maps").toObject(); const char* k = SLOT_KEY[slot];
  const PolyMesh* B = baseOf(m); int mat = sets[size_t(set)].mat;
  const PolyMesh::Map* own = B && mat >= 0 && size_t(mat) < B->matMaps.size() ? &B->matMaps[size_t(mat)][size_t(slot)] : nullptr;
  if (!maps.contains(k)) {                                    // the file's own
    if (own && !own->data.isEmpty()) { r.kind = MapSrc::File; r.bytes = own->data; r.key = QString("o:%1:%2:%3").arg(quintptr(B)).arg(mat).arg(slot); r.label = "from the file" + (own->name.isEmpty() ? QString() : ": " + own->name); }
    else r.label = own && !own->name.isEmpty() ? "from the file: " + own->name + " — not found" : QString("—");
    return r;
  }
  QJsonValue v = maps.value(k);
  if (v.isNull()) { r.removed = true; r.label = own && (!own->data.isEmpty() || !own->name.isEmpty()) ? "removed (the file has one)" : "—"; return r; }
  QJsonObject o = v.toObject();
  if (o.value("data").isString()) { QString d = o.value("data").toString(); r.kind = MapSrc::Kept; r.key = dataKey(d); r.bytes = dataUrlBytes(d); r.label = o.value("name").toString() + " · in the project"; return r; }
  const TexFile* f = texDirFile(o);
  r.label = "🔗 " + o.value("path").toString(o.value("name").toString());
  if (!f) { r.missing = true; QString dir = o.value("dir").toString(); r.label += " — not found" + (dir.isEmpty() ? QString() : QString(", connect «%1»").arg(dir)); return r; }
  r.kind = MapSrc::Linked; r.path = f->abs; r.key = QString("f:%1:%2:%3").arg(f->abs).arg(f->mtime).arg(f->size);
  return r;
}

QString setColor(const Item& m, int set, bool hasMap) {
  auto sets = setsOf(m); QJsonArray LS = norm(m.raw.value("look"), sets).value("sets").toArray();
  if (set >= 0 && set < LS.size()) { QString c = LS[set].toObject().value("color").toString(); if (!c.isEmpty()) return c; }
  if (hasMap) return "#ffffff";
  return set >= 0 && size_t(set) < sets.size() && !sets[size_t(set)].color.isEmpty() ? sets[size_t(set)].color : hexOf(HEAD_GREY);
}
bool setAlpha(const Item& m, int set) {
  auto sets = setsOf(m); QJsonArray LS = norm(m.raw.value("look"), sets).value("sets").toArray();
  if (set >= 0 && set < LS.size() && LS[set].toObject().contains("alpha") && !LS[set].toObject().value("alpha").isNull()) return LS[set].toObject().value("alpha").toBool();
  return set >= 0 && size_t(set) < sets.size() && sets[size_t(set)].alpha;
}

const TexDir& texDir() { ensureLoaded(); return g_dir; }
void connectTexDir(const QString& root) {
  ensureLoaded();
  QSettings().setValue("textures/folder", root);
  setFiles(root, root.isEmpty() ? std::vector<TexFile>{} : scan(root));
}
bool rescanTexDir() {
  ensureLoaded(); if (g_dir.root.isEmpty()) return false;
  auto list = scan(g_dir.root);
  bool changed = list.size() != g_dir.files.size();
  if (!changed) for (const TexFile& f : list) { auto it = g_byPath.find(f.path.toLower()); if (it == g_byPath.end()) { changed = true; break; } const TexFile& o = g_dir.files[it->second]; if (o.mtime != f.mtime || o.size != f.size) { changed = true; break; } }
  if (changed) { QString filter = g_dir.filter; setFiles(g_dir.root, std::move(list)); g_dir.filter = filter; }
  return changed;
}
const TexFile* texDirFile(const QJsonObject& o) {
  ensureLoaded();
  QString dir = o.value("dir").toString();
  if (g_dir.name.isEmpty() || (!dir.isEmpty() && dir != g_dir.name)) return nullptr;
  auto a = g_byPath.find(o.value("path").toString().toLower()); if (a != g_byPath.end()) return &g_dir.files[a->second];
  auto b = g_byName.find(o.value("name").toString().toLower()); return b != g_byName.end() ? &g_dir.files[b->second] : nullptr;
}
int revision() { ensureLoaded(); return g_rev; }

int linkedCount(const Scene& sc) {
  int n = 0;
  for (auto& [id, it] : sc.items) for (const QJsonValue& s : it.raw.value("look").toObject().value("sets").toArray())
    for (const char* k : SLOT_KEY) { QJsonValue v = s.toObject().value("maps").toObject().value(k); if (v.isObject() && !v.toObject().value("data").isString()) n++; }
  return n;
}

}  // namespace hm::looks

namespace hm::looks {
namespace {
QString projKey(const QString& path) { return "textures/byProject/" + QString::fromLatin1(QCryptographicHash::hash(QFileInfo(path).absoluteFilePath().toLower().toUtf8(), QCryptographicHash::Md5).toHex()); }
}
void rememberTexDir(Scene& sc) {
  ensureLoaded();
  if (g_dir.root.isEmpty()) sc.fileRaw.remove("texFolder"); else sc.fileRaw.insert("texFolder", g_dir.root);
  if (!sc.path.isEmpty()) QSettings().setValue(projKey(sc.path), g_dir.root);
}
void projectTexDir(const Scene& sc) {
  ensureLoaded();
  auto use = [](const QString& d) { if (d.isEmpty() || !QDir(d).exists()) return false; if (QDir(d) != QDir(g_dir.root)) { QSettings().setValue("textures/folder", d); setFiles(d, scan(d)); } return true; };
  if (use(sc.fileRaw.value("texFolder").toString())) return;                       // (the project's own)
  if (!sc.path.isEmpty()) { const QVariant v = QSettings().value(projKey(sc.path)); if (v.isValid()) { if (v.toString().isEmpty()) { if (!g_dir.root.isEmpty()) setFiles({}, {}); return; } if (use(v.toString())) return; } }
  // (none remembered: the folder its links name, next to the project or one level up)
  QSet<QString> names; for (auto& [id, it] : sc.items) for (const QJsonValue& s : it.raw.value("look").toObject().value("sets").toArray())
    for (const char* k : SLOT_KEY) { QJsonValue v = s.toObject().value("maps").toObject().value(k); if (v.isObject() && !v.toObject().value("data").isString()) { const QString d = v.toObject().value("dir").toString(); if (!d.isEmpty()) names.insert(d); } }
  if (names.isEmpty()) return;                                                       // (nothing linked: the folder stays as it is)
  if (names.contains(g_dir.name)) return;
  if (!sc.path.isEmpty()) { const QDir pd = QFileInfo(sc.path).absoluteDir(); for (const QString& n : names) for (const QString& c : {pd.filePath(n), QFileInfo(pd.absolutePath()).absoluteDir().filePath(n)}) if (use(c)) return; }
  if (!g_dir.root.isEmpty()) setFiles({}, {});                                      // (another project's folder would link the wrong images: disconnected)
}
}  // namespace hm::looks
