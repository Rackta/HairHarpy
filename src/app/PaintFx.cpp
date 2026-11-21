// Layer effects in the Paint room (paint/LayerFx.h): an effect layer over a layer or a group — its pixels made from the source's
// coverage over the surface, again in the background a moment after the source stops changing; a layer of its own with its own
// blend, so it goes to Photoshop as one (the author, 19.11: "отдельным слоем")
#include "app/PaintSession.h"
#include "paint/LayerFx.h"
#include "paint/Bake.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <thread>

namespace hm {

using namespace paint;

namespace {
// what the effect is made from: the layer itself, or a group's shown paint layers (deep), never an effect layer
void fxMembers(const Document& d, int src, std::vector<int>& out) {
  const Layer* s = d.layer(src); if (!s) return;
  if (s->kind == LayerKind::Paint) { if (s->fx.isEmpty()) out.push_back(src); return; }
  if (s->kind != LayerKind::Group) return;
  for (const Layer& l : d.layers()) if (l.parent == src && l.visible) fxMembers(d, l.id, out);
}
quint64 fnv(quint64 k, const void* p, size_t n) { const uchar* b = static_cast<const uchar*>(p); for (size_t i = 0; i < n; i++) { k ^= b[i]; k *= 1099511628211ULL; } return k; }
}  // namespace

int PaintSession::addEffect(int srcId, const QString& type) {
  const Layer* s = doc.layer(srcId); if (!s || (s->kind != LayerKind::Paint && s->kind != LayerKind::Group) || !s->fx.isEmpty()) return -1;
  const Channel ch = s->kind == LayerKind::Paint ? s->channel : modeChannel();
  if (mapInfo(ch).storage != Storage::Colour) return -1;
  const int id = doc.addPaint(ch, doc.freeName(s->name + QString::fromUtf8(" · ") + fxName(type)), srcId);   // (right over the source)
  Layer* l = doc.layer(id); if (!l) return -1;
  QJsonObject fx = fxDefaults(type); fx.insert("src", srcId); l->fx = fx;
  l->blend = type == "inner" ? Blend::Multiply : type == "bevel" ? Blend::Overlay : type == "shine" ? Blend::Screen : Blend::Normal;
  layer = id; edits++; emit layersChanged(); emit activeChanged(); requestFx(0);
  return id;
}

int PaintSession::addShadow(const QStringList& casters) {
  if (casters.isEmpty() || doc.width() <= 0) return -1;
  const Channel ch = modeChannel(); if (mapInfo(ch).storage != Storage::Colour) return -1;
  const int id = doc.addPaint(ch, doc.freeName(QStringLiteral("Shadow of ") + (casters.size() <= 2 ? casters.join(", ") : casters.first() + QString(" +%1").arg(casters.size() - 1))), layer);
  Layer* l = doc.layer(id); if (!l) return -1;
  QJsonObject fx = fxDefaults("shadow"); fx.insert("casters", QJsonArray::fromStringList(casters)); if (!doc.activeSet().isEmpty()) fx.insert("set", doc.activeSet());
  l->fx = fx; l->blend = Blend::Multiply; l->opacity = 0.45f;
  layer = id; edits++; emit layersChanged(); emit activeChanged(); requestFx(0);
  return id;
}

int PaintSession::splitShapeColour(int id) {
  Layer* l = doc.layer(id); if (!l || l->kind != LayerKind::Paint || !l->fx.isEmpty() || mapInfo(l->channel).storage != Storage::Colour) return -1;
  const QString name = l->name; const int c = doc.duplicate(id); if (c < 0) return -1;
  { QString bn = name + "Mask"; for (const Layer& x : doc.layers()) if (x.name == bn) { bn = doc.freeName(bn, "_"); break; }   // (the author's own names: EmeraldMask, MetalMask)
    if (Layer* b = doc.layer(id)) b->name = bn; }
  Layer* t = doc.layer(c); if (!t) return -1; t->name = name; t->clip = true; t->mask = {};   // (the shape is the base's: no mask of its own)
  for (auto& [k, p] : t->other) p.mask = {};
  layer = c; edits++; emit layersChanged(); emit activeChanged(); emit pixelsChanged({});
  return c;
}

void PaintSession::requestFx(int delayMs) {
  if (!fxTimer_) { fxTimer_ = new QTimer(this); fxTimer_->setSingleShot(true); connect(fxTimer_, &QTimer::timeout, this, [this] { runFx(); }); }
  fxTimer_->start(std::max(0, delayMs));
}

void PaintSession::runFx() {
  if (doc.width() <= 0) return;
  bool any = false; for (const Layer& l : doc.layers()) any |= !l.fx.isEmpty(); if (!any) return;
  std::vector<std::pair<QString, QSize>> sets{{doc.activeSet(), QSize(doc.width(), doc.height())}};
  for (const auto& x : doc.sets()) if (x.key != doc.activeSet() && x.w > 0 && x.h > 0) sets.push_back({x.key, QSize(x.w, x.h)});
  struct Job { int fid; QString set; quint64 key; QImage a8; std::shared_ptr<const FxFrame> frame; QJsonObject fx; };
  std::vector<Job> jobs;
  struct SJob { int fid; QString set; quint64 key; std::shared_ptr<const ShadowFrame> frame; std::shared_ptr<const TriMesh> casters; QJsonObject fx; };
  std::vector<SJob> sjobs;
  for (const Layer& l : doc.layers()) {
    if (l.fx.isEmpty()) continue;
    if (l.fx.value("type").toString() == "shadow") {          // (no source layer: made from the scene — the casters and the light)
      QStringList names; for (const auto& v : l.fx.value("casters").toArray()) names << v.toString();
      const auto cm = castersOf ? castersOf(names) : nullptr; const QByteArray fxSig = QJsonDocument(l.fx).toJson(QJsonDocument::Compact);
      for (const auto& [set, size] : sets) {
        if (l.fx.contains("set") && l.fx.value("set").toString() != set) continue;
        const qint64 grev = doc.geomRevOf ? doc.geomRevOf(set) : 0; const quintptr cp = quintptr(cm.get()); const int wh[2] = {size.width(), size.height()};
        quint64 key = fnv(1469598103934665603ULL, fxSig.constData(), size_t(fxSig.size())); key = fnv(key, &grev, sizeof grev); key = fnv(key, &cp, sizeof cp); key = fnv(key, wh, sizeof wh);
        auto& kept = fxKey_[{l.id, set}]; if (kept == key) continue; kept = key;
        if (!cm || cm->empty()) { QImage* p = set == doc.activeSet() ? &doc.layer(l.id)->image : doc.pixelsIn(l.id, set, true); if (p) { QImage e(size, formatOf(l.channel)); e.fill(Qt::transparent); *p = e; } continue; }
        const QString fk = set + QChar(0x1f) + QString::number(grev) + "x" + QString::number(size.width()) + "x" + QString::number(size.height());
        auto& fr = shFrames_[fk];
        if (!fr) { const float *P = nullptr, *N = nullptr; if (!doc.positionsOf || !doc.positionsOf(set, size.width(), size.height(), P, N) || !P || !N) { kept = 0; continue; }
          fr = shadowFrame(P, N, size.width(), size.height()); }
        sjobs.push_back({l.id, set, key, fr, cm, l.fx});
      }
      continue;
    }
    const int src = l.fx.value("src").toInt(-1); std::vector<int> mem; fxMembers(doc, src, mem);
    const QByteArray fxSig = QJsonDocument(l.fx).toJson(QJsonDocument::Compact);
    for (const auto& [set, size] : sets) {
      quint64 key = fnv(1469598103934665603ULL, fxSig.constData(), size_t(fxSig.size())); const qint64 grev = doc.geomRevOf ? doc.geomRevOf(set) : 0; key = fnv(key, &grev, sizeof grev);
      std::vector<const QImage*> ims;
      for (int m : mem) { QImage* im = doc.pixelsIn(m, set, false); if (!im || im->isNull() || im->size() != size) continue; ims.push_back(im); const qint64 ck = im->cacheKey(); key = fnv(key, &ck, sizeof ck); }
      if (ims.empty()) continue;                                     // (nothing of the source in this set)
      auto& kept = fxKey_[{l.id, set}]; if (kept == key) continue; kept = key;
      QImage a8(size, QImage::Format_Grayscale8); a8.fill(0);
      for (const QImage* im : ims) { const QImage c = im->format() == QImage::Format_ARGB32 ? *im : im->convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < size.height(); y++) { const QRgb* p = reinterpret_cast<const QRgb*>(c.constScanLine(y)); uchar* o = a8.scanLine(y); for (int x = 0; x < size.width(); x++) o[x] = std::max<uchar>(o[x], uchar(qAlpha(p[x]))); } }
      // (the surface's frame: made again when the geometry or the light changed)
      float L[3]; fxLight(l.fx.value("az").toDouble(30), l.fx.value("el").toDouble(55), L);
      quint64 fk = fnv(1469598103934665603ULL, L, sizeof L); fk = fnv(fk, &grev, sizeof grev); const int wh[2] = {size.width(), size.height()}; fk = fnv(fk, wh, sizeof wh);
      auto& fr = fxFrames_[set + QChar(0x1f) + QString::number(fk)];
      if (!fr.second) { const float *P = nullptr, *N = nullptr; if (!doc.positionsOf || !doc.positionsOf(set, size.width(), size.height(), P, N) || !P || !N) { kept = 0; continue; }
        fr = {fk, fxFrame(size.width(), size.height(), P, N, L)}; }
      jobs.push_back({l.id, set, key, a8, fr.second, l.fx});
    }
  }
  while (shFrames_.size() > 4) shFrames_.erase(shFrames_.begin());   // (old geometry let go)
  if (fxFrames_.size() > 8) { for (auto it = fxFrames_.begin(); it != fxFrames_.end() && fxFrames_.size() > 4;) it = fxFrames_.erase(it); }   // (old lights / sizes let go)
  QPointer<PaintSession> self(this);
  for (SJob& j : sjobs) std::thread([self, j] {
    float L[3]; fxLight(j.fx.value("az").toDouble(30), j.fx.value("el").toDouble(60), L);
    const QImage a8 = castShadow(*j.frame, *j.casters, L, float(j.fx.value("soft").toDouble(3)), j.fx.value("rays").toInt(16));
    const QColor c(j.fx.value("c1").toString("#6a5a7a")); QImage img(a8.size(), QImage::Format_ARGB32); img.fill(0);
    for (int y = 0; y < a8.height(); y++) { const uchar* a = a8.constScanLine(y); QRgb* o = reinterpret_cast<QRgb*>(img.scanLine(y)); for (int x = 0; x < a8.width(); x++) if (a[x]) o[x] = qRgba(c.red(), c.green(), c.blue(), a[x]); }
    QMetaObject::invokeMethod(self, [self, j, img] { if (!self) return; PaintSession* s = self;
      const auto it = s->fxKey_.find({j.fid, j.set}); if (it == s->fxKey_.end() || it->second != j.key) return;   // (changed meanwhile: a newer one is on its way)
      Layer* l = s->doc.layer(j.fid); if (!l || l->fx.isEmpty()) return;
      const QImage px = img.convertToFormat(formatOf(l->channel));
      if (j.set == s->doc.activeSet()) l->image = px; else if (QImage* p = s->doc.pixelsIn(j.fid, j.set, true)) *p = px;
      if (j.set == s->doc.activeSet()) emit s->pixelsChanged({}); else emit s->setPixels(j.set, {}); }, Qt::QueuedConnection);
  }).detach();
  for (Job& j : jobs) std::thread([self, j] {
    QImage img = fxRender(j.a8, *j.frame, j.fx);
    QMetaObject::invokeMethod(self, [self, j, img] { if (!self) return; PaintSession* s = self;
      const auto it = s->fxKey_.find({j.fid, j.set}); if (it == s->fxKey_.end() || it->second != j.key) return;   // (the source changed meanwhile: a newer one is on its way)
      Layer* l = s->doc.layer(j.fid); if (!l || l->fx.isEmpty()) return;
      const QImage px = img.convertToFormat(formatOf(l->channel));
      if (j.set == s->doc.activeSet()) l->image = px; else if (QImage* p = s->doc.pixelsIn(j.fid, j.set, true)) *p = px;
      if (j.set == s->doc.activeSet()) emit s->pixelsChanged({}); else emit s->setPixels(j.set, {}); }, Qt::QueuedConnection);
  }).detach();
}

}  // namespace hm
