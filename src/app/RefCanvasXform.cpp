// The reference canvas's ⬚ Transform / ✥ Move (the floating piece of the active layer) and ◧→◨ symmetric copy (RefCanvas.h).
#include "app/PaintCanvas.h"
#include "app/PaintSession.h"
#include "app/RefCanvas.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "paint/PaintDoc.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QKeyEvent>
#include <QScrollArea>
#include <QFrame>
#include <QScreen>
#include <QTimer>
#include <QFileInfo>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <cmath>

namespace hm {

using namespace paint;

QTransform RefCanvas::flT() const {
  QTransform T; T.translate(fl_.cx, fl_.cy); T.rotate(fl_.a); T.scale(fl_.sx, fl_.sy); T.translate(-fl_.img.width() / 2.0, -fl_.img.height() / 2.0); return T;
}

// the active layer's part r cut out into the floating piece (a colour paint layer only); an undo step of the canvas taken first
bool RefCanvas::floatRect(const QRect& r0) {
  Layer* l = s_->active();
  if (!l || l->kind != LayerKind::Paint || l->channel != Channel::Color || l->image.isNull()) { ui::toast(tr("Select a colour paint layer")); return false; }
  if (l->locked) { ui::toast(tr("The layer is locked")); return false; }
  const QRect r = r0.intersected(l->image.rect()); if (r.width() < 2 || r.height() < 2) return false;
  s_->pushUndo({l->id, false, l->image.copy()});
  fl_ = {}; fl_.on = true; fl_.layer = l->id; fl_.img = l->image.copy(r); fl_.cx = r.x() + r.width() / 2.0; fl_.cy = r.y() + r.height() / 2.0;
  { QPainter p(&l->image); p.setCompositionMode(QPainter::CompositionMode_Clear); p.fillRect(r, Qt::transparent); }
  flBegin(); emit s_->pixelsChanged(r); return true;
}
// a pasted picture, kept whole: lifted off its layer again as it was placed
bool RefCanvas::placedValid(int layer) const { auto f = placed_.find(layer); const Layer* l = s_->doc.layer(layer); return f != placed_.end() && l && l->image.cacheKey() == f->second.key; }   // (the layer painted on since: its pixels are what counts now)
bool RefCanvas::floatPlaced() {
  Layer* l = s_->active(); if (!l || !placedValid(l->id) || l->locked) return false; const Placed& pl = placed_[l->id];
  s_->pushUndo({l->id, false, l->image.copy()});
  { Placed was = pl; was.key = s_->undo.back().img.cacheKey(); placedByKey_[was.key] = was; }   // (Ctrl+Z after it is put down: the picture as it was placed before)
  fl_ = {}; fl_.on = true; fl_.keep = true; fl_.layer = l->id; fl_.img = pl.src; fl_.cx = pl.cx; fl_.cy = pl.cy; fl_.sx = pl.sx; fl_.sy = pl.sy; fl_.a = pl.a;
  l->image.fill(Qt::transparent); flBegin(); emit s_->pixelsChanged({}); return true;
}
void RefCanvas::applyFloat() {
  if (!fl_.on) return; fl_.on = false; const bool keep = fl_.pasted || fl_.keep; fl_.pasted = false; fl_.keep = false;
  if (Layer* l = s_->doc.layer(fl_.layer); l && !l->image.isNull()) { { QPainter p(&l->image); p.setRenderHint(QPainter::SmoothPixmapTransform); p.setTransform(flT()); p.drawImage(0, 0, fl_.img); }
    if (keep) { placed_[l->id] = Placed{fl_.img, fl_.cx, fl_.cy, fl_.sx, fl_.sy, fl_.a, l->image.cacheKey()}; placedByKey_[l->image.cacheKey()] = placed_[l->id]; } }
  flHist_.clear(); flRedo_.clear();
  fl_.img = {}; emit s_->pixelsChanged({}); canvas_->update();
}
void RefCanvas::cancelFloat() {
  if (!fl_.on) return; fl_.on = false;
  if (fl_.pasted) { if (!s_->undo.empty() && s_->undo.back().layer == fl_.layer) s_->undo.pop_back(); s_->doc.remove(fl_.layer); if (s_->doc.layer(s_->layer) == nullptr && !s_->doc.layers().empty()) s_->layer = s_->doc.layers().back().id;
    fl_ = {}; emit s_->layersChanged(); emit s_->activeChanged(); emit s_->pixelsChanged({}); canvas_->update(); return; }
  if (!s_->undo.empty() && s_->undo.back().layer == fl_.layer && !s_->undo.back().img.isNull()) { if (Layer* l = s_->doc.layer(fl_.layer)) { l->image = s_->undo.back().img; if (fl_.keep) if (auto pf = placed_.find(l->id); pf != placed_.end()) pf->second.key = l->image.cacheKey(); } s_->undo.pop_back(); }
  fl_.img = {}; emit s_->pixelsChanged({}); canvas_->update();
}

bool RefCanvas::xformInput(int phase, const QPointF& t, Qt::KeyboardModifiers mods) {
  const double z = std::max(1e-6, canvas_->zoom()), hs = 8 / z;
  if (phase == 0) {
    xmode_ = 0; xp0_ = t;
    if (!fl_.on) if (const Layer* al = s_->active(); al && placedValid(al->id)) {   // (a pasted picture: lifted off again from its original — when it is what was clicked, or Move)
      const Placed& pl = placed_.at(al->id); QTransform T; T.translate(pl.cx, pl.cy); T.rotate(pl.a); T.scale(pl.sx, pl.sy); T.translate(-pl.src.width() / 2.0, -pl.src.height() / 2.0);
      if (tool_ == "move" || T.map(QPolygonF(QRectF(pl.src.rect()))).containsPoint(t, Qt::OddEvenFill)) floatPlaced(); }
    if (fl_.on) {
      const QTransform T = flT(); const double w = fl_.img.width(), h = fl_.img.height();
      const QPointF C[4] = {T.map(QPointF(0, 0)), T.map(QPointF(w, 0)), T.map(QPointF(w, h)), T.map(QPointF(0, h))};
      const QPointF topMid = T.map(QPointF(w / 2, 0)), up = topMid - T.map(QPointF(w / 2, h / 2)); const double ul = std::hypot(up.x(), up.y());
      const QPointF rot = topMid + (ul > 1e-9 ? up / ul : QPointF(0, -1)) * (26 / z);
      if (tool_ == "select" && std::hypot(t.x() - rot.x(), t.y() - rot.y()) < hs * 1.3) xmode_ = 4;
      else if (tool_ == "select") { for (int k = 0; k < 4; k++) if (std::hypot(t.x() - C[k].x(), t.y() - C[k].y()) < hs) { xmode_ = 3; xk_ = k; break; } }
      if (!xmode_ && QPolygonF({C[0], C[1], C[2], C[3]}).containsPoint(t, Qt::OddEvenFill)) xmode_ = 2;
      if (!xmode_) applyFloat();                              // (outside: put down — then a new frame / a new drag)
      else flPush();                                          // (this drag: one step of the transform's own undo)
    }
    if (!xmode_ && !fl_.on && hasSel()) {                     // (something selected: Move lifts it wherever pressed, Transform — pressed inside it)
      const QPoint q(int(std::floor(t.x())), int(std::floor(t.y()))); const bool inside = q.x() >= 0 && q.y() >= 0 && q.x() < selMask_.width() && q.y() < selMask_.height() && selMask_.constScanLine(q.y())[q.x()] >= 128;
      if ((tool_ == "move" || inside) && floatSel()) xmode_ = 2;
    }
    if (!xmode_) {
      if (tool_ == "move") { Layer* l = s_->active(); if (l && floatRect(l->image.rect())) xmode_ = 2; }
      else { selecting_ = true; selRect_ = QRectF(t, t); }
    }
    fl0_ = fl_; canvas_->update(); return true;
  }
  if (selecting_) {
    selRect_ = QRectF(xp0_, t).normalized();
    if (phase == 2) { selecting_ = false; floatRect(selRect_.toAlignedRect()); }
    canvas_->update(); return true;
  }
  const QPointF d = t - xp0_;
  if (xmode_ == 2) { fl_.cx = fl0_.cx + d.x(); fl_.cy = fl0_.cy + d.y(); }
  else if (xmode_ == 3) {                                     // (a corner: its distance from the middle along the piece's own axes)
    const double ar = -fl0_.a * M_PI / 180, ca = std::cos(ar), sa = std::sin(ar);
    auto local = [&](const QPointF& p) { const double x = p.x() - fl0_.cx, y = p.y() - fl0_.cy; return QPointF(x * ca - y * sa, x * sa + y * ca); };
    const QPointF a = local(xp0_), b = local(t);
    double kx = std::abs(a.x()) > 1e-6 ? b.x() / a.x() : 1, ky = std::abs(a.y()) > 1e-6 ? b.y() / a.y() : 1;
    if (mods & Qt::ShiftModifier) { const double k = std::abs(kx) > std::abs(ky) ? kx : ky; kx = ky = k; }
    fl_.sx = fl0_.sx * kx; fl_.sy = fl0_.sy * ky;
    if (std::abs(fl_.sx) < 1e-3) fl_.sx = 1e-3;
    if (std::abs(fl_.sy) < 1e-3) fl_.sy = 1e-3;
  } else if (xmode_ == 4) {
    double a = fl0_.a + (std::atan2(t.y() - fl0_.cy, t.x() - fl0_.cx) - std::atan2(xp0_.y() - fl0_.cy, xp0_.x() - fl0_.cx)) * 180 / M_PI;
    if (mods & Qt::ShiftModifier) a = std::round(a / 15) * 15;
    fl_.a = a;
  }
  if (phase == 2) { if (tool_ == "move") applyFloat(); xmode_ = 0; }
  canvas_->update(); return true;
}

void RefCanvas::drawFloat(QPainter& p, const QRectF&) {
  auto W = [&](const QPointF& t) { return canvas_->texToWidget(t); };
  if (selecting_) { p.setPen(QPen(QColor(255, 255, 255, 220), 1, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRect(QRectF(W(selRect_.topLeft()), W(selRect_.bottomRight()))); }
  if (!fl_.on) return;
  p.save(); p.setTransform(flT() * canvas_->mapping()); p.setRenderHint(QPainter::SmoothPixmapTransform); { const RefPlane* rp = plane(); p.setOpacity(rp && rp->sheet ? rp->opacity : 1); } p.drawImage(0, 0, fl_.img); p.restore();
  if (tool_ != "select") return;
  const QTransform T = flT(); const double w = fl_.img.width(), h = fl_.img.height();
  const QPointF C[4] = {W(T.map(QPointF(0, 0))), W(T.map(QPointF(w, 0))), W(T.map(QPointF(w, h))), W(T.map(QPointF(0, h)))};
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(0, 0, 0, 160), 3)); p.setBrush(Qt::NoBrush); p.drawPolygon(C, 4);
  p.setPen(QPen(QColor(110, 170, 255), 1, Qt::DashLine)); p.drawPolygon(C, 4);
  p.setPen(QPen(Qt::black, 1)); p.setBrush(QColor(110, 170, 255));
  for (const QPointF& c : C) p.drawRect(QRectF(c - QPointF(4, 4), QSizeF(8, 8)));
  const QPointF tm = (C[0] + C[1]) / 2, up = tm - (C[0] + C[2]) / 2; const double ul = std::hypot(up.x(), up.y());
  const QPointF rot = tm + (ul > 1e-9 ? up / ul : QPointF(0, -1)) * 26;
  p.setPen(QPen(QColor(110, 170, 255), 1)); p.drawLine(tm, rot); p.setPen(QPen(Qt::black, 1)); p.drawEllipse(rot, 5, 5);
}

void RefCanvas::pasteClipboard() {
  const QMimeData* md = QApplication::clipboard()->mimeData(); QImage img;
  if (md && md->hasImage()) img = qvariant_cast<QImage>(md->imageData());
  if (img.isNull() && md && md->hasUrls()) for (const QUrl& u : md->urls()) if (u.isLocalFile() && img.load(u.toLocalFile())) break;
  if (img.isNull()) { ui::toast(tr("No picture in the clipboard")); return; }
  if (pasteImage(img)) ui::toast(tr("Pasted onto a new layer — move / scale / turn it, Enter puts it down, Esc removes it"));
}
bool RefCanvas::pasteImage(const QImage& src, std::optional<QPointF> at) {
  if (s_->doc.width() == 0 || src.isNull()) return false;
  if (fl_.on) applyFloat();
  setTool("select");
  const int id = s_->doc.addPaint(Channel::Color, tr("Pasted"), s_->layer); s_->layer = id;
  Layer* l = s_->doc.layer(id); if (!l) return false;
  s_->pushUndo({id, false, l->image.copy()});
  fl_ = {}; fl_.on = true; fl_.pasted = true; fl_.layer = id; fl_.img = src.convertToFormat(QImage::Format_ARGB32);
  // (in the middle of what is seen, no bigger than that — the handles in reach; bigger than the canvas: fitted)
  const QRectF seen = canvas_->mapping().inverted().mapRect(QRectF(0, top_->height(), width() - (side_->isVisible() ? side_->width() : 0), height() - top_->height())).intersected(QRectF(0, 0, s_->doc.width(), s_->doc.height()));
  const QPointF mid = seen.isEmpty() ? QPointF(s_->doc.width() / 2.0, s_->doc.height() / 2.0) : seen.center(); fl_.cx = at ? at->x() : mid.x(); fl_.cy = at ? at->y() : mid.y();
  const double k = std::min({1.0, 0.9 * s_->doc.width() / src.width(), 0.9 * s_->doc.height() / src.height(), seen.isEmpty() ? 1.0 : 0.8 * seen.width() / src.width(), seen.isEmpty() ? 1.0 : 0.8 * seen.height() / src.height()}); fl_.sx = fl_.sy = k;
  flBegin();
  emit s_->layersChanged(); emit s_->activeChanged(); canvas_->update(); return true;
}

void RefCanvas::flBegin() { flHist_.clear(); flRedo_.clear(); flStart_ = fl_; }
void RefCanvas::flPush() { if (!fl_.on) return; flHist_.push_back(fl_); if (flHist_.size() > 60) flHist_.erase(flHist_.begin()); flRedo_.clear(); }
// Ctrl+Z / Ctrl+Shift+Z: a floating piece — its own steps (all undone: the piece put back where it was, as it was); else the canvas's strokes
bool RefCanvas::xformUndo(bool redo) {
  if (fl_.on) {
    if (!redo) { if (flHist_.empty()) { cancelFloat(); ui::toast(tr("Transform cancelled")); return true; } flRedo_.push_back(fl_); fl_ = flHist_.back(); flHist_.pop_back(); }
    else { if (flRedo_.empty()) { ui::toast(tr("Nothing to redo in the transform")); return true; } flHist_.push_back(fl_); fl_ = flRedo_.back(); flRedo_.pop_back(); }
    canvas_->update(); return true;
  }
  const bool ok = redo ? s_->redoStroke() : s_->undoStroke();
  if (!ok) ui::toast(redo ? tr("Nothing to redo in the canvas") : tr("Nothing to undo in the canvas")); else restorePlaced();
  return true;
}
// (a pasted picture's layer back to a state it had: the picture is whole and movable again, as it was then)
void RefCanvas::restorePlaced() {
  for (const Layer& l : s_->doc.layers()) if (auto f = placedByKey_.find(l.image.cacheKey()); f != placedByKey_.end()) placed_[l.id] = f->second;
}
// the piece the transform's buttons act on: the floating one; else the active layer's pasted picture, what is selected, or the whole layer
bool RefCanvas::ensureFloat() {
  if (fl_.on) return true;
  Layer* l = s_->active(); if (!l) { ui::toast(tr("No layer chosen")); return false; }
  if (placedValid(l->id)) return floatPlaced();
  if (hasSel()) return floatSel();
  return floatRect(l->image.rect());
}
// flipped across (left ↔ right on the screen) or along (top ↔ bottom) — the piece's turn mirrored with it
void RefCanvas::flipFloat(bool horizontal) {
  if (tool_ != "select") setTool("select");
  if (!ensureFloat()) return; flPush();
  if (horizontal) fl_.sx = -fl_.sx; else fl_.sy = -fl_.sy; fl_.a = -fl_.a;
  canvas_->update(); ui::toast(tr("Flipped — Enter puts it down, Ctrl+Z steps back"));
}
// back as it was: no turn, no flip, its own size (a pasted picture: fitted into the canvas as when pasted), where it was lifted from
void RefCanvas::resetFloat() {
  if (tool_ != "select") setTool("select");
  if (!ensureFloat()) return; flPush();
  double k = 1; if ((fl_.keep || fl_.pasted) && !fl_.img.isNull()) k = std::min({1.0, 0.9 * s_->doc.width() / fl_.img.width(), 0.9 * s_->doc.height() / fl_.img.height()});
  fl_.sx = fl_.sy = k; fl_.a = 0; fl_.cx = flStart_.cx; fl_.cy = flStart_.cy;
  canvas_->update(); ui::toast(tr("Back as it was — Enter puts it down"));
}

// X = 0 of the world on this canvas: its column (none when the canvas's left / right is not the world's X — a side view)
std::optional<double> RefCanvas::mirrorX() const {
  if (s_->doc.width() == 0) return std::nullopt;
  auto c = toWorld(QPointF(s_->doc.width() / 2.0, s_->doc.height() / 2.0)); RefPlane* rp = plane(); if (!c || !rp) return std::nullopt;
  const double d = std::max(1e-6, std::abs(rp->scl.x) * 0.1);
  auto a = toTex(Vec3(0, c->y, c->z)), b = toTex(Vec3(d, c->y, c->z)); if (!a || !b) return std::nullopt;
  if (std::abs(b->x() - a->x()) < s_->doc.width() * 0.05 || std::abs(b->y() - a->y()) > std::abs(b->x() - a->x()) * 0.05) return std::nullopt;
  return a->x();
}
// one half of the active layer mirrored onto the other across X = 0 (pixel x → 2·x0 − 1 − x)
void RefCanvas::mirrorCopy(bool leftToRight) {
  if (fl_.on) applyFloat();
  auto x0 = mirrorX(); if (!x0) { ui::toast(tr("Symmetric copy works on a front, back or top canvas (left / right is X there)")); return; }
  Layer* l = s_->active();
  if (!l || l->kind != LayerKind::Paint || l->channel != Channel::Color || l->image.isNull()) { ui::toast(tr("Select a colour paint layer")); return; }
  if (l->locked) { ui::toast(tr("The layer is locked")); return; }
  s_->pushUndo({l->id, false, l->image.copy()});
  const int W = l->image.width(), H = l->image.height(); const QImage f = l->image.mirrored(true, false);
  { QPainter p(&l->image); p.setClipRect(leftToRight ? QRectF(*x0, 0, W - *x0, H) : QRectF(0, 0, *x0, H)); p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(QPointF(2 * *x0 - W, 0), f); }
  emit s_->pixelsChanged({});
  ui::toast(leftToRight ? tr("The left half mirrored onto the right") : tr("The right half mirrored onto the left"));
}

}  // namespace hm

namespace hm {

// refcanvas open | tool <name> | stroke x,y x,y … [ctrl] [shift] (canvas texture px) | mirror l|r | key enter|esc | shot <png> | info
QString RefCanvas::testCmd(const QStringList& w) {
  const QString op = w.value(1);
  auto settle = [](int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); };
  if (op == "open") { int id = w.size() > 2 ? sc_->refPlanes[w[2].toInt()].id : sc_->refPlanes.empty() ? -1 : sc_->refPlanes.back().id; openFor(id); settle(200); return QString("open plane %1 canvas %2: %3×%4 modal %5").arg(id).arg(cid_).arg(s_->doc.width()).arg(s_->doc.height()).arg(QApplication::activeModalWidget() == this); }
  if (op == "new") { newSheet(w.value(2)); settle(300); const QScreen* sc = vpBox_->screen(); return QString("new canvas %1: %2×%3 screen %4×%5 dpr %6").arg(cid_).arg(s_->doc.width()).arg(s_->doc.height()).arg(sc->size().width()).arg(sc->size().height()).arg(sc->devicePixelRatio()); }
  if (op == "dump") { QString r; for (auto& l : s_->doc.layers()) { l.image.save(w.value(2) + "_" + QString::number(l.id) + ".png"); r += QString(" %1:%2x%3").arg(l.name).arg(l.image.width()).arg(l.image.height()); } auto d = sc_->refSketches.find(cid_); if (d != sc_->refSketches.end()) d->second.flat.save(w.value(2) + "_flat.png"); s_->doc.compose(Channel::Color).save(w.value(2) + "_comp.png"); return "dump" + r; }
  if (op == "leave") { leave(); settle(200); return "left"; }
  if (op == "insert") { QImage im(w.value(2)); return pasteImage(im) ? QString("inserted %1×%2").arg(im.width()).arg(im.height()) : QString("insert FAILED"); }
  if (op == "link" || op == "copy") {                          // refcanvas link|copy <plane index> <view>: the plane on another view — the same canvas / a separate copy of it
    const RefPlane src = sc_->refPlanes[w.value(2).toInt()]; RefPlane t = refs::copyTo(src, w.value(3), *sc_, vp_->refHomeCamera(src), sc_->nextRefId++, refs::viewLabel(w.value(3)) + " t");
    if (op == "copy") t.canvas = refs::copyCanvas(*sc_, src.canvas); sc_->refPlanes.push_back(t); vp_->refPlanesEdited(true); return QString("%1: plane %2 canvas %3 (users %4)").arg(op).arg(t.id).arg(t.canvas).arg(refs::canvasUsers(*sc_, t.canvas)); }
  if (op == "state") { QString o; for (auto& r : sc_->refPlanes) o += QString(" [plane %1 %2 canvas %3 sheet %4]").arg(r.id).arg(r.view).arg(r.canvas).arg(r.sheet); for (auto& [id, k] : sc_->refSketches) { paint::Document d; d.unpackJson(k.data); o += QString(" {canvas %1 '%2' %3×%4 layers:").arg(id).arg(k.name).arg(d.width()).arg(d.height()); for (auto& l : d.layers()) o += " " + l.name; o += QString(" placed %1}").arg(k.data.value("placed").toArray().size()); } return "state" + o; }
  if (op == "screen") { settle(500); QImage im = vp_->grab(); if (vp_->colorFormat() == VK_FORMAT_B8G8R8A8_SRGB) im = im.rgbSwapped();   // (the viewport's frame with this window over it: what is on the screen)
    if (isVisible()) { QPainter pa(&im); pa.setRenderHint(QPainter::SmoothPixmapTransform); pa.drawPixmap(im.rect(), grab()); }
    return im.save(w.value(2)) ? "screen " + w.value(2) : QString("screen FAILED"); }
  if (op == "kdown" || op == "kup") {                          // refcanvas kdown|kup space|alt|e|x|d|1 …: a key to the canvas, as the keyboard sends it
    const QString n = w.value(2); const int key = n == "space" ? Qt::Key_Space : n == "alt" ? Qt::Key_Alt : n[0].toUpper().unicode(); const int vk = n == "space" ? 0x20 : n == "alt" ? 0x12 : n[0].toUpper().unicode();
    QKeyEvent ke(op == "kdown" ? QEvent::KeyPress : QEvent::KeyRelease, key, Qt::NoModifier, 0, quint32(vk), 0); QApplication::sendEvent(canvas_, &ke); settle(30); }
  if (op == "tool") { setTool(w.value(2)); return "tool " + tool_; }
  if (op == "sel") {                                           // refcanvas sel all|none|invert|clear|rect|ellipse|lasso|poly|wand: the selection
    const QString a = w.value(2); if (a == "all") selectAll(); else if (a == "none") deselect(); else if (a == "invert") invertSel(); else if (a == "clear") clearSelected(); else { selKind_ = a; syncModes(); }
    return QString("sel %1: %2, box %3,%4 %5x%6, outline %7 parts").arg(a).arg(hasSel() ? "on" : "off").arg(selBox_.x()).arg(selBox_.y()).arg(selBox_.width()).arg(selBox_.height()).arg(selPath_.elementCount() / 2); }
  if (op == "split") { if (w.value(2) == "side") setActiveSide(w.value(3).toInt()); else if (w.value(2) == "view") setPaneViewTo(w.value(3).toInt(), w.value(4));
    else { if (w.size() > 3) { const int i = splitLayout_->findData(w[3]); if (i >= 0) splitLayout_->setCurrentIndex(i); } if (w.value(2) == "on" && splitOn_) setSplit(false); setSplit(w.value(2) != "off"); }
    QString o; for (auto& [p, id] : panePlane_) o += QString(" [pane %1 plane %2]").arg(p).arg(id); return QString("split %1: active pane %2 plane %3 canvas %4, kept %5,%6").arg(splitOn_).arg(vp_->activePane()).arg(id_).arg(cid_).arg(stash_.size()).arg(o); }
  if (op == "undo" || op == "redo") { xformUndo(op == "redo"); }
  if (op == "flip") { flipFloat(w.value(2) != "v"); }
  if (op == "reset") { resetFloat(); }
  if (op == "hard") { s_->brush.hardness = float(w.value(2).toDouble()); s_->brush.tip = {}; emit s_->brushChanged(); return QString("hardness %1").arg(s_->brush.hardness); }
  if (op == "optime") {   // refcanvas optime: the active layer's opacity moved in 30 steps — the time each step keeps the program busy, and one whole compose
    QElapsedTimer t; t.start(); qint64 worst = 0; for (int k = 0; k < 30; k++) { QElapsedTimer s1; s1.start(); if (Layer* l = s_->active()) { l->opacity = 1 - k / 40.f; emit s_->pixelsChanged({}); } settle(16); worst = std::max(worst, s1.elapsed() - 16); }
    QElapsedTimer c; c.start(); (void)s_->doc.compose(Channel::Color); return QString("opacity steps: worst %1 ms busy per step (30 steps in %2 ms); a whole compose %3 ms").arg(worst).arg(t.elapsed()).arg(c.elapsed()); }
  if (op == "grid") { toggleGrid(); return QString("grid %1").arg(gridB_->isChecked()); }
  if (op == "brushes") { return QString("presets %6 from %7; brushes %1 (built-in %2) in %3, sets: %4, listed %5").arg(s_->lib.brushes.size()).arg(s_->lib.builtin).arg(paint::BrushLibrary::folder(), s_->lib.sets().join(" | ")).arg(brushList_->count()).arg(s_->lib.presets).arg(paint::BrushLibrary::presetFolder()); }
  if (op == "fit") { canvas_->fit(); settle(150); const QTransform mt = canvas_->mapping(); return QString("fit: z %1 at %2 %3").arg(mt.m11(), 0, 'f', 3).arg(mt.dx(), 0, 'f', 0).arg(mt.dy(), 0, 'f', 0); }
  if (op == "panels") { panelsOn_ = w.value(2) != "off"; side_->setVisible(panelsOn_); tw_->setVisible(panelsOn_); return QString("panels %1").arg(panelsOn_); }
  if (op == "fillsel") { fillSelection(w.value(2) == "2" ? s_->color2 : s_->color); }
  if (op == "toolshot") { QApplication::processEvents(); return tw_->grab().save(w.value(2)) ? QString("tool window at %1,%2").arg(tw_->x()).arg(tw_->y()) : QString("toolshot FAILED"); }
  if (op == "toolmove") { tw_->move(w.value(2).toInt(), w.value(3).toInt()); saveTools(); return QString("tool window moved to %1,%2").arg(tw_->x()).arg(tw_->y()); }
  if (op == "size") { s_->brush.size = float(w.value(2).toDouble()); emit s_->brushChanged(); return QString("size %1").arg(s_->brush.size); }
  if (op == "movekind") { moveKind_ = w.value(2); syncModes(); return "move " + moveKind_; }
  if (op == "layer") { layerAction(w.value(2)); return QString("layer %1: %2 layers").arg(w.value(2)).arg(s_->doc.layers().size()); }
  if (op == "click") {                                         // refcanvas click x,y [shift|alt]: one press and release (the polygonal lasso's corners, the wand, the fill)
    Qt::KeyboardModifiers mods; if (w.contains("shift")) mods |= Qt::ShiftModifier; if (w.contains("alt")) mods |= Qt::AltModifier;
    const QStringList xy = w.value(2).split(','); const QPointF pt = canvas_->texToWidget(QPointF(xy.value(0).toDouble(), xy.value(1).toDouble()));
    QMouseEvent pe(QEvent::MouseButtonPress, pt, canvas_->mapToGlobal(pt), Qt::LeftButton, Qt::LeftButton, mods); QApplication::sendEvent(canvas_, &pe);
    QMouseEvent re(QEvent::MouseButtonRelease, pt, canvas_->mapToGlobal(pt), Qt::LeftButton, Qt::NoButton, mods); QApplication::sendEvent(canvas_, &re); QApplication::processEvents();
    return QString("click %1: sel %2 box %3x%4, poly %5").arg(w.value(2)).arg(hasSel() ? "on" : "off").arg(selBox_.width()).arg(selBox_.height()).arg(poly_.size()); }
  if (op == "enter") { QKeyEvent ke(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); keyPressEvent(&ke); return QString("enter: sel %1 box %2x%3").arg(hasSel() ? "on" : "off").arg(selBox_.width()).arg(selBox_.height()); }
  if (op == "hidden") { QString o; for (auto& x : hidden_) if (x) o += " " + QString(x->metaObject()->className()) + ":" + x->objectName(); return QString("hidden %1:%2").arg(hidden_.size()).arg(o); }
  if (op == "mirror") { mirrorCopy(w.value(2) == "l"); auto x0 = mirrorX(); return QString("mirror, x0 %1").arg(x0 ? *x0 : -1.0, 0, 'f', 1); }
  if (op == "paste") { QImage im(w.size() > 2 ? w[2].toInt() : 300, w.size() > 3 ? w[3].toInt() : 200, QImage::Format_ARGB32); im.fill(QColor(40, 160, 220)); return pasteImage(im) ? "pasted" : "paste FAILED"; }
  if (op == "key") { if (w.value(2) == "enter") applyFloat(); else cancelFloat(); return "key " + w.value(2); }
  if (op == "shot") { QApplication::processEvents(); return grab().save(w.value(2)) ? "shot " + w.value(2) : QString("shot FAILED"); }
  if (op == "stroke") {
    Qt::KeyboardModifiers mods; std::vector<QPointF> P;
    for (int i = 2; i < w.size(); i++) { if (w[i] == "ctrl") mods |= Qt::ControlModifier; else if (w[i] == "alt") mods |= Qt::AltModifier; else if (w[i] == "shift") mods |= Qt::ShiftModifier;
      else { const QStringList xy = w[i].split(','); P.push_back(canvas_->texToWidget(QPointF(xy.value(0).toDouble(), xy.value(1).toDouble()))); } }
    if (P.empty()) return "stroke: no points";
    auto send = [&](QEvent::Type t, const QPointF& p, Qt::MouseButtons bs) { QMouseEvent e(t, p, canvas_->mapToGlobal(p), Qt::LeftButton, bs, mods); QApplication::sendEvent(canvas_, &e); };
    send(QEvent::MouseButtonPress, P[0], Qt::LeftButton);
    for (size_t i = 1; i < P.size(); i++) for (int k = 1; k <= 12; k++) send(QEvent::MouseMove, P[i - 1] + (P[i] - P[i - 1]) * (k / 12.0), Qt::LeftButton);
    send(QEvent::MouseButtonRelease, P.back(), Qt::NoButton); QApplication::processEvents();
  }
  // (info: the active layer's painted pixels, the markers, the floating piece)
  const Layer* l = s_->active(); int painted = 0;
  if (l && !l->image.isNull()) for (int y = 0; y < l->image.height(); y += 4) { const QRgb* r = reinterpret_cast<const QRgb*>(l->image.constScanLine(y)); for (int x = 0; x < l->image.width(); x += 4) if (qAlpha(r[x]) > 20) painted++; }
  QString mk; for (auto& m : sc_->refMarkers) mk += QString(" (%1 %2 %3)").arg(m.pos.x, 0, 'f', 2).arg(m.pos.y, 0, 'f', 2).arg(m.pos.z, 0, 'f', 2);
  auto f = sc_->refSketches.find(cid_);
  { const QTransform mt = canvas_->mapping(); mk += QString(" | rot %8 cam %9 %10 zoom %11").arg(std::atan2(mt.m12(), mt.m11()) * 180 / M_PI, 0, 'f', 1).arg(vp_->camera().pos.x, 0, 'f', 3).arg(vp_->camera().pos.y, 0, 'f', 3).arg(vp_->camera().zoom, 0, 'f', 3); mk += QString(" | colour %1 / %2, brush op %3, map z %4 at %5 %6, selected %7").arg(s_->color.name(), s_->color2.name()).arg(s_->brush.opacity, 0, 'f', 1).arg(canvas_->zoom(), 0, 'f', 3).arg(mt.dx(), 0, 'f', 0).arg(mt.dy(), 0, 'f', 0).arg(QApplication::activeModalWidget() == this); }
  return QString("%1: painted %2 (of 1/16 px), layers %3, markers %4%5, float %6 (%7 %8 s %9 a %10), sketch rev %11")
    .arg(op).arg(painted).arg(s_->doc.layers().size()).arg(sc_->refMarkers.size()).arg(mk).arg(fl_.on).arg(fl_.cx, 0, 'f', 0).arg(fl_.cy, 0, 'f', 0).arg(fl_.sx, 0, 'f', 2).arg(fl_.a, 0, 'f', 0)
    .arg(f == sc_->refSketches.end() ? -1 : f->second.rev);
}

}  // namespace hm
