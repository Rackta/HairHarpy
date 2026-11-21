#include "app/PaintSession.h"
#include "io/ProjectIO.h"
#include "scene/Scene.h"
#include <QFile>
#include <map>
#include <set>
#include "paint/Region.h"
#include "ui/Pie.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include "paint/Psd.h"
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <thread>
#include <QTimer>
#include <cmath>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace hm {

PaintSession::PaintSession(QObject* parent) : QObject(parent), bridge(this) {
  // (layer effects: made again a moment after their source changes — its pixels, the list, the effect's settings)
  connect(this, &PaintSession::pixelsChanged, this, [this](const QRect&) { requestFx(); });
  connect(this, &PaintSession::layersChanged, this, [this] { requestFx(); });
  lib.load(); brush = lib.brushes.empty() ? paint::Brush() : lib.brushes.front();
  alphaFavs = QSettings().value("paint/alphaFavs", QStringList{"made:2", "made:1", "made:3", "made:4"}).toStringList();   // (the hair brush first: the cylinder)
  pickSize = QSettings().value("paint/pickSize", 3).toInt(); pickFrom = QSettings().value("paint/pickFrom", "all").toString();
  islandLock = QSettings().value("paint/islandLock", false).toBool();
  // (a layer added, renamed, moved, its blend changed: a change to save, too — told by the layers' records, so a list merely shown anew is not one)
  connect(this, &PaintSession::layersChanged, this, [this] { const QByteArray sig = doc.width() > 0 ? QJsonDocument(doc.toJson()).toJson(QJsonDocument::Compact) : QByteArray();
    if (sig != layersSig_) { if (!layersSig_.isEmpty()) edits++; layersSig_ = sig; } });
  { QSettings st; drawMode = st.value("paint/drawMode", "stroke").toString(); drawFlat = st.value("paint/drawFlat", false).toBool(); drawThrough = st.value("paint/drawThrough", false).toBool(); lineSmooth = st.value("paint/lineSmooth", false).toBool(); stampMode = std::clamp(st.value("paint/stampMode", 0).toInt(), 0, 2); stampAll = st.value("paint/stampAll", false).toBool(); shapeOutline = st.value("paint/shapeOutline", false).toBool();
    lineStart = st.value("paint/lineStart", 1.0).toFloat(); lineEnd = st.value("paint/lineEnd", 1.0).toFloat(); lineTaper = st.value("paint/lineTaper", 0.25).toFloat(); recolorOp = paint::regionOpFromKey(st.value("paint/recolorOp", "hue").toString()); }
  { QSettings st; freezeHard = st.value("paint/freezeHard", true).toBool(); freezeSmooth = st.value("paint/freezeSmooth", 2.0).toFloat(); freezeSoft = st.value("paint/freezeSoft", 1.5).toFloat(); }
  { QSettings st; ovDark = st.value("paint/ovDark", true).toBool(); ovEdge = st.value("paint/ovEdge", true).toBool(); ovWire = st.value("paint/ovWire", false).toBool(); }
  { auto* keep = new QTimer(this); keep->setSingleShot(true); keep->setInterval(500);   // (the brush changed — a slider, a library pick: kept as the tool's once it rests)
    connect(this, &PaintSession::brushChanged, keep, qOverload<>(&QTimer::start)); connect(keep, &QTimer::timeout, this, [this] { rememberTool(); }); }
  connect(&bridge, &paint::Bridge::imageReturned, this, [this](const QString& key, const QImage& im) { whenIdle([this, key, im] {
    const int at = key.indexOf('@'); const int id = (at < 0 ? key : key.left(at)).toInt(); const QString set = at < 0 ? doc.activeSet() : key.mid(at + 1);
    paint::Layer* l = doc.layer(id); if (!l || l->kind != paint::LayerKind::Paint || im.isNull()) return;
    const QString cur = doc.activeSet(); const int cw = doc.width(), ch = doc.height(); bool other = set != cur;
    if (other) { bool known = false; for (const auto& x : doc.sets()) if (x.key == set) { const QString k = x.key; const int sw = x.w, sh = x.h; doc.setActiveSet(k, sw, sh); known = true; break; }   /* (copies and out: setActiveSet may grow the list walked here) */ if (!known) other = false; }   // (another UV set's picture: into that set's pixels)
    l = doc.layer(id); if (!l) return; const QImage::Format f = paint::formatOf(l->channel);
    l->image = (im.size() == QSize(doc.width(), doc.height()) ? im : im.scaled(doc.width(), doc.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)).convertToFormat(f);
    if (other) doc.setActiveSet(cur, cw, ch);
    emit pixelsChanged({}); ui::toast(tr("The layer came back from the editor%1").arg(other ? tr(" (UV set %1)").arg(set) : QString())); }); });
  connect(&bridge, &paint::Bridge::layersReturned, this, [this](const QString& path) { if (QFileInfo(path).fileName() == QLatin1String("hh_view_projection.psd")) return; whenIdle([this, path] { ui::toast(takeLayers(path)); }); });   // (the view sent for the projection: PaintProj takes it)   // (the layered PSD saved in the editor)
  connect(&bridge, &paint::Bridge::fileReturned, this, [this](const QString& path) { whenIdle([this, path] {   // (3D-Coat's export: a new layer from the file)
    QImage im(path); if (im.isNull()) return; ensureDoc();
    const int id = doc.addPaint(paint::Channel::Color, QFileInfo(path).completeBaseName(), layer);
    doc.layer(id)->image = im.scaled(doc.width(), doc.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
    layer = id; emit layersChanged(); emit pixelsChanged({}); }); });
}
// (a file back from the editor while a stroke is going: taken once the stroke ends — the stroke holds the layer's picture, and a layer
//  added meanwhile may move the list it lies in)
void PaintSession::whenIdle(std::function<void()> f) {
  if (strokeGoing && strokeGoing()) { QTimer::singleShot(250, this, [this, f = std::move(f)]() mutable { whenIdle(std::move(f)); }); return; }
  f();
}

void PaintSession::ensureDoc(int size) {
  if (!doc.empty() || doc.width() > 0) return;
  doc.reset(size, size); const int bg = doc.addFill(paint::Channel::Color, QColor(128, 128, 128), "Base colour"); layer = doc.addPaint(paint::Channel::Color, "Paint"); (void)bg;
  if (modeChannel() != paint::Channel::Color) layer = -1;
  emit layersChanged(); emit pixelsChanged({});
}

paint::Channel PaintSession::modeChannel() const {
  if (view == "normalBump") return paint::Channel::Height;
  bool ok = false; const paint::Channel c = paint::channelFromKey(view, &ok); return ok ? c : paint::Channel::Color;
}
void PaintSession::setView(const QString& key) {
  if (view == key) return; view = key;
  if (!key.startsWith("bake:")) { const paint::Channel ch = modeChannel(); const paint::Layer* cur = doc.layer(layer);
    if (!cur || cur->channel != ch || cur->kind == paint::LayerKind::Group) { layer = -1; for (const paint::Layer& l : doc.layers()) if (l.channel == ch && l.kind != paint::LayerKind::Group) layer = l.id; } }
  editMask = false; emit viewChanged(); emit layersChanged(); emit activeChanged(); emit pixelsChanged({}); emit brushChanged();
}
void PaintSession::setSet(const QString& key, int w, int h) {
  ui::DevOp devT("paintSet", 15, {{"set", key}});
  if (doc.width() == 0) { doc.reset(w, h); doc.setActiveSet(key, w, h); }
  else { if (key == doc.activeSet() && w == doc.width() && h == doc.height()) return;
    // (the steps kept stay: each knows its set. Only a set given another size loses its own — their pixels no longer fit)
    bool resized = key == doc.activeSet(); if (!resized) for (const auto& x : doc.sets()) if (x.key == key && (x.w != w || x.h != h)) resized = true;
    if (resized) { auto drop = [&](std::vector<Undo>& v) { v.erase(std::remove_if(v.begin(), v.end(), [&](const Undo& u) { return u.set == key; }), v.end()); }; drop(undo); drop(redo); }
    if (key != doc.activeSet()) emit setLeaving();
    doc.setActiveSet(key, w, h); }
  emit viewChanged(); emit layersChanged(); emit pixelsChanged({});
}
// (a step of another UV set: that set shown first — false: the set is no longer there)
bool PaintSession::showSetOf(const Undo& u) {
  if (u.set == doc.activeSet()) return true;
  for (const auto& x : doc.sets()) if (x.key == u.set) { const QString k = x.key; const int w = x.w, h = x.h; emit setLeaving(); doc.setActiveSet(k, w, h); emit viewChanged(); emit layersChanged(); emit pixelsChanged({}); return true; }
  return false;
}
QImage PaintSession::normalResult() const {
  if (doc.width() == 0) return {};
  const bool bump = doc.hasLayers(paint::Channel::Height), bk = baked.contains("normal");
  if (!bump && !bk) return {};
  // (no Bake mask layers: the whole bake is used — as before the mask; with them: only where it is painted)
  const QImage mask = bk && doc.hasLayers(paint::Channel::BakeMask) ? doc.flatten(paint::Channel::BakeMask) : QImage();
  return paint::finalNormal(bk ? baked.value("normal") : QImage(), mask, bump ? doc.compose(paint::Channel::Height) : QImage(), bumpStrength, bakeS.flipGreen, QSize(doc.width(), doc.height()));
}

paint::BrushMode PaintSession::effectiveMode() const {
  using paint::BrushMode; using paint::Channel;
  const paint::Layer* l = doc.layer(layer); BrushMode m = brush.mode;
  if (editMask) return m == BrushMode::Erase || m == BrushMode::MaskHide || m == BrushMode::Lower ? BrushMode::MaskHide : BrushMode::MaskReveal;
  if (!l) return m;
  if (l->channel == Channel::Height) { if (m == BrushMode::Paint) return BrushMode::Raise; if (m == BrushMode::Erase) return BrushMode::Lower; }
  if (l->channel == Channel::Flow && (m == BrushMode::Paint)) return BrushMode::Flow;
  if (paint::mapInfo(l->channel).storage == paint::Storage::Colour && (m == BrushMode::Raise || m == BrushMode::Lower || m == BrushMode::Flatten || m == BrushMode::Flow)) return BrushMode::Paint;
  if (l->channel == Channel::Height && m == BrushMode::Flow) return BrushMode::Raise;
  if (l->channel == Channel::Flow && m != BrushMode::Erase) return BrushMode::Flow;
  return m;
}

void PaintSession::stepBrush(bool up, bool hardness) {
  if (hardness) { brush.hardness = std::clamp(brush.hardness + (up ? 0.1f : -0.1f), 0.f, 1.f); brush.tip = {}; ui::hud(tr("Brush hardness %1%").arg(int(std::lround(brush.hardness * 100))), 700); }
  else { brush.size = up ? std::min(2048.f, brush.size / 0.85f) : std::max(1.f, brush.size * 0.85f); ui::hud(tr("Brush size %1").arg(int(std::lround(brush.size))), 700); }
  emit brushChanged(); rememberTool();
}

namespace {
bool paintingTool(const QString& t) { return t == "brush" || t == "eraser" || t == "blur" || t == "smudge" || t == "recolor" || t == "stamp"; }
// a tool's brush in the settings: the brush's own fields and, for its tip, the library brush it came from (by name and set)
void storeToolBrush(const QString& t, const paint::Brush& b) { QSettings st; st.setValue("paint/tool/" + t, QJsonDocument(b.toJson()).toJson(QJsonDocument::Compact)); }
}  // namespace

bool ownBrushTool(const QString& t) { return paintingTool(t) || t == "fill" || t == "text"; }   // (the fill and the text: their own opacity, not the brush's)
void PaintSession::rememberTool() { if (ownBrushTool(tool)) { toolBrush[tool] = brush; storeToolBrush(tool, brush); } }
void PaintSession::recallTool(const QString& t) {
  if (!ownBrushTool(t)) return;
  auto it = toolBrush.find(t);
  if (it == toolBrush.end()) {                                  // (not in hand yet this time: what the settings kept, its tip from the library)
    const QByteArray j = QSettings().value("paint/tool/" + t).toByteArray();
    if (!j.isEmpty()) { paint::Brush b = paint::Brush::fromJson(QJsonDocument::fromJson(j).object());
      for (const paint::Brush& l : lib.brushes) if (l.name == b.name && l.set == b.set) { b.tip = l.tip; break; }
      it = toolBrush.emplace(t, b).first; } }
  if (it != toolBrush.end()) { brush = it->second; emit brushChanged(); }
}

std::optional<QColor> PaintSession::pickColor(const QPointF& tex, double radiusTex) const {
  if (doc.width() <= 0) return std::nullopt;
  const paint::IslandMap* im = islands ? islands() : nullptr; const bool byIsland = im && !im->ids.isNull();
  const int island = byIsland ? im->at(tex) : 0;
  if (byIsland && island == 0) return std::nullopt;             // (outside the UV islands: the background, nothing to take)
  // (the patch is round in the texture, so it reaches the islands packed beside this one — another strand, the dress under the
  // dress: their colours are not the one under the pointer. Only the texels of the island under the pointer are taken — 18.11
  // "the pipette takes the colour through the mesh: the under-dress, the hair")
  const bool whole = pickSize <= 0;                             // (the mean under the whole brush)
  const int n = whole ? std::clamp(int(std::lround(radiusTex > 0 ? radiusTex : brush.size / 2)), 0, 512) : pickSize >= 5 ? 2 : pickSize >= 3 ? 1 : 0, cx = int(std::floor(tex.x())), cy = int(std::floor(tex.y()));
  const QRect r = QRect(cx - n, cy - n, 2 * n + 1, 2 * n + 1).intersected(QRect(0, 0, doc.width(), doc.height())); if (r.isEmpty()) return std::nullopt;
  QImage c;
  if (pickFrom == "layer") { const paint::Layer* l = doc.layer(layer); if (!l || l->image.isNull() || l->image.format() != QImage::Format_ARGB32) return std::nullopt; c = l->image.copy(r); }   // (the active layer alone, as it is painted)
  else if (pickFrom == "normal") {                              // (the painted colour under the tints: the layers in another blend and the adjustments left out for this look)
    auto& d = const_cast<paint::Document&>(doc); std::vector<int> off;
    for (const paint::Layer& l : d.layers()) if (l.visible && l.kind != paint::LayerKind::Group && (l.kind == paint::LayerKind::Adjust || l.blend != paint::Blend::Normal)) off.push_back(l.id);
    for (int id : off) d.layer(id)->visible = false;
    c = d.compose(paint::Channel::Color, r).convertToFormat(QImage::Format_ARGB32);
    for (int id : off) d.layer(id)->visible = true; }
  else c = doc.compose(paint::Channel::Color, r).convertToFormat(QImage::Format_ARGB32);
  double R = 0, G = 0, B = 0, W = 0;
  for (int y = 0; y < c.height(); y++) { const QRgb* p = reinterpret_cast<const QRgb*>(c.constScanLine(y));
    for (int x = 0; x < c.width(); x++) { if (whole) { const int dx = r.x() + x - cx, dy = r.y() + y - cy; if (dx * dx + dy * dy > n * n) continue; }   // (a round patch: the brush's)
      if (byIsland && n > 0 && im->at(r.x() + x, r.y() + y) != island) continue;   // (another island)
      const double a = qAlpha(p[x]) / 255.0; if (a < 0.5) continue; R += qRed(p[x]) * a; G += qGreen(p[x]) * a; B += qBlue(p[x]) * a; W += a; } }   // (half-transparent edge texels: not a colour to take)
  if (W <= 0) return std::nullopt;
  return QColor(int(std::lround(R / W)), int(std::lround(G / W)), int(std::lround(B / W)));
}

// ---- the alphas ----
namespace {
const paint::Brush* libBrush(const paint::BrushLibrary& lib, const QString& key) {   // ("lib:<set>/<name>")
  const QString k = key.mid(4); for (const paint::Brush& b : lib.brushes) if (paint::BrushLibrary::setOf(b) + "/" + b.name == k) return &b; return nullptr; }
}  // namespace
bool PaintSession::applyAlpha(const QString& key) {
  QImage tip; paint::TipKind kind = paint::TipKind::Image;
  if (key == "own") { restoreTip(); alphaKey = "own"; emit brushChanged(); return true; }
  float hard = -1, soft = -1;
  if (key.startsWith("round:")) { kind = paint::TipKind::Round; hard = std::clamp(key.mid(6).toFloat(), 0.f, 1.f); }
  else if (key.startsWith("square:")) { kind = paint::TipKind::Square; soft = std::clamp(key.mid(7).toFloat(), 0.f, 1.f); }
  else if (key.startsWith("file:")) { QImage im(key.mid(5)); if (im.isNull()) return false;   // (white paints; a picture with clear parts — its opaque parts paint)
    bool clear = false; if (im.hasAlphaChannel()) { const QImage a = im.convertToFormat(QImage::Format_ARGB32); for (int y = 0; y < a.height() && !clear; y += 4) { const QRgb* r = reinterpret_cast<const QRgb*>(a.constScanLine(y)); for (int x = 0; x < a.width(); x += 4) if (qAlpha(r[x]) < 250) { clear = true; break; } } }
    if (clear) { const QImage a = im.convertToFormat(QImage::Format_ARGB32); tip = QImage(a.size(), QImage::Format_Grayscale8); for (int y = 0; y < a.height(); y++) { const QRgb* r = reinterpret_cast<const QRgb*>(a.constScanLine(y)); uchar* d = tip.scanLine(y); for (int x = 0; x < a.width(); x++) d[x] = uchar(qAlpha(r[x])); } }
    else tip = im.convertToFormat(QImage::Format_Grayscale8);
    if (std::max(tip.width(), tip.height()) > 512) tip = tip.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation); }
  else if (key.startsWith("made:")) kind = paint::TipKind(std::clamp(key.mid(5).toInt(), 1, 4));
  else if (key.startsWith("lib:")) { const paint::Brush* b = libBrush(lib, key); if (!b) return false; if (b->tip.isNull()) kind = paint::TipKind::Round; else tip = b->tip; }
  else return false;
  if (!alphaStandIn) { ownTip_ = brush.tip; ownKind_ = brush.tipKind; ownHard_ = brush.hardness; ownSoft_ = brush.tipSoft; alphaStandIn = true; }
  brush.tip = tip; brush.tipKind = kind; if (hard >= 0) brush.hardness = hard; if (soft >= 0) brush.tipSoft = soft; alphaKey = key; emit brushChanged(); ui::hud(tr("Alpha: %1").arg(alphaLabel(key)), 700); return true;
}
QString PaintSession::alphaFolder() { return paint::BrushLibrary::presetFolder() + "/Alphas"; }
QStringList PaintSession::userAlphas() const {
  QStringList out; for (const QFileInfo& f : QDir(alphaFolder()).entryInfoList({"*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp", "*.tga", "*.tif", "*.tiff"}, QDir::Files, QDir::Name)) out << "file:" + f.absoluteFilePath(); return out; }
void PaintSession::restoreTip() { alphaKey = "own"; if (!alphaStandIn) return; brush.tip = ownTip_; brush.tipKind = ownKind_; brush.hardness = ownHard_; brush.tipSoft = ownSoft_; alphaStandIn = false; emit brushChanged(); ui::hud(tr("The brush's own tip back"), 700); }
QString PaintSession::alphaLabel(const QString& key) const {
  if (key == "own") return tr("The brush's own");
  if (key.startsWith("round:")) { const float h = key.mid(6).toFloat(); return h > 0.9f ? tr("Hard circle") : h > 0.3f ? tr("Soft circle") : tr("Very soft circle"); }
  if (key.startsWith("square:")) return key.mid(7).toFloat() > 0.05f ? tr("Soft square") : tr("Square");
  if (key.startsWith("file:")) return QFileInfo(key.mid(5)).completeBaseName();
  if (key.startsWith("made:")) { switch (paint::TipKind(key.mid(5).toInt())) { case paint::TipKind::Round: return tr("Round"); case paint::TipKind::Cylinder: return tr("Cylinder"); case paint::TipKind::Square: return tr("Square"); case paint::TipKind::Gradient: return tr("Gradient"); default: return key; } }
  return key.mid(key.indexOf('/') + 1);
}
QImage PaintSession::alphaThumb(const QString& key, int px) const {
  QImage a;
  auto roundOf = [px](float hard) { return paint::proceduralTip(paint::TipKind::Round, std::clamp(1 - hard, 0.02f, 1.f), 0, 0, px); };
  if (key == "own") { const QImage t = alphaStandIn ? ownTip_ : brush.tip; const paint::TipKind k = alphaStandIn ? ownKind_ : brush.tipKind; const float h = alphaStandIn ? ownHard_ : brush.hardness;
    a = k == paint::TipKind::Image && !t.isNull() ? t : (k == paint::TipKind::Round || k == paint::TipKind::Image) ? roundOf(h) : paint::proceduralTip(k, brush.tipSoft, brush.tipFlat, brush.tipCore, px); }
  else if (key.startsWith("round:")) a = roundOf(key.mid(6).toFloat());
  else if (key.startsWith("square:")) a = paint::proceduralTip(paint::TipKind::Square, key.mid(7).toFloat(), 0, 0, px);
  else if (key.startsWith("file:")) { QImage im(key.mid(5)); if (!im.isNull()) a = im.hasAlphaChannel() ? [&] { const QImage x = im.convertToFormat(QImage::Format_ARGB32).scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation); QImage g(x.size(), QImage::Format_Grayscale8);
      for (int y = 0; y < x.height(); y++) { const QRgb* r = reinterpret_cast<const QRgb*>(x.constScanLine(y)); uchar* d = g.scanLine(y); for (int i = 0; i < x.width(); i++) d[i] = qAlpha(r[i]) < 250 ? uchar(qAlpha(r[i])) : uchar(qGray(r[i])); } return g; }() : im; }
  else if (key.startsWith("made:")) a = paint::proceduralTip(paint::TipKind(key.mid(5).toInt()), brush.tipSoft, brush.tipFlat, brush.tipCore, px);
  else if (const paint::Brush* b = libBrush(lib, key)) a = b->tip.isNull() ? paint::proceduralTip(paint::TipKind::Round, 1 - b->hardness, 0, 0, px) : b->tip;
  if (a.isNull()) a = paint::proceduralTip(paint::TipKind::Round, 0.5f, 0, 0, px);
  QImage out(px, px, QImage::Format_ARGB32); out.fill(QColor(28, 28, 30));   // (white on dark — the alpha as it paints)
  const QImage g = a.scaled(px - 6, px - 6, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
  for (int y = 0; y < g.height(); y++) for (int x = 0; x < g.width(); x++) { const int v = g.constScanLine(y)[x]; out.setPixelColor(x + (px - g.width()) / 2, y + (px - g.height()) / 2, QColor(28 + (227 * v) / 255, 28 + (227 * v) / 255, 30 + (225 * v) / 255)); }
  return out;
}
void PaintSession::saveFavs() const { QSettings().setValue("paint/alphaFavs", alphaFavs); }

qint64 PaintSession::undoBudget() {
  static const qint64 v = [] {
    qint64 ram = 8LL << 30;
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; if (GlobalMemoryStatusEx(&ms)) ram = qint64(ms.ullTotalPhys);
#else
    const long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGE_SIZE); if (pages > 0 && psz > 0) ram = qint64(pages) * psz;
#endif
    return std::clamp(ram / 8, 512LL << 20, 4LL << 30); }();
  return v;
}
int PaintSession::pushUndo(Undo u) {
  static int serial = 0; u.id = ++serial; u.set = doc.activeSet(); redo.clear(); undo.push_back(std::move(u)); edits++;
  qint64 sum = 0; for (const Undo& x : undo) sum += x.bytes();
  while (undo.size() > 1 && (undo.size() > 40 || sum > undoBudget())) { sum -= undo.front().bytes(); undo.erase(undo.begin()); }   // (the newest step always stays)
  return undo.back().id;
}
namespace {
// (the bytes of the pictures in `kept` that `now` no longer shares — what a layer-list step alone keeps alive)
qint64 aloneBytes(const std::vector<paint::Layer>& kept, const std::vector<paint::Layer>& now) {
  QSet<qint64> live; auto note = [&live](const QImage& i) { if (!i.isNull()) live.insert(i.cacheKey()); };
  for (const paint::Layer& l : now) { note(l.image); note(l.mask); for (const auto& [k, p] : l.other) { note(p.image); note(p.mask); } }
  qint64 n = 0; auto add = [&](const QImage& i) { if (!i.isNull() && !live.contains(i.cacheKey())) n += i.sizeInBytes(); };
  for (const paint::Layer& l : kept) { add(l.image); add(l.mask); for (const auto& [k, p] : l.other) { add(p.image); add(p.mask); } }
  return n;
}
}  // namespace
bool PaintSession::layersEdit(const std::function<bool()>& op) {
  auto kept = std::make_shared<std::vector<paint::Layer>>(doc.layers()); const int was = layer;   // (the pictures shared, not copied)
  if (!op()) return false;
  Undo u; u.list = kept; u.active = was; u.extra = aloneBytes(*kept, doc.layers()); pushUndo(std::move(u)); return true;
}
PaintSession::Undo* PaintSession::undoById(int id) { for (auto it = undo.rbegin(); it != undo.rend(); ++it) if (it->id == id) return &*it; return nullptr; }

namespace {
// a step put back into its image; what it replaced comes back as the step the other way; r — the part changed (null: all of it)
PaintSession::Undo swapStep(PaintSession::Undo&& u, QImage& t, QRect& r) {
  PaintSession::Undo back; back.layer = u.layer; back.mask = u.mask; back.id = u.id; back.set = u.set; r = {};
  if (u.tiles.empty()) { if (u.img.isNull()) return back;   /* (a stroke that left nothing: an empty step) */ back.img = t; t = u.img; return back; }
  const int bpp = t.depth() / 8;
  for (const auto& tile : u.tiles) {
    const QRect tr = QRect(tile.at, tile.img.size()).intersected(t.rect()); if (tr.isEmpty() || tile.img.format() != t.format()) continue;
    back.tiles.push_back({tr.topLeft(), t.copy(tr)});
    for (int y = 0; y < tr.height(); y++) std::memcpy(t.scanLine(tr.y() + y) + size_t(tr.x()) * bpp, tile.img.constScanLine(y), size_t(tr.width()) * bpp);
    r = r.united(tr);
  }
  return back;
}
}  // namespace

// ---- the freeze mask ----
QImage* PaintSession::freezeOf(bool make) {
  if (doc.width() <= 0) return nullptr; const QString k = doc.activeSet(); const QSize sz(doc.width(), doc.height());
  auto it = freeze.find(k);
  if (it != freeze.end() && !it->isNull()) { if (it->size() != sz) *it = it->scaled(sz).convertToFormat(QImage::Format_Grayscale8); return &*it; }   // (the set resized: the mask with it)
  if (!make) return nullptr;
  QImage m(sz, QImage::Format_Grayscale8); m.fill(0); return &(freeze[k] = m);
}
bool PaintSession::freezeAny() const {
  const auto it = freeze.constFind(doc.activeSet()); if (it == freeze.constEnd() || it->isNull()) return false;
  for (int y = 0; y < it->height(); y++) { const uchar* p = it->constScanLine(y); for (int x = 0; x < it->width(); x++) if (p[x]) return true; }
  return false;
}
void PaintSession::freezeStep() { ensureDoc(); if (QImage* m = freezeOf(true)) { Undo u; u.layer = -2; u.img = *m; pushUndo(std::move(u)); } }
void PaintSession::freezeClear() { if (!freezeAny()) return; freezeStep(); if (QImage* m = freezeOf(false)) m->fill(0); emit pixelsChanged({}); }   // (kept, empty: Ctrl+Z brings it back)
void PaintSession::freezeInvert() { freezeStep(); if (QImage* m = freezeOf(true)) { m->invertPixels(); emit pixelsChanged({}); } }
void PaintSession::freezeSolid() {
  if (!freezeAny()) return; freezeStep(); QImage* m = freezeOf(false); if (!m) return;
  *m = paint::maskSolid(*m, 1, freezeSmooth, freezeSoft); emit pixelsChanged({});
}
void PaintSession::freezeEdge(int op) {
  if (!freezeAny()) return; freezeStep(); QImage* m = freezeOf(false); if (!m) return;
  *m = paint::maskEdge(*m, paint::EdgeOp(std::clamp(op, 0, 3)), freezeEdgePx, islands ? islands() : nullptr); freezeShown = true; emit pixelsChanged({});
}
bool PaintSession::freezeAdd(const QImage& cov, const QPoint& at, bool remove) {
  if (cov.isNull() || cov.format() != QImage::Format_Grayscale8) return false; ensureDoc(); QImage* m = freezeOf(true); if (!m) return false;
  const QRect r = QRect(at, cov.size()).intersected(m->rect()); if (r.isEmpty()) return false;
  freezeStep();
  for (int y = r.top(); y <= r.bottom(); y++) { uchar* d = m->scanLine(y); const uchar* c = cov.constScanLine(y - at.y()) - at.x();
    for (int x = r.left(); x <= r.right(); x++) { if (!c[x]) continue; d[x] = remove ? uchar(d[x] * (255 - c[x]) / 255) : std::max(d[x], c[x]); } }
  freezeShown = true; emit pixelsChanged(r); return true;
}
bool PaintSession::freezeFillAt(const QPointF& tex, bool remove) {
  ensureDoc(); QImage* m = freezeOf(true); if (!m) return false;
  const QPoint p(int(std::floor(tex.x())), int(std::floor(tex.y()))); const paint::IslandMap* im = islands ? islands() : nullptr; QRect box;
  QImage src = *m; if (remove) src.invertPixels();             // (freeing: the frozen patch round the point — the free one of the mask turned over)
  const QImage cov = paint::floodFree(src, p, 128, im, 2, &box); if (cov.isNull()) return false;
  return freezeAdd(cov.copy(box), box.topLeft(), remove);
}
bool PaintSession::freezeFromLayer(bool painted) {
  const paint::Layer* l = doc.layer(layer); if (!l || l->image.isNull() || l->image.format() != QImage::Format_ARGB32) return false;
  QImage cov(l->image.size(), QImage::Format_Grayscale8);
  for (int y = 0; y < cov.height(); y++) { uchar* d = cov.scanLine(y); const QRgb* s = reinterpret_cast<const QRgb*>(l->image.constScanLine(y)); for (int x = 0; x < cov.width(); x++) d[x] = uchar(painted ? qAlpha(s[x]) : 255 - qAlpha(s[x])); }
  return freezeAdd(cov, QPoint(0, 0), false);
}
QStringList PaintSession::freezeNames() const {
  QStringList out; const QString pre = doc.activeSet() + QChar(0x1f);
  for (auto it = freezeKept.constBegin(); it != freezeKept.constEnd(); ++it) if (it.key().startsWith(pre)) out << it.key().mid(pre.size());
  return out;
}
void PaintSession::freezeSave(const QString& name) { if (name.trimmed().isEmpty()) return; if (QImage* m = freezeOf(false)) { freezeKept[doc.activeSet() + QChar(0x1f) + name.trimmed()] = *m; emit settingsChanged(); } }
bool PaintSession::freezeLoad(const QString& name, int how) {
  const auto it = freezeKept.constFind(doc.activeSet() + QChar(0x1f) + name); if (it == freezeKept.constEnd() || it->isNull()) return false;
  ensureDoc(); QImage* m = freezeOf(true); if (!m) return false;
  const QImage k = it->size() == m->size() ? *it : it->scaled(m->size()).convertToFormat(QImage::Format_Grayscale8);
  if (how == 0) { freezeStep(); *m = k; freezeShown = true; emit pixelsChanged({}); return true; }
  return freezeAdd(k, QPoint(0, 0), how == 2);
}
void PaintSession::freezeDrop(const QString& name) { freezeKept.remove(doc.activeSet() + QChar(0x1f) + name); emit settingsChanged(); }
// ---- all the layers to the editor as one PSD, and back ----
QString PaintSession::sendLayers(const QImage& wire, const QString& saveTo) {
  if (doc.width() <= 0) return tr("Nothing to send");
  const paint::Channel ch = modeChannel(); if (paint::mapInfo(ch).storage != paint::Storage::Colour) return tr("Layers go to the editor from a colour or a mask map — not from the bump or the flow");
  std::vector<paint::PsdLayer> out; PsdSent sent; sent.set = doc.activeSet(); sent.ch = ch;
  for (const paint::Layer& l : doc.layers()) {                  // (bottom → top, as the PSD holds them)
    if (l.kind == paint::LayerKind::Fill && l.gradLive && l.channel == ch && (l.onlySets.isEmpty() || l.onlySets.contains(doc.activeSet()))) {
      // (a live gradient: as its pixels, "(live)" in the name — seen and blended in the editor, left alone when the file comes back:
      // it stays live here)
      paint::Layer* m = doc.layer(l.id); const int wasSolo = doc.solo; const float wasOp = m->opacity; const bool wasVis = m->visible;
      doc.solo = l.id; m->opacity = 1; m->visible = true; const QImage im = doc.compose(ch); doc.solo = wasSolo; m->opacity = wasOp; m->visible = wasVis;
      if (!im.isNull()) { paint::PsdLayer p; p.name = l.name + " (live)"; p.image = im.convertToFormat(QImage::Format_ARGB32); p.blend = l.blend; p.opacity = l.opacity; p.visible = l.visible; out.push_back(p); }
      continue; }
    if (l.kind != paint::LayerKind::Paint || l.channel != ch || l.image.isNull() || l.image.format() != QImage::Format_ARGB32) continue;
    QString name = l.name.isEmpty() ? QString("Layer %1").arg(l.id) : l.name; if (sent.names.contains(name)) name += QString(" #%1").arg(l.id);   // (two layers of one name: told apart in the file)
    paint::PsdLayer p; p.name = name; p.image = l.image; p.blend = l.blend; p.opacity = l.opacity; p.visible = l.visible; out.push_back(p); sent.ids.push_back(l.id); sent.names << name; }
  if (out.empty()) return tr("No paint layer of this map has pixels in this UV set");
  if (!wire.isNull()) { paint::PsdLayer p; p.name = "Wireframe"; p.image = wire.size() == QSize(doc.width(), doc.height()) ? wire : wire.scaled(doc.width(), doc.height()); p.visible = false; out.push_back(p); }
  QString safe = doc.activeSet().isEmpty() ? QString("texture") : doc.activeSet(); safe.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
  sent.path = saveTo.isEmpty() ? bridge.workFolder() + "/Layers_" + safe + ".psd" : saveTo;
  // (the freeze mask of this set as an alpha channel "Freeze": Ctrl+click on it in Photoshop's Channels — the frozen part selected,
  // no outline drawn again by hand there)
  std::vector<paint::PsdAlpha> alphas; { const auto fz = freeze.constFind(doc.activeSet()); if (fz != freeze.constEnd() && !fz->isNull()) {
      bool any = false; for (int y = 0; y < fz->height() && !any; y++) { const uchar* r = fz->constScanLine(y); for (int x = 0; x < fz->width(); x++) if (r[x]) { any = true; break; } }
      if (any) alphas.push_back({"Freeze", *fz}); } }
  QString err; if (!paint::writePsd(sent.path, doc.width(), doc.height(), out, doc.compose(ch), &err, alphas)) return err;
  psdSent_ = sent;
  if (saveTo.isEmpty()) { paint::BridgeApp app = bridge.editor(); if (app == paint::BridgeApp::ThreeDCoat) app = paint::BridgeApp::Photoshop; if (!bridge.sendFile(app, sent.path, &err)) return err; }
  return tr("%1 layers sent as %2 — save the file in the editor and they come back").arg(sent.ids.size()).arg(QFileInfo(sent.path).fileName());
}
QString PaintSession::importLayers(const QString& path, std::function<int(int, int)> grow) {
  if (view.startsWith("bake:")) return tr("A baked map is shown — pick a map to bring the layers into");
  if (mapInfo(modeChannel()).storage != paint::Storage::Colour) return tr("Layers from a file go into a colour or mask map — not the bump or the flow");
  ensureDoc(); const int W = doc.width(), H = doc.height(); if (W <= 0 || H <= 0) return tr("No texture to bring the layers into");
  std::vector<paint::PsdLayer> in; int fw = 0, fh = 0; QString err;
  if (QFileInfo(path).suffix().compare("psd", Qt::CaseInsensitive) == 0) { if (!paint::readPsd(path, &fw, &fh, &in, &err)) return err.isEmpty() ? tr("The file could not be read") : err; }
  else { QImage im(path); if (im.isNull()) return tr("The picture could not be read"); fw = im.width(); fh = im.height(); paint::PsdLayer l; l.name = QFileInfo(path).completeBaseName(); l.image = im.convertToFormat(QImage::Format_ARGB32); in.push_back(l); }
  int W2 = W, H2 = H;
  if ((fw > W || fh > H) && grow) { const int g = grow(fw, fh); if (g < 0) return tr("Nothing brought in");
    if (g == 1) { setSet(doc.activeSet(), fw, fh); W2 = doc.width(); H2 = doc.height(); } }
  const int Wt = W2, Ht = H2;
  int added = 0, empty = 0, under = layer; const bool scaled = fw != Wt || fh != Ht;
  for (const paint::PsdLayer& p : in) {                          // (bottom → top: each over the one before)
    if (!p.pixels || p.image.isNull()) continue;
    QImage img = p.image.format() == QImage::Format_ARGB32 ? p.image : p.image.convertToFormat(QImage::Format_ARGB32);
    if (scaled) img = img.scaled(Wt, Ht, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    bool any = false; for (int y = 0; y < img.height() && !any; y++) { const QRgb* q = reinterpret_cast<const QRgb*>(img.constScanLine(y)); for (int x = 0; x < img.width(); x++) if (qAlpha(q[x])) { any = true; break; } }
    if (!any) { empty++; continue; }                             // (a layer of the file with nothing in it — another UV set's, as 3D-Coat writes them all)
    const int id = doc.addPaint(modeChannel(), p.name, under);
    if (paint::Layer* n = doc.layer(id)) { n->image = img; n->blend = p.blend; n->opacity = p.opacity; n->visible = p.visible; }
    under = id; added++; edits++; }
  if (!added) return empty ? tr("Every layer of the file is empty in this UV set (%1) — show the UV set the file was made for").arg(doc.activeSet().isEmpty() ? tr("single texture") : doc.activeSet()) : tr("No pixel layers in the file");
  layer = under; emit layersChanged(); emit activeChanged(); emit pixelsChanged({});
  QString r = tr("%1 layer(s) brought in").arg(added); if (empty) r += tr(", %1 empty left out").arg(empty); if (scaled) r += tr(" · scaled from %1×%2 to %3×%4").arg(fw).arg(fh).arg(Wt).arg(Ht); else if (Wt != W || Ht != H) r += tr(" · the UV set's texture is now %1×%2").arg(Wt).arg(Ht);
  return r;
}

namespace {
// the same picture to the eye: every texel's alpha equal, the colour equal where anything shows (a PSD keeps no colour under a fully
// clear texel — a layer sent and taken back unedited is not a change)
bool sameShown(const QImage& a, const QImage& b) {
  if (a.size() != b.size()) return false; if (a.isNull()) return true;
  const QImage x = a.format() == QImage::Format_ARGB32 ? a : a.convertToFormat(QImage::Format_ARGB32), y = b.format() == QImage::Format_ARGB32 ? b : b.convertToFormat(QImage::Format_ARGB32);
  for (int r = 0; r < x.height(); r++) { const QRgb* p = reinterpret_cast<const QRgb*>(x.constScanLine(r)); const QRgb* q = reinterpret_cast<const QRgb*>(y.constScanLine(r));
    for (int c = 0; c < x.width(); c++) if (p[c] != q[c] && (qAlpha(p[c]) || qAlpha(q[c]))) return false; }
  return true;
}
}  // namespace

QString PaintSession::takeLayers(const QString& path) {
  if (psdSent_.ids.empty()) return tr("No layers were sent to the editor from this project");
  if (doc.activeSet() != psdSent_.set) return tr("The layers were sent from the UV set %1 — show it, then save the file in the editor again").arg(psdSent_.set.isEmpty() ? tr("(single texture)") : psdSent_.set);
  int w = 0, h = 0; std::vector<paint::PsdLayer> in; QString err; if (!paint::readPsd(path, &w, &h, &in, &err)) return err;
  if (w != doc.width() || h != doc.height()) return tr("The file is %1×%2, the texture is %3×%4 — the picture's size must stay as it was sent").arg(w).arg(h).arg(doc.width()).arg(doc.height());
  std::vector<char> used(psdSent_.ids.size(), 0); int changed = 0, added = 0, removed = 0, under = -1;
  for (const paint::PsdLayer& p : in) {
    if (!p.pixels || p.name.startsWith("Wireframe") || p.name.endsWith(" (live)")) continue;   // (the wire; a live gradient — it stays live here)
    int k = -1; for (size_t i = 0; i < psdSent_.ids.size(); i++) if (!used[i] && psdSent_.names[int(i)] == p.name) { k = int(i); break; }
    paint::Layer* l = k >= 0 ? doc.layer(psdSent_.ids[size_t(k)]) : nullptr;
    if (l) { used[size_t(k)] = 1;
      if (!sameShown(l->image, p.image)) { if (!l->locked) { Undo u; u.layer = l->id; u.img = l->image; pushUndo(std::move(u)); l->image = p.image; changed++; } }   // (a locked layer keeps its pixels)
      l->blend = p.blend; l->opacity = p.opacity; l->visible = p.visible; under = l->id; }
    else { const int id = doc.addPaint(psdSent_.ch, p.name, under);   // (new in the editor: over the layer that lies under it there)
      if (paint::Layer* n = doc.layer(id)) { n->image = p.image; n->blend = p.blend; n->opacity = p.opacity; n->visible = p.visible; }
      if (under < 0) { const int at = doc.indexOf(id); for (int i = at; i > 0; i--) doc.move(id, -1); }   // (the lowest layer of the file: under everything)
      under = id; added++; edits++; }
  }
  for (size_t i = 0; i < psdSent_.ids.size(); i++) if (!used[i]) { const paint::Layer* l = doc.layer(psdSent_.ids[i]); if (l && !l->locked) { doc.remove(l->id); removed++; edits++; } }   // (merged or deleted in the editor)
  if (!doc.layer(layer)) layer = under >= 0 ? under : (doc.layers().empty() ? -1 : doc.layers().back().id);
  // (the file as it is now is what was "sent": the next save there is told against it)
  PsdSent s2; s2.set = psdSent_.set; s2.path = psdSent_.path; s2.ch = psdSent_.ch;
  for (const paint::Layer& l : doc.layers()) { if (l.kind != paint::LayerKind::Paint || l.channel != s2.ch || l.image.isNull() || l.image.format() != QImage::Format_ARGB32) continue; QString name = l.name; if (s2.names.contains(name)) name += QString(" #%1").arg(l.id); s2.ids.push_back(l.id); s2.names << name; }
  psdSent_ = s2;
  emit layersChanged(); emit activeChanged(); emit pixelsChanged({});
  return tr("Layers back from the editor: %1 changed, %2 new, %3 removed").arg(changed).arg(added).arg(removed);
}

// ---- the mask as a region of the active layer ----
namespace {
// how much of a texel is in the region (0…255): the mask there, or what it leaves free
inline int regionAt(const QImage* fz, int x, int y, bool free) { const int f = fz ? fz->constScanLine(y)[x] : 0; return free ? 255 - f : f; }
}  // namespace
namespace {
// sRGB 0…255 ↔ CIE Lab (D65)
void toLab(int r8, int g8, int b8, double out[3]) {
  auto lin = [](double c) { c /= 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
  const double r = lin(r8), g = lin(g8), b = lin(b8);
  double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047, y = 0.2126 * r + 0.7152 * g + 0.0722 * b, z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
  auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116; }; x = f(x); y = f(y); z = f(z);
  out[0] = 116 * y - 16; out[1] = 500 * (x - y); out[2] = 200 * (y - z); }
QRgb fromLab(const double L[3], int a) {
  const double fy = (L[0] + 16) / 116, fx = fy + L[1] / 500, fz = fy - L[2] / 200;
  auto fi = [](double t) { return t * t * t > 0.008856 ? t * t * t : (t - 16.0 / 116) / 7.787; };
  const double x = fi(fx) * 0.95047, y = fi(fy), z = fi(fz) * 1.08883;
  double r = 3.2406 * x - 1.5372 * y - 0.4986 * z, g = -0.9689 * x + 1.8758 * y + 0.0415 * z, b = 0.0557 * x - 0.2040 * y + 1.0570 * z;
  auto gam = [](double c) { c = std::clamp(c, 0.0, 1.0); return int(std::lround((c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055) * 255)); };
  return qRgba(gam(r), gam(g), gam(b), a); }
}  // namespace
QString PaintSession::matchColourBelow(int id) {
  paint::Layer* l = doc.layer(id);
  if (!l || l->kind != paint::LayerKind::Paint || l->image.isNull() || l->image.format() != QImage::Format_ARGB32) return tr("Match colour works on a colour paint layer");
  if (l->locked) return tr("The layer is locked");
  // (what lies under it: the layers below composed — this one and the ones above it hidden for a moment)
  std::vector<std::pair<int, bool>> was; bool from = false;
  for (const paint::Layer& x : doc.layers()) { if (x.id == id) from = true; if (from) was.push_back({x.id, x.visible}); }
  for (auto& [i, v] : was) if (paint::Layer* x = doc.layer(i)) x->visible = false;
  QImage below = doc.compose(l->channel); for (auto& [i, v] : was) if (paint::Layer* x = doc.layer(i)) x->visible = v;
  if (below.isNull()) return tr("Nothing under the layer to match");
  below = below.convertToFormat(QImage::Format_ARGB32); if (below.size() != l->image.size()) below = below.scaled(l->image.size());
  double sm[3] = {}, ss[3] = {}, tm[3] = {}, ts[3] = {}; qint64 n = 0;   // (the layer's and the under-painting's means and squares, where both have paint)
  const int st = std::max(1, int(std::sqrt(double(l->image.width()) * l->image.height() / 1.5e6)));   // (a big map read every few texels)
  for (int y = 0; y < l->image.height(); y += st) { const QRgb* p = reinterpret_cast<const QRgb*>(l->image.constScanLine(y)); const QRgb* q = reinterpret_cast<const QRgb*>(below.constScanLine(y));
    for (int x = 0; x < l->image.width(); x += st) { if (qAlpha(p[x]) < 128 || qAlpha(q[x]) < 128) continue; double a[3], b[3]; toLab(qRed(p[x]), qGreen(p[x]), qBlue(p[x]), a); toLab(qRed(q[x]), qGreen(q[x]), qBlue(q[x]), b);
      for (int k = 0; k < 3; k++) { sm[k] += a[k]; ss[k] += a[k] * a[k]; tm[k] += b[k]; ts[k] += b[k] * b[k]; } n++; } }
  if (n < 16) return tr("The layer lies over nothing painted — nothing to match it to");
  double gain[3], mS[3], mT[3];
  for (int k = 0; k < 3; k++) { mS[k] = sm[k] / n; mT[k] = tm[k] / n; const double sS = std::sqrt(std::max(1e-6, ss[k] / n - mS[k] * mS[k])), sT = std::sqrt(std::max(1e-6, ts[k] / n - mT[k] * mT[k]));
    gain[k] = std::clamp(sT / sS, 0.33, 3.0); }
  Undo u; u.layer = l->id; u.img = l->image; pushUndo(std::move(u));
  for (int y = 0; y < l->image.height(); y++) { QRgb* p = reinterpret_cast<QRgb*>(l->image.scanLine(y));
    for (int x = 0; x < l->image.width(); x++) { const int a = qAlpha(p[x]); if (!a) continue; double c[3]; toLab(qRed(p[x]), qGreen(p[x]), qBlue(p[x]), c);
      for (int k = 0; k < 3; k++) c[k] = (c[k] - mS[k]) * gain[k] + mT[k]; c[0] = std::clamp(c[0], 0.0, 100.0); p[x] = fromLab(c, a); } }
  edits++; emit pixelsChanged({});
  return tr("Colour matched to what lies under the layer: lightness %1 → %2, %3 texels compared — Ctrl+Z takes it back").arg(mS[0], 0, 'f', 0).arg(mT[0], 0, 'f', 0).arg(n);
}
bool PaintSession::regionErase(bool free) {
  paint::Layer* l = active(); const QImage* fz = freezeOf(false);
  if (!l || l->kind != paint::LayerKind::Paint || l->locked || l->image.isNull() || l->image.format() != QImage::Format_ARGB32 || (fz && fz->size() != l->image.size())) return false;
  if (!fz && !free) return false;                              // (nothing frozen: nothing to erase "where it is frozen")
  Undo u; u.layer = l->id; u.img = l->image; pushUndo(std::move(u));
  for (int y = 0; y < l->image.height(); y++) { QRgb* p = reinterpret_cast<QRgb*>(l->image.scanLine(y)); for (int x = 0; x < l->image.width(); x++) { const int w = regionAt(fz, x, y, free); if (!w) continue; p[x] = qRgba(qRed(p[x]), qGreen(p[x]), qBlue(p[x]), qAlpha(p[x]) * (255 - w) / 255); } }
  emit pixelsChanged({}); return true;
}
int PaintSession::regionLift(bool free, bool cut) {
  paint::Layer* l = active(); const QImage* fz = freezeOf(false);
  if (!l || l->kind != paint::LayerKind::Paint || l->image.isNull() || l->image.format() != QImage::Format_ARGB32 || (fz && fz->size() != l->image.size()) || (!fz && !free)) return -1;
  if (cut && l->locked) return -1;
  QImage part = l->image; bool any = false;
  for (int y = 0; y < part.height(); y++) { QRgb* p = reinterpret_cast<QRgb*>(part.scanLine(y)); for (int x = 0; x < part.width(); x++) { const int a = qAlpha(p[x]) * regionAt(fz, x, y, free) / 255; any |= a > 0; p[x] = qRgba(qRed(p[x]), qGreen(p[x]), qBlue(p[x]), a); } }
  if (!any) return -1;
  const int src = l->id; const QString name = l->name; const paint::Channel ch = l->channel; const paint::Blend bl = l->blend;
  if (cut) regionErase(free);                                   // (its own Ctrl+Z step)
  const int id = doc.addPaint(ch, name + (cut ? tr(" (cut)") : tr(" (part)")), src);   // (the list may have moved: the layers looked up again)
  if (paint::Layer* n = doc.layer(id)) { n->image = part; n->blend = bl; }
  layer = id; emit layersChanged(); emit activeChanged(); emit pixelsChanged({}); return id;
}
int PaintSession::regionAdjust(paint::AdjustKind k, bool free) {
  paint::Layer* l = active(); const QImage* fz = freezeOf(false);
  if (!l || paint::mapInfo(l->channel).storage != paint::Storage::Colour || (!fz && !free)) return -1;
  const int W = doc.width(), H = doc.height(); if (fz && fz->size() != QSize(W, H)) return -1;
  QImage m(W, H, QImage::Format_Grayscale8); for (int y = 0; y < H; y++) { uchar* d = m.scanLine(y); for (int x = 0; x < W; x++) d[x] = uchar(regionAt(fz, x, y, free)); }
  // (clipped to the layer it was made for: it changes nothing where that one is empty; over another adjustment — clipped as that one is)
  const bool clip = l->kind == paint::LayerKind::Group ? false : l->kind == paint::LayerKind::Adjust ? l->clip : true; const int id = doc.addAdjust(k, {}, l->id, l->channel);
  if (paint::Layer* a = doc.layer(id)) { a->mask = m; a->maskOn = true; a->clip = clip; }
  layer = id; emit layersChanged(); emit activeChanged(); emit pixelsChanged({}); return id;
}

void PaintSession::setThrough(bool on, bool keep) { if (drawThrough == on) return; drawThrough = on; if (keep) saveDrawPrefs(); emit throughChanged(); }
void PaintSession::saveDrawPrefs() const { QSettings st; st.setValue("paint/drawMode", drawMode); st.setValue("paint/drawFlat", drawFlat); st.setValue("paint/drawThrough", drawThrough); st.setValue("paint/lineSmooth", lineSmooth); st.setValue("paint/shapeOutline", shapeOutline);
  st.setValue("paint/lineStart", lineStart); st.setValue("paint/lineEnd", lineEnd); st.setValue("paint/lineTaper", lineTaper); st.setValue("paint/recolorOp", paint::regionOpKey(recolorOp)); }
void PaintSession::saveFreezePrefs() const { QSettings st; st.setValue("paint/freezeHard", freezeHard); st.setValue("paint/freezeSmooth", freezeSmooth); st.setValue("paint/freezeSoft", freezeSoft); }

// One step back or forth. A stroke of the projected brush (multi) is one step whatever UV sets it passed over: the layer's picture
// put back in each of them, none of them shown for it. A colour layer's step in a set that is not shown is put back where it lies.
// A layer mask's / the freeze mask's step: its set shown first
bool PaintSession::stepSwap(std::vector<Undo>& from, std::vector<Undo>& to) {
  while (!from.empty()) {
    Undo u = std::move(from.back()); from.pop_back();
    if (u.list) { if (!showSetOf(u)) continue;   // (a layer-list step: the list as it was put back whole, the one replaced kept as the step the other way)
      Undo back; back.id = u.id; back.set = u.set; back.active = layer; back.list = std::make_shared<std::vector<paint::Layer>>(doc.layers());
      doc.restoreLayers(std::move(*u.list)); back.extra = aloneBytes(*back.list, doc.layers());
      layer = doc.layer(u.active) ? u.active : -1; if (!active() || active()->mask.isNull()) editMask = showMask = false;
      to.push_back(std::move(back)); emit layersChanged(); emit activeChanged(); emit pixelsChanged({}); return true; }
    if (!u.multi.empty()) { Undo back; back.layer = u.layer; back.id = u.id; back.set = u.set;
      for (auto& [set, img] : u.multi) { QImage* t = doc.pixelsIn(u.layer, set, true); if (!t || t->size() != img.size()) continue; back.multi.push_back({set, *t}); *t = img;
        if (set == doc.activeSet()) emit pixelsChanged({}); else emit setPixels(set, QRect()); }
      if (back.multi.empty()) continue; to.push_back(std::move(back)); return true; }
    if (u.layer != -2 && !u.mask && u.set != doc.activeSet()) { QImage* t = doc.pixelsIn(u.layer, u.set, false); if (!t) continue;
      const QString set = u.set; QRect r; to.push_back(swapStep(std::move(u), *t, r)); emit setPixels(set, r); return true; }
    if (!showSetOf(u)) continue;
    if (u.layer == -2) { QImage* f = freezeOf(false); if (!f) continue; QRect r; to.push_back(swapStep(std::move(u), *f, r)); emit pixelsChanged(r); return true; }   // (a stroke of the freeze mask)
    paint::Layer* l = doc.layer(u.layer); if (!l) continue;
    QImage& t = u.mask ? l->mask : l->image; QRect r; to.push_back(swapStep(std::move(u), t, r)); emit pixelsChanged(r); return true;
  }
  return false;
}
bool PaintSession::undoStroke() { return stepSwap(undo, redo); }
bool PaintSession::redoStroke() { return stepSwap(redo, undo); }

QJsonObject PaintSession::toJson() const {
  QJsonObject o; if (doc.width() > 0) o["doc"] = doc.toJson();
  o["layer"] = layer; o["view"] = view; o["color"] = color.name(); o["brush"] = brush.toJson(); o["rig"] = rig.toJson(); o["toon"] = toon.toJson(); o["bump"] = bumpStrength;
  QJsonObject b; b["size"] = bakeS.size; b["pad"] = bakeS.padding; b["cage"] = bakeS.cage; b["back"] = bakeS.back; b["aa"] = bakeS.samples; b["aoRays"] = bakeS.aoRays; b["aoDist"] = bakeS.aoDist;
  b["normal"] = bakeS.normal; b["ao"] = bakeS.ao; b["height"] = bakeS.height; b["position"] = bakeS.position; b["curv"] = bakeS.curvature; b["col"] = bakeS.colour; b["flipG"] = bakeS.flipGreen; b["high"] = highId; b["low"] = lowId;
  o["bake"] = b; if (!material.nodes.empty()) o["mat"] = material.toJson(); if (!texIn.isEmpty()) o["texIn"] = QJsonArray::fromStringList(texIn); return o;
}

// another project comes (opened, made new, restored): nothing of the one before stays — its layers and their pixels, the
// steps to take back or redo, the baked maps, the freeze masks, the list of textures brought in, the material
void PaintSession::resetForProject() {
  doc.reset(0, 0); layer = -1; undo.clear(); redo.clear(); baked.clear(); freeze.clear(); freezeKept.clear(); texIn.clear(); material = paint::MatGraph();
}

void PaintSession::fromJson(const QJsonObject& o) {
  resetForProject();                // (a project never painted has no record: it kept the previous project's layers — and saved them as its own)
  if (o.value("mat").isObject()) material.fromJson(o.value("mat").toObject());
  for (const QJsonValue& v : o.value("texIn").toArray()) texIn << v.toString();
  if (o.isEmpty()) { emit layersChanged(); emit pixelsChanged({}); return; }
  if (o.contains("doc")) doc.fromJson(o.value("doc").toObject());
  layer = o.value("layer").toInt(-1); if (!doc.layer(layer)) layer = doc.layers().empty() ? -1 : doc.layers().back().id;
  view = o.value("view").toString("color"); color = QColor(o.value("color").toString("#ffffff")); if (o.contains("brush")) { brush = paint::Brush::fromJson(o.value("brush").toObject());
    for (const paint::Brush& l : lib.brushes) if (l.name == brush.name && l.set == brush.set) { brush.tip = l.tip; break; } }   // (the project keeps no tip picture: the library brush of that name gives it back, as recallTool does)
  rig = paint::LightRig::fromJson(o.value("rig").toObject()); toon = paint::ToonSettings::fromJson(o.value("toon").toObject()); bumpStrength = float(o.value("bump").toDouble(1));
  const QJsonObject b = o.value("bake").toObject(); if (!b.isEmpty()) {
    bakeS.size = b.value("size").toInt(2048); bakeS.padding = b.value("pad").toInt(8); bakeS.cage = float(b.value("cage").toDouble(0.02)); bakeS.back = float(b.value("back").toDouble(0.02)); bakeS.samples = b.value("aa").toInt(1);
    bakeS.aoRays = b.value("aoRays").toInt(16); bakeS.aoDist = float(b.value("aoDist").toDouble(0.08)); bakeS.normal = b.value("normal").toBool(true); bakeS.ao = b.value("ao").toBool(true);
    bakeS.height = b.value("height").toBool(true); bakeS.position = b.value("position").toBool(); bakeS.curvature = b.value("curv").toBool(); bakeS.colour = b.value("col").toBool(); bakeS.flipGreen = b.value("flipG").toBool();
    highId = b.value("high").toInt(-1); lowId = b.value("low").toInt(-1); }
  undo.clear(); emit layersChanged(); emit pixelsChanged({}); emit brushChanged(); emit settingsChanged();
}

namespace {
// (a freeze mask's file: freeze.png — the single texture's, freeze@<UV set>.png; a character Windows forbids in a file name written as
//  %XX — a plain name stays as it always was)
const QString kBadInName = QStringLiteral("<>:\"/\\|?*");
QString freezeFile(const QString& set) {
  if (set.isEmpty()) return QStringLiteral("freeze.png");
  QString s; for (const QChar c : set) { if (c.unicode() < 32 || kBadInName.contains(c)) s += QString("%%1").arg(int(c.unicode()), 2, 16, QChar('0')).toUpper(); else s += c; }
  return "freeze@" + s + ".png";
}
QString freezeSetOf(const QString& file) {
  if (file == QLatin1String("freeze.png")) return {};
  const QString k = file.mid(7, file.size() - 11); QString s;
  for (int i = 0; i < k.size(); i++) { if (k[i] == '%' && i + 2 < k.size()) { bool ok = false; const int v = k.mid(i + 1, 2).toInt(&ok, 16); if (ok && (v < 32 || kBadInName.contains(QChar(v)))) { s += QChar(v); i += 2; continue; } } s += k[i]; }
  return s;
}
}  // namespace

bool PaintSession::saveFiles(const QString& projectPath) const {
  ui::DevOp devT("paintSave", 0);
  if (doc.width() == 0 || projectPath.isEmpty()) return true;
  const bool ok = doc.saveTo(sideFolder(projectPath));
  // (the freeze masks beside the layers: freeze.png — the single texture's, freeze@<UV set>.png; a mask cleared — its file gone)
  const QDir d(sideFolder(projectPath)); QSet<QString> kept;
  auto anyIn = [](const QImage& m) { for (int y = 0; y < m.height(); y++) { const uchar* p = m.constScanLine(y); for (int x = 0; x < m.width(); x++) if (p[x]) return true; } return false; };
  for (auto it = freeze.constBegin(); it != freeze.constEnd(); ++it) { if (it->isNull() || !anyIn(*it)) continue; const QString f = freezeFile(it.key()); if (it->save(d.filePath(f))) kept.insert(f); }
  // (the masks kept under a name: fmask_<the UV set, hex>_<the name, hex>.png — any name is a safe file name that way)
  for (auto it = freezeKept.constBegin(); it != freezeKept.constEnd(); ++it) { if (it->isNull()) continue; const int cut = it.key().indexOf(QChar(0x1f));
    const QString f = "fmask_" + QString::fromLatin1(it.key().left(cut).toUtf8().toHex()) + "_" + QString::fromLatin1(it.key().mid(cut + 1).toUtf8().toHex()) + ".png"; if (it->save(d.filePath(f))) kept.insert(f); }
  for (const QString& f : d.entryList({"freeze.png", "freeze@*.png", "fmask_*.png"}, QDir::Files)) if (!kept.contains(f)) QFile::remove(d.filePath(f));
  return ok;
}

void PaintSession::saveFilesAsync(const QString& projectPath, bool own, std::function<void(bool)> done) {
  if (doc.width() == 0 || projectPath.isEmpty()) { if (done) done(true); return; }
  if (saving_) { saveNext_ = {projectPath, own, std::move(done), true}; return; }
  saving_ = true; const QString dir = sideFolder(projectPath);
  auto plan = std::make_shared<paint::Document::SavePlan>(doc.savePlan(dir)); plan->mine = own;
  // (the freeze masks — the one painted, the ones kept under a name: their pictures as they are now)
  struct Fz { QImage im; QString name; }; auto fz = std::make_shared<std::vector<Fz>>();
  auto anyIn = [](const QImage& m) { for (int y = 0; y < m.height(); y++) { const uchar* p = m.constScanLine(y); for (int x = 0; x < m.width(); x++) if (p[x]) return true; } return false; };
  for (auto it = freeze.constBegin(); it != freeze.constEnd(); ++it) if (!it->isNull() && anyIn(*it)) fz->push_back({*it, freezeFile(it.key())});
  for (auto it = freezeKept.constBegin(); it != freezeKept.constEnd(); ++it) { if (it->isNull()) continue; const int cut = it.key().indexOf(QChar(0x1f));
    fz->push_back({*it, "fmask_" + QString::fromLatin1(it.key().left(cut).toUtf8().toHex()) + "_" + QString::fromLatin1(it.key().mid(cut + 1).toUtf8().toHex()) + ".png"}); }
  QPointer<PaintSession> self(this);
  std::thread([self, plan, fz, dir, done] {
    const std::vector<char> ok = plan->ok ? paint::Document::saveWrite(*plan) : std::vector<char>();
    const QDir d(dir); QSet<QString> kept; bool fzOk = true; for (const Fz& f : *fz) { if (f.im.save(d.filePath(f.name))) kept.insert(f.name); else fzOk = false; }
    QMetaObject::invokeMethod(qApp, [self, plan, ok, dir, kept, fzOk, done] {
      if (!self) return;
      const bool all = self->doc.saveFinish(*plan, ok) && fzOk; const QDir d(dir);
      if (fzOk) for (const QString& f : d.entryList({"freeze.png", "freeze@*.png", "fmask_*.png"}, QDir::Files)) if (!kept.contains(f)) QFile::remove(d.filePath(f));
      self->saving_ = false; if (done) done(all);
      if (self->saveNext_.on) { SaveAsk a = std::move(self->saveNext_); self->saveNext_ = {}; self->saveFilesAsync(a.path, a.own, std::move(a.done)); } }, Qt::QueuedConnection);
  }).detach();
}

void PaintSession::loadFiles(const QString& projectPath) {
  ui::DevOp devT("paintLoad", 0);
  if (doc.width() > 0 && !projectPath.isEmpty()) doc.loadFrom(sideFolder(projectPath));
  freeze.clear(); freezeKept.clear();
  if (!projectPath.isEmpty()) { const QDir d(sideFolder(projectPath));
    for (const QString& f : d.entryList({"fmask_*.png"}, QDir::Files)) { const QStringList p = f.mid(6, f.size() - 10).split('_'); if (p.size() != 2) continue; QImage m(d.filePath(f)); if (m.isNull()) continue;
      freezeKept[QString::fromUtf8(QByteArray::fromHex(p[0].toLatin1())) + QChar(0x1f) + QString::fromUtf8(QByteArray::fromHex(p[1].toLatin1()))] = m.convertToFormat(QImage::Format_Grayscale8); }
    for (const QString& f : d.entryList({"freeze.png", "freeze@*.png"}, QDir::Files)) { QImage m(d.filePath(f)); if (m.isNull()) continue; const QString k = freezeSetOf(f); freeze[k] = m.convertToFormat(QImage::Format_Grayscale8); } }
  emit pixelsChanged({});
}

// ---- layers from another project ----
namespace {
bool otherDoc(const QString& projectPath, paint::Document& d, QString* error) {
  QFile f(projectPath); if (!f.open(QIODevice::ReadOnly)) { if (error) *error = QObject::tr("The file can't be read"); return false; }
  Scene sc; const LoadResult r = loadProjectData(f.readAll(), projectPath, sc);
  if (!r.ok) { if (error) *error = r.error; return false; }
  const QJsonObject p = sc.fileRaw.value("paint").toObject();
  if (!p.contains("doc")) { if (error) *error = QObject::tr("That project has no Paint layers"); return false; }
  d.fromJson(p.value("doc").toObject()); return true;
}
}  // namespace
std::vector<PaintSession::FileLayer> PaintSession::layersInFile(const QString& projectPath, QString* error) const {
  paint::Document d; if (!otherDoc(projectPath, d, error)) return {};
  std::vector<FileLayer> out;
  std::function<void(int, int)> walk = [&](int parent, int depth) {
    const auto& L = d.layers();
    for (int i = int(L.size()) - 1; i >= 0; i--) { const paint::Layer& l = L[size_t(i)]; if (l.parent != parent) continue;
      const QString kind = l.kind == paint::LayerKind::Group ? tr("group") : l.kind == paint::LayerKind::Adjust ? paint::adjustName(l.adjust)
                         : l.kind == paint::LayerKind::Fill ? tr("fill") : QString(paint::mapInfo(l.channel).name);
      out.push_back({l.id, l.name, kind, depth}); if (l.kind == paint::LayerKind::Group) walk(l.id, depth + 1); } };
  walk(-1, 0); return out;
}
QString PaintSession::takeLayersFrom(const QString& projectPath, const std::vector<int>& ids) {
  if (doc.width() <= 0) return tr("Nothing to paint on yet");
  paint::Document d; QString err; if (!otherDoc(projectPath, d, &err)) return err;
  const QDir side(sideFolder(projectPath));
  // (this project's UV sets: the shown one and the ones kept aside — a layer brings its pixels for each set of the same name and size)
  std::vector<std::pair<QString, QSize>> mine{{doc.activeSet(), QSize(doc.width(), doc.height())}};
  for (const auto& x : doc.sets()) if (x.key != doc.activeSet()) mine.push_back({x.key, QSize(x.w, x.h)});
  std::vector<int> order;                                      // (bottom → top, as they lie there — groups bring what lies in them)
  std::set<int> want(ids.begin(), ids.end());
  for (const paint::Layer& l : d.layers()) { bool in = want.count(l.id) > 0; for (int p = l.parent; !in && p >= 0;) { in = want.count(p) > 0; const paint::Layer* g = d.layer(p); p = g ? g->parent : -1; } if (in) order.push_back(l.id); }
  if (order.empty()) return tr("No layers chosen");
  std::map<int, int> newId; int above = layer, made = 0, withPx = 0;
  for (int oid : order) { const paint::Layer* src = d.layer(oid); if (!src) continue;
    paint::Layer l = *src; l.image = {}; l.mask = {}; l.other.clear();
    const int par = src->parent >= 0 && newId.count(src->parent) ? newId[src->parent] : -1;
    {
      for (const auto& [key, size] : mine) {
        QImage im(side.filePath(paint::Document::layerFile("layer", oid, key))), mk(side.filePath(paint::Document::layerFile("mask", oid, key)));
        if (!im.isNull() && im.size() != size) im = im.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (!mk.isNull() && mk.size() != size) mk = mk.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (!im.isNull()) im = im.convertToFormat(paint::formatOf(l.channel)); if (!mk.isNull()) mk = mk.convertToFormat(QImage::Format_Grayscale8);
        if (im.isNull() && mk.isNull()) continue;
        if (key == doc.activeSet()) { if (l.kind == paint::LayerKind::Paint) l.image = im; l.mask = mk; } else { auto& p = l.other[key]; if (l.kind == paint::LayerKind::Paint) p.image = im; p.mask = mk; }
        withPx++; } }
    const int nid = doc.adopt(std::move(l), par >= 0 ? -1 : above);
    if (par >= 0) if (paint::Layer* x = doc.layer(nid)) x->parent = par;   // (inside a group brought with it)
    newId[oid] = nid; if (par < 0) above = nid; made++; }
  if (newId.count(order.back())) layer = newId[order.back()];
  edits++; emit layersChanged(); emit pixelsChanged({});
  return withPx ? tr("%1 layer%2 brought from %3").arg(made).arg(made == 1 ? "" : "s").arg(QFileInfo(projectPath).fileName())
                : tr("%1 layer%2 brought from %3 — no pixels: its UV sets differ from this project's, or its .paint folder is missing").arg(made).arg(made == 1 ? "" : "s").arg(QFileInfo(projectPath).fileName());
}

}  // namespace hm
