#include "app/PaintPreview.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVector4D>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <thread>

namespace hm {

// ======================= the history of the room's visibility =======================
struct VisHistory::State { Viewport::PvVis v; };

VisHistory::VisHistory(Viewport* vp) : vp_(vp) {
  wait_ = new QTimer; wait_->setInterval(60);
  QObject::connect(wait_, &QTimer::timeout, [this] { if (QApplication::mouseButtons() != Qt::NoButton || batch_ > 0) return; wait_->stop(); commit(); });
}
void VisHistory::reset() { undo_.clear(); redo_.clear(); pending_ = false; before_.reset(); last_ = std::make_shared<State>(State{vp_->pvVisState()}); }
void VisHistory::changed() {
  auto cur = std::make_shared<State>(State{vp_->pvVisState()});
  if (restoring_ || !last_) { last_ = cur; return; }
  if (cur->v == last_->v) return;
  if (!pending_) { before_ = last_; pending_ = true; }
  last_ = cur;
  if (batch_ > 0) return;
  if (QApplication::mouseButtons() != Qt::NoButton) { wait_->start(); return; }   // (a gesture held: one step when it is let go)
  commit();
}
void VisHistory::end() { if (batch_ > 0 && --batch_ == 0 && pending_ && QApplication::mouseButtons() == Qt::NoButton) commit(); else if (batch_ == 0 && pending_) wait_->start(); }
void VisHistory::commit() {
  if (!pending_) return; pending_ = false; wait_->stop();
  if (before_) { undo_.push_back(before_); if (undo_.size() > 300) undo_.erase(undo_.begin()); }
  before_.reset(); redo_.clear();
}
bool VisHistory::undo() {
  commit(); if (undo_.empty()) return false;
  auto s = undo_.back(); undo_.pop_back(); if (last_) redo_.push_back(last_);
  restoring_ = true; vp_->pvVisRestore(s->v); restoring_ = false; last_ = s; return true;
}
bool VisHistory::redo() {
  commit(); if (redo_.empty()) return false;
  auto s = redo_.back(); redo_.pop_back(); if (last_) undo_.push_back(last_);
  restoring_ = true; vp_->pvVisRestore(s->v); restoring_ = false; last_ = s; return true;
}

// ======================= the model preview =======================
namespace {
const QColor kBg(0x1e, 0x1e, 0x20), kSel(0x3d, 0x86, 0xff), kHoverPart(0xff, 0xd8, 0x3a), kHoverFill(0xff, 0x9a, 0x2e), kLock(0xe0, 0xa0, 0x50), kGhost(0x8a, 0x92, 0xa8);
inline QRgb mixC(QRgb a, const QColor& b, double t) { return qRgb(int(qRed(a) + (b.red() - qRed(a)) * t), int(qGreen(a) + (b.green() - qGreen(a)) * t), int(qBlue(a) + (b.blue() - qBlue(a)) * t)); }
double segDist2(const QPointF& p, const QPointF& a, const QPointF& b) {
  const QPointF d = b - a; const double L = d.x() * d.x() + d.y() * d.y(); double t = L > 1e-12 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / L : 0; t = std::clamp(t, 0.0, 1.0);
  const QPointF q = a + d * t; return (p.x() - q.x()) * (p.x() - q.x()) + (p.y() - q.y()) * (p.y() - q.y()); }
}  // namespace

PaintPreview::PaintPreview(Viewport* vp, Scene* sc, VisHistory* hist, QWidget* parent) : QWidget(parent), vp_(vp), sc_(sc), hist_(hist) {
  setMouseTracking(true); setFocusPolicy(Qt::NoFocus); setMinimumSize(120, 120); setAttribute(Qt::WA_OpaquePaintEvent);
  setToolTip(QObject::tr("The model's preview: nothing is painted here — parts are chosen and hidden.\nLMB on a part — select it by the tool on the left (Shift — add, Ctrl — take off; held — painted over); "
                         "on the empty ground — turn the view. RMB — turn, MMB — move, the wheel — zoom, F — the shown framed.\n"
                         "H — hide the selected (none selected: the part under the pointer; a ghost under it — shown again) · Shift+H — only it · Ctrl+H — the object · Ctrl+X — everything shown · Ctrl+D — deselect.\n"
                         "Ctrl+Z over this window — the hiding taken back, not the stroke"));
  followT_ = new QTimer(this); followT_->setInterval(100);
  QObject::connect(followT_, &QTimer::timeout, this, [this] {
    if (!isVisible() || sc_->room != "paint") return;
    if (!follow) return; const Camera& m = vp_->camera(); const Quat q = m.quat;
    if (q.x == followQ_.x && q.y == followQ_.y && q.z == followQ_.z && q.w == followQ_.w) return; followQ_ = q; takeMainView(); });
  followT_->start();
  colT_.start();
}

void PaintPreview::setKind(const QString& k) { kind = k; setHover(hoverAt_); }

bool PaintPreview::pointerOver() const { return isVisible() && rect().contains(mapFromGlobal(QCursor::pos())); }

void PaintPreview::ensureGeometry() {
  QByteArray key = QByteArray::number(sc_->revision) + ":" + QByteArray::number(qulonglong(sc_->items.size()));
  std::vector<int> ids; for (int id : vp_->pvMeshes()) { const Item* it = sc_->item(id); if (!it || !sc_->isVisibleBase(*it)) continue; ids.push_back(id);
    key += "," + QByteArray::number(id) + "/" + QByteArray::number(qulonglong(quintptr(it->poly.get()))) + "/" + QByteArray::number(qulonglong(quintptr(vp_->pvBase(id).get()))); }
  if (key == geoKey_) return;
  geoKey_ = key; meshes_.clear(); pos_.clear(); fStart_.assign(1, 0); fVerts_.clear(); fMesh_.clear(); fNorm_.clear();
  for (int id : ids) { const Item* it = sc_->item(id); const auto B = vp_->pvBase(id); if (!B || B->faceCount() == 0) continue;
    const auto& P = it->poly && it->poly->pos.size() >= B->pos.size() ? it->poly->pos : B->pos;
    Mesh m; m.id = id; m.B = B; m.faceOff = int(fMesh_.size()); m.vertOff = int(pos_.size()); m.nf = B->faceCount();
    for (size_t v = 0; v < B->pos.size(); v++) pos_.push_back({float(P[v].x), float(P[v].y), float(P[v].z)});
    for (int f = 0; f < B->faceCount(); f++) { const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1];
      Vec3 n; for (int k = s; k < e; k++) { const Vec3& a = P[size_t(B->faceVerts[size_t(k)])]; const Vec3& b = P[size_t(B->faceVerts[size_t(k + 1 < e ? k + 1 : s)])]; n = n + Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); }
      const double L = std::sqrt(n.dot(n)); if (L > 1e-30) n = n * (1 / L);
      fNorm_.push_back({float(n.x), float(n.y), float(n.z)});
      for (int k = s; k < e; k++) fVerts_.push_back(m.vertOff + B->faceVerts[size_t(k)]);
      fStart_.push_back(int(fVerts_.size())); fMesh_.push_back(int(meshes_.size())); }
    meshes_.push_back(m); }
  visKey_ = -1; colKey_.clear(); rasterDirty_ = true;
  if (!camSet_ && !meshes_.empty()) fitCam();
}

void PaintPreview::ensureFlags() {
  const qint64 k = vp_->pvKeyNow();
  if (k != visKey_) { visKey_ = k; const size_t N = fMesh_.size(); hid_.assign(N, 0); lock_.assign(N, 0);
    for (const Mesh& m : meshes_) { const auto h = vp_->pvFaceFlags(m.id, false), l = vp_->pvFaceFlags(m.id, true);
      for (int f = 0; f < m.nf; f++) { if (size_t(f) < h.size()) hid_[size_t(m.faceOff + f)] = h[size_t(f)]; if (size_t(f) < l.size()) lock_[size_t(m.faceOff + f)] = l[size_t(f)]; } }
    rasterDirty_ = true; selSeen_ = 0; }
  // (the selection: the room's own — the Select tool's and this view's alike)
  quint64 n = 1469598103934665603ull;                           // (every selected polygon in the key: another selection of the same size is seen as a change)
  for (const auto& [m, fs] : vp_->pvSel) { n = (n ^ quint64(quint32(m))) * 1099511628211ull; for (int x : fs) n = (n ^ quint64(quint32(x))) * 1099511628211ull; n = (n ^ 0xffffffffull) * 1099511628211ull; }
  if (n != selSeen_ || sel_.size() != fMesh_.size()) { selSeen_ = n; sel_.assign(fMesh_.size(), 0);
    for (const Mesh& m : meshes_) { const auto f = vp_->pvSel.find(m.id); if (f == vp_->pvSel.end()) continue; for (int x : f->second) if (x >= 0 && x < m.nf) sel_[size_t(m.faceOff + x)] = 1; }
    compDirty_ = true; }
  if (hov_.size() != fMesh_.size()) hov_.assign(fMesh_.size(), 0);
}

// the painted map's colour on each polygon — made anew when the map changed, not more often than twice a second while it is being painted
QByteArray PaintPreview::colourKey() const {
  const auto& pv = sc_->paintView; QByteArray key = geoKey_ + "|" + pv.set.toUtf8() + ":" + QByteArray::number(pv.rev) + ":" + QByteArray::number(qulonglong(quintptr(pv.flat.constBits())));
  for (const auto& [n, p] : pv.others) key += "|" + n.toUtf8() + ":" + QByteArray::number(p.second);
  return key;
}
void PaintPreview::ensureColours(bool force) {
  // (the preview is for choosing and hiding parts: no texture on it — the model shown gray, the map is not read a second time — the author, 17.11)
  if (!col_.empty()) { col_.clear(); compDirty_ = true; } return; (void)force;
  const QByteArray key = colourKey();
  if (key == colKey_ || (!force && !colKey_.isEmpty() && col_.size() == fMesh_.size())) return;   // (a change while painting: the timer takes it once the stroke is over)
  colKey_ = key; colT_.restart(); col_.assign(fMesh_.size(), 0);
  for (const Mesh& m : meshes_) { const auto c = vp_->pvFaceColours(m.id); for (int f = 0; f < m.nf && size_t(f) < c.size(); f++) col_[size_t(m.faceOff + f)] = c[size_t(f)]; }
  compDirty_ = true;
}

Box3 PaintPreview::shownBox(bool all) const {
  Box3 b; for (size_t f = 0; f < fMesh_.size(); f++) { if (!all && f < hid_.size() && hid_[f]) continue; for (int k = fStart_[f]; k < fStart_[f + 1]; k++) { const auto& p = pos_[size_t(fVerts_[size_t(k)])]; b.expand(Vec3(p[0], p[1], p[2])); } }
  return b;
}
void PaintPreview::fitCam() {
  const Box3 b = shownBox(true); if (b.empty()) return;
  cam_.width = std::max(1, W_); cam_.height = std::max(1, H_); cam_.view = "persp"; cam_.fov = 30; cam_.fitToBox(b); cam_.frame(b, false); camSet_ = true; rasterDirty_ = true;
}
void PaintPreview::frameShown() {
  ensureGeometry(); ensureFlags(); Box3 b = shownBox(false); if (b.empty()) b = shownBox(true); if (b.empty()) return;
  if (!camSet_) fitCam(); cam_.frame(b, true); rasterDirty_ = true; update();
}
void PaintPreview::takeMainView() {
  ensureGeometry(); if (!camSet_) fitCam(); const Camera& m = vp_->camera();
  const double d = std::max(cam_.pivot.distanceTo(cam_.pos), cam_.sceneScale * 0.05);
  cam_.quat = m.quat; cam_.up = m.up; cam_.freeRoll = m.freeRoll; cam_.pos = cam_.pivot - cam_.dir() * d; rasterDirty_ = true; update();
}

void PaintPreview::refresh() { update(); }

// ---------- the picture ----------
void PaintPreview::raster() {
  QElapsedTimer t; t.start();
  const int W = W_, H = H_; cam_.width = W; cam_.height = H;
  const QMatrix4x4 M = cam_.projMatrix() * cam_.viewMatrix();
  scr_.resize(pos_.size());
  for (size_t i = 0; i < pos_.size(); i++) { const auto& p = pos_[i]; const QVector4D c = M * QVector4D(p[0], p[1], p[2], 1.f);
    if (c.w() <= 1e-6f) { scr_[i] = {NAN, 0, 0}; continue; } const float iw = 1.f / c.w();
    scr_[i] = {(c.x() * iw * 0.5f + 0.5f) * W, (0.5f - c.y() * iw * 0.5f) * H, c.z() * iw}; }
  const size_t N = fMesh_.size(); shade_.assign(N, 1.f); const Vec3 vd = cam_.dir();
  for (size_t f = 0; f < N; f++) { const auto& n = fNorm_[f]; const double d = std::abs(n[0] * vd.x + n[1] * vd.y + n[2] * vd.z); shade_[f] = float(0.32 + 0.68 * d); }
  zv_.assign(size_t(W) * H, 2.f); zg_.assign(size_t(W) * H, 2.f); iv_.assign(size_t(W) * H, -1); ig_.assign(size_t(W) * H, -1);
  const bool gh = ghosts;
  auto band = [&](int y0, int y1) {
    for (size_t f = 0; f < N; f++) {
      const bool hidden = hid_[f]; if (hidden && !gh) continue;
      float* Z = hidden ? zg_.data() : zv_.data(); int* I = hidden ? ig_.data() : iv_.data();
      const int s = fStart_[f], e = fStart_[f + 1]; if (e - s < 3) continue;
      const auto& A = scr_[size_t(fVerts_[size_t(s)])]; if (std::isnan(A[0])) continue;
      for (int k = s + 1; k + 1 < e; k++) {
        const auto& B = scr_[size_t(fVerts_[size_t(k)])]; const auto& C = scr_[size_t(fVerts_[size_t(k + 1)])]; if (std::isnan(B[0]) || std::isnan(C[0])) continue;
        const float minx = std::min({A[0], B[0], C[0]}), maxx = std::max({A[0], B[0], C[0]}), miny = std::min({A[1], B[1], C[1]}), maxy = std::max({A[1], B[1], C[1]});
        if (maxx < 0 || minx >= W || maxy < y0 || miny >= y1) continue;
        const int x0 = std::max(0, int(std::floor(minx))), x1 = std::min(W - 1, int(std::ceil(maxx))), ya = std::max(y0, int(std::floor(miny))), yb = std::min(y1 - 1, int(std::ceil(maxy)));
        const float area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
        bool any = false;
        if (std::abs(area) > 1e-8f) { const float ia = 1.f / area;
          for (int y = ya; y <= yb; y++) { const float py = y + 0.5f;
            for (int x = x0; x <= x1; x++) { const float px = x + 0.5f;
              float w0 = ((B[0] - px) * (C[1] - py) - (B[1] - py) * (C[0] - px)) * ia, w1 = ((C[0] - px) * (A[1] - py) - (C[1] - py) * (A[0] - px)) * ia, w2 = 1 - w0 - w1;
              if (w0 < 0 || w1 < 0 || w2 < 0) continue; any = true;
              const float z = w0 * A[2] + w1 * B[2] + w2 * C[2]; if (z < -1 || z > 1) continue;
              const size_t i = size_t(y) * W + x; if (z < Z[i]) { Z[i] = z; I[i] = int(f); } } } }
        if (!any && maxx - minx < 2 && maxy - miny < 2) {     // (a polygon smaller than a pixel: its middle's pixel — a thin lock does not fall apart into dots)
          const int x = int((A[0] + B[0] + C[0]) / 3), y = int((A[1] + B[1] + C[1]) / 3); if (x < 0 || x >= W || y < y0 || y >= y1) continue;
          const float z = (A[2] + B[2] + C[2]) / 3; if (z < -1 || z > 1) continue; const size_t i = size_t(y) * W + x; if (z < Z[i]) { Z[i] = z; I[i] = int(f); } }
      } } };
  const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 8), step = (H + nt - 1) / nt; std::vector<std::thread> ts;
  for (int k = 1; k < nt; k++) if (k * step < H) ts.emplace_back(band, k * step, std::min(H, (k + 1) * step));
  band(0, std::min(H, step)); for (auto& th : ts) th.join();
  rasterDirty_ = false; compDirty_ = true; lastMs_ = double(t.nsecsElapsed()) / 1e6;
}

void PaintPreview::compose() {
  const int W = W_, H = H_; if (img_.size() != QSize(W, H)) img_ = QImage(W, H, QImage::Format_RGB32);
  const QColor hc = kind == "fill" ? kHoverFill : kHoverPart;
  for (int y = 0; y < H; y++) { QRgb* o = reinterpret_cast<QRgb*>(img_.scanLine(y));
    for (int x = 0; x < W; x++) { const size_t i = size_t(y) * W + x; QRgb c = kBg.rgb(); const int id = iv_[i];
      if (id >= 0) { const QRgb base = col_.size() > size_t(id) && col_[size_t(id)] ? col_[size_t(id)] : qRgb(0x9c, 0x9c, 0xa2); const float s = shade_[size_t(id)];
        c = qRgb(int(qRed(base) * s), int(qGreen(base) * s), int(qBlue(base) * s));
        if (lock_[size_t(id)]) c = mixC(c, kLock, 0.28);
        if (sel_[size_t(id)]) c = mixC(c, kSel, 0.6);
        if (hov_[size_t(id)]) c = mixC(c, hc, sel_[size_t(id)] ? 0.3 : 0.55); }   // (a selected part under the pointer: still seen as selected)
      const int g = ig_[i];
      if (g >= 0 && (id < 0 || zg_[i] < zv_[i])) { const bool s = sel_[size_t(g)], h = hov_[size_t(g)];
        c = mixC(c, h ? hc : s ? kSel : kGhost, h || s ? 0.55 : id < 0 ? 0.24 : 0.16); }
      o[x] = c; } }
  compDirty_ = false;
}

void PaintPreview::paintEvent(QPaintEvent*) {
  QPainter p(this); p.fillRect(rect(), kBg);
  if (sc_->room != "paint") return;
  ensureGeometry(); ensureFlags(); ensureColours();
  if (meshes_.empty()) { p.setPen(QColor(0x80, 0x86, 0x8e)); p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, QObject::tr("No mesh in the Paint room")); return; }
  if (rasterDirty_) raster();
  if (compDirty_) compose();
  p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(QRectF(QPointF(0, 0), QSizeF(width(), height())), img_);
  // (the outline being dragged)
  if (drag_ == 4 && shape_.size() > 1) { p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(QColor(0xff, 0xd8, 0x3a), 1.2, Qt::DashLine)); p.setBrush(QColor(0xff, 0xd8, 0x3a, 30));
    if (kind == "rect") p.drawRect(QRectF(press_, last_).normalized()); else p.drawPolygon(shape_); }
  // (the counts)
  int nh = 0, ns = 0; for (char c : hid_) nh += c; for (char c : sel_) ns += c;
  QString t = nh ? QObject::tr("%1 of %2 hidden").arg(nh).arg(hid_.size()) : QObject::tr("%1 polygons, none hidden").arg(hid_.size());
  if (ns) t += QObject::tr(" · %1 selected").arg(ns);
  QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.85); p.setFont(f); p.setPen(QColor(0x9a, 0xa0, 0xa8)); p.drawText(rect().adjusted(6, 0, -6, -4), Qt::AlignLeft | Qt::AlignBottom, t);
  if (follow) { p.setPen(QColor(0x7f, 0xb0, 0xff)); p.drawText(rect().adjusted(6, 4, -6, 0), Qt::AlignRight | Qt::AlignTop, QObject::tr("follows the view")); }
}
void PaintPreview::resizeEvent(QResizeEvent*) {
  dpr_ = devicePixelRatioF(); W_ = std::max(1, int(width() * std::min(dpr_, 1.5))); H_ = std::max(1, int(height() * std::min(dpr_, 1.5)));
  cam_.width = W_; cam_.height = H_; rasterDirty_ = true;
}
void PaintPreview::showEvent(QShowEvent*) { rasterDirty_ = true; if (!camSet_) { ensureGeometry(); if (!meshes_.empty()) fitCam(); } }
QImage PaintPreview::grabShot() { ensureGeometry(); ensureFlags(); ensureColours(true); if (W_ <= 1) { W_ = std::max(1, width()); H_ = std::max(1, height()); } if (rasterDirty_) raster(); compose(); return img_.copy(); }

// ---------- picking ----------
std::optional<Vec3> PaintPreview::pointAt(const QPointF& w) const {   // (the surface's point under the pointer — or the nearest round it, a few pixels away)
  if (W_ <= 0 || zv_.size() != size_t(W_) * H_) return std::nullopt; const double k = double(W_) / std::max(1, width()); const int cx = int(w.x() * k), cy = int(w.y() * k);
  for (int r = 0; r <= 12; r++) for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
    if (std::max(std::abs(dx), std::abs(dy)) != r) continue; const int x = cx + dx, y = cy + dy; if (x < 0 || y < 0 || x >= W_ || y >= H_) continue; const size_t i = size_t(y) * W_ + x;
    if (iv_[i] < 0) continue;
    Camera c = cam_; c.width = W_; c.height = H_; bool inv = false; const QMatrix4x4 Mi = (c.projMatrix() * c.viewMatrix()).inverted(&inv); if (!inv) return std::nullopt;
    const QVector4D q = Mi * QVector4D(float((x + 0.5) / W_ * 2 - 1), float(1 - (y + 0.5) / H_ * 2), zv_[i], 1.f); if (std::abs(q.w()) < 1e-9f) return std::nullopt;
    return Vec3{q.x() / q.w(), q.y() / q.w(), q.z() / q.w()}; }
  return std::nullopt;
}
int PaintPreview::pickAt(const QPointF& w, bool* ghost) const {
  if (W_ <= 0 || iv_.size() != size_t(W_) * H_) return -1; const double k = double(W_) / std::max(1, width());
  const int cx = int(w.x() * k), cy = int(w.y() * k);
  for (int r = 0; r <= 3; r++) for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {   // (the nearest polygon within a few pixels: a thin lock is hit)
    if (std::max(std::abs(dx), std::abs(dy)) != r) continue; const int x = cx + dx, y = cy + dy; if (x < 0 || y < 0 || x >= W_ || y >= H_) continue; const size_t i = size_t(y) * W_ + x;
    if (iv_[i] >= 0) { if (ghost) *ghost = false; return iv_[i]; } }
  for (int r = 0; r <= 3; r++) for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {   // (none shown there: a ghost)
    if (std::max(std::abs(dx), std::abs(dy)) != r) continue; const int x = cx + dx, y = cy + dy; if (x < 0 || y < 0 || x >= W_ || y >= H_) continue; const size_t i = size_t(y) * W_ + x;
    if (ig_[i] >= 0) { if (ghost) *ghost = true; return ig_[i]; } }
  return -1;
}
std::vector<int> PaintPreview::walkAt(const QPointF& w, int g, const QString& kd) const {
  if (g < 0 || size_t(g) >= fMesh_.size()) return {}; const Mesh& m = meshes_[size_t(fMesh_[size_t(g)])]; const int bf = g - m.faceOff;
  std::vector<int> local;
  if (kd == "loop" || kd == "ring") {                            // (across the side of the polygon nearest the pointer — this view's camera)
    const int s = fStart_[size_t(g)], e = fStart_[size_t(g) + 1], n = e - s; const double k = double(W_) / std::max(1, width()); const QPointF p = w * k;
    std::vector<double> d(static_cast<size_t>(n)); for (int c = 0; c < n; c++) { const auto& a = scr_[size_t(fVerts_[size_t(s + c)])]; const auto& b = scr_[size_t(fVerts_[size_t(s + (c + 1) % n)])]; d[size_t(c)] = segDist2(p, QPointF(a[0], a[1]), QPointF(b[0], b[1])); }
    int best = 0; for (int c = 1; c < n; c++) if (d[size_t(c)] < d[size_t(best)]) best = c;
    if (n == 4) { for (double& x : d) x = std::sqrt(x); const double wa = d[0] + d[2], wb = d[1] + d[3], ra = std::min(d[0], d[2]) / std::max(1e-9, wa), rb = std::min(d[1], d[3]) / std::max(1e-9, wb);
      const bool pairA = std::abs(ra - rb) < 0.12 ? wa > wb : ra < rb; best = pairA ? (d[0] <= d[2] ? 0 : 2) : (d[1] <= d[3] ? 1 : 3); }
    local = vp_->pvLoopAt(m.id, bf, best, kd == "ring"); }
  else if (kd == "fill" || kd == "part" || kd == "group") local = vp_->pvWalkAt(QPointF(), m.id, bf, kd);
  else local = {bf};
  std::vector<int> out; out.reserve(local.size()); for (int f : local) if (f >= 0 && f < m.nf) out.push_back(m.faceOff + f); return out;
}
std::map<int, std::vector<int>> PaintPreview::toMeshes(const std::vector<int>& g) const {
  std::map<int, std::vector<int>> out; for (int x : g) if (x >= 0 && size_t(x) < fMesh_.size()) { const Mesh& m = meshes_[size_t(fMesh_[size_t(x)])]; out[m.id].push_back(x - m.faceOff); } return out;
}
void PaintPreview::setHover(const QPointF& w) {
  hoverAt_ = w; bool gh = false; const int g = (drag_ == 0 || drag_ == 3) && rect().contains(w.toPoint()) ? pickAt(w, &gh) : -1;
  std::vector<char> h(fMesh_.size(), 0);
  if (g >= 0 && kind != "rect" && kind != "lasso") for (int x : walkAt(w, g, kind)) h[size_t(x)] = 1;
  hoverFace_ = g; hoverGhost_ = gh;
  if (h != hov_) { hov_ = std::move(h); compDirty_ = true; update(); }
}

// ---------- the mouse ----------
void PaintPreview::press(const QPointF& w, Qt::MouseButton b, Qt::KeyboardModifiers mods) {
  if (drag_ != 0) return;                                        // (a second button while one drag is held: the drag held goes on, the next release ends it and its history batch)
  ensureGeometry(); ensureFlags(); if (rasterDirty_) raster();
  press_ = last_ = w; pressMods_ = mods; moved_ = false; lastPick_ = -1;
  if (const auto p = pointAt(w)) cam_.pivot = *p;               // (the view turns round the point under the pointer — as the main view, any press)
  if (b == Qt::MiddleButton) { drag_ = 2; return; }
  if (b == Qt::RightButton) { drag_ = (mods & Qt::AltModifier) ? 5 : 1; return; }
  if (b != Qt::LeftButton) return;
  if (mods & Qt::AltModifier) { drag_ = 1; return; }
  selOp_ = (mods & Qt::ControlModifier) ? 2 : (mods & Qt::ShiftModifier) ? 1 : 0;
  if (kind == "rect" || kind == "lasso") { drag_ = 4; shape_.clear(); shape_ << w; return; }
  bool gh = false; const int g = pickAt(w, &gh);
  if (g < 0) { drag_ = 1; return; }                              // (the empty ground: the view turned)
  drag_ = 3; hist_->begin(); lastPick_ = g;
  vp_->pvSelSet(toMeshes(walkAt(w, g, kind)), selOp_);
}
void PaintPreview::move(const QPointF& w, Qt::MouseButtons, Qt::KeyboardModifiers) {
  const QPointF d = w - last_; if (!moved_ && std::hypot(w.x() - press_.x(), w.y() - press_.y()) > 3) moved_ = true;
  const double k = double(W_) / std::max(1, width());
  if (drag_ == 1) { cam_.orbit(d.x() * 1.4, d.y() * 1.4); rasterDirty_ = true; if (follow) { follow = false; if (stateChanged) stateChanged(); } update(); }
  else if (drag_ == 2) { cam_.pan(d.x() * k, d.y() * k); rasterDirty_ = true; update(); }
  else if (drag_ == 5) { cam_.dolly(-d.y() * 2); rasterDirty_ = true; update(); }
  else if (drag_ == 3) { const int g = pickAt(w); if (g >= 0 && g != lastPick_) { lastPick_ = g; ensureFlags();   // (painted over: each polygon passed takes the tool's walk — not again for one already so)
      const bool isSel = size_t(g) < sel_.size() && sel_[size_t(g)];
      if (selOp_ == 2 ? isSel : !isSel) vp_->pvSelSet(toMeshes(walkAt(w, g, kind)), selOp_ == 2 ? 2 : 1); } }
  else if (drag_ == 4) { if (kind == "lasso") shape_ << w; update(); }
  last_ = w;
}
void PaintPreview::release(const QPointF& w, Qt::MouseButton) {
  const int was = drag_; drag_ = 0;
  if (was == 3) hist_->end();
  else if (was == 1 && !moved_ && !(pressMods_ & (Qt::AltModifier | Qt::ShiftModifier | Qt::ControlModifier)) && QApplication::mouseButtons() == Qt::NoButton && pickAt(w) < 0) vp_->pvSelClear();   // (a click on the empty ground: nothing selected)
  else if (was == 4) {                                           // (what is seen inside the outline)
    QPolygonF poly = kind == "rect" ? QPolygonF(QRectF(press_, w).normalized()) : shape_; const double k = double(W_) / std::max(1, width());
    QTransform T; T.scale(k, k); poly = T.map(poly); const QRect bb = poly.boundingRect().toAlignedRect().intersected(QRect(0, 0, W_, H_));
    std::set<int> got; QPainterPath path; path.addPolygon(poly);
    if (bb.width() * bb.height() > 4 && iv_.size() >= size_t(W_) * size_t(H_)) for (int y = bb.top(); y <= bb.bottom(); y++) for (int x = bb.left(); x <= bb.right(); x++) { const int id = iv_[size_t(y) * W_ + x]; if (id >= 0 && !got.count(id) && path.contains(QPointF(x + 0.5, y + 0.5))) got.insert(id); }
    shape_.clear();
    if (!got.empty()) vp_->pvSelSet(toMeshes(std::vector<int>(got.begin(), got.end())), selOp_);
    else if (selOp_ == 0) vp_->pvSelClear(); }
  setHover(w); update();
}
bool PaintPreview::test(int phase, const QPointF& at, Qt::KeyboardModifiers mods, Qt::MouseButton b) {
  if (phase == 0) press(at, b, mods); else if (phase == 1) { move(at, b, mods); setHover(at); } else if (phase == 2) { release(at, b); } else if (phase == 3) setHover(at);
  return true;
}
void PaintPreview::mousePressEvent(QMouseEvent* e) { press(e->position(), e->button(), e->modifiers()); }
void PaintPreview::mouseMoveEvent(QMouseEvent* e) { if (drag_) move(e->position(), e->buttons(), e->modifiers()); else setHover(e->position()); }
void PaintPreview::mouseReleaseEvent(QMouseEvent* e) { release(e->position(), e->button()); }
void PaintPreview::wheelEvent(QWheelEvent* e) {
  const double k = double(W_) / std::max(1, width()); const QPointF p = e->position() * k;
  cam_.wheel(p.x(), p.y(), e->angleDelta().y() / 120.0); rasterDirty_ = true; update(); e->accept();
}
void PaintPreview::leaveEvent(QEvent*) { if (drag_) return; hoverFace_ = -1; if (std::any_of(hov_.begin(), hov_.end(), [](char c) { return c != 0; })) { std::fill(hov_.begin(), hov_.end(), char(0)); compDirty_ = true; update(); } }

// ---------- the keys ----------
bool PaintPreview::key(const QString& op) { if (!pointerOver()) return false; return keyAt(op, mapFromGlobal(QCursor::pos())); }
bool PaintPreview::keyAt(const QString& op, const QPointF& w) {
  ensureGeometry(); ensureFlags(); if (rasterDirty_) raster();
  if (op == "showAll") { vp_->pvKey("showAll"); return true; }
  if (!vp_->pvSel.empty()) { vp_->pvKey(op); return true; }      // (the selected: as over the model)
  bool gh = false; const int g = pickAt(w, &gh);
  if (g < 0) { ui::toast(QObject::tr("Point at a part of the preview — or select polygons first")); return true; }
  const Mesh& m = meshes_[size_t(fMesh_[size_t(g)])];
  std::vector<int> part; QString o = op;
  if (op == "hideObj" || op == "onlyObj") { for (int f = 0; f < m.nf; f++) part.push_back(m.faceOff + f); o = op == "hideObj" ? "hideObj" : "onlyObj"; }
  else part = walkAt(w, g, kind == "loop" || kind == "ring" || kind == "fill" ? kind : QString("part"));
  if (gh && op == "hide") o = "show";                            // (a ghost under the pointer: H brings it back)
  hist_->begin(); vp_->pvSelSet(toMeshes(part), 0); vp_->pvKey(o); hist_->end();
  return true;
}

QString PaintPreview::status() const {
  int nh = 0, ns = 0; for (char c : hid_) nh += c; for (char c : sel_) ns += c;
  return QString("faces %1 hidden %2 selected %3 hover %4 kind %5 ms %6 follow %7 size %8x%9").arg(hid_.size()).arg(nh).arg(ns).arg(std::count(hov_.begin(), hov_.end(), char(1))).arg(kind).arg(lastMs_, 0, 'f', 1).arg(follow ? 1 : 0).arg(width()).arg(height());
}

}  // namespace hm
