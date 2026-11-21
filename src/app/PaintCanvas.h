// The Paint room's texture canvas: a floating window with the texture (UV space) — the composed Colour / Height / Flow map, the
// bump read as a normal map, or one of the bake's maps. The active brush paints into the active layer (or its mask) here: left
// button (a tablet's pen with its pressure), Alt + click picks a colour, the wheel zooms, middle button / Space + drag pans,
// Ctrl+Z undoes a stroke. (Painting right on the 3D model goes through the same brush and layers: the next stage.)
#pragma once
#include "paint/Region.h"
#include <QImage>
#include <QPointF>
#include <QTransform>
#include <QWidget>
#include <QHash>
#include <QPolygonF>
#include <vector>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

class QPainter;
class QTimer;
namespace hm::paint { class Stroke; }

namespace hm {

class PaintSession;

class PaintCanvas : public QWidget {
  Q_OBJECT
public:
  explicit PaintCanvas(PaintSession* s, QWidget* parent = nullptr);
  void fit();
  // (hooks for a canvas that is not the Paint room's — the reference canvas): drawn under / over the texture (r — the texture's
  // rectangle in the widget), a tool other than the brush taking the left button (phase 0 press, 1 move, 2 release — true: taken),
  // the mirrored point of a texture point while Symmetry is on (nullopt — none: the second cursor and the mirrored dabs)
  std::function<void(QPainter&, const QRectF&)> drawUnder, drawOver;
  std::function<bool(int, const QPointF&, Qt::KeyboardModifiers)> toolInput;
  std::function<std::optional<QPointF>(const QPointF&)> mirror;
  std::function<std::optional<QPointF>(const QPointF&)> mirrorOf; std::function<bool()> symmetryOn;   // (a texture point's mirror under the symmetry, painting or not — the Stamp's symmetric copy; nullopt: Symmetry off or no mirror there)
  std::function<std::vector<QPointF>(const QPointF&)> mirrorsN;   // (the Paint room's symmetry: any number of mirrored dabs — texture px)
  bool painting() const { return painting_; }
  // (the reference canvas's selection): after each part of a stroke — the target image, the image before the stroke, the part changed
  // (pixels outside the selection put back)
  std::function<void(QImage&, const QImage&, const QRect&)> clip;
  std::function<void()> eraserKey;
  std::function<void(const QPointF&)> pickLayer;   // (Ctrl + Shift + click: the layer painted at that texture point taken as the active one — texture px)
  std::function<bool(bool redo)> undoHook;       // (Ctrl+Z / Ctrl+Shift+Z: the owner's first — true: done there)
  std::function<void()> strokeBegan;
  std::function<void(bool pen, float lo, float hi)> strokeEnded;   // (a stroke done: drawn with the pen (its pressure's range) or the mouse — the owner says so)            // (a stroke laid down: its colour into the owner's recent colours)
  std::function<void(const QPointF&)> hovered;   // (the pointer moved with no button held: an owner's tool drawing to it — the polygonal lasso's line, the split's other view)
  // (a press seen first — the split: a click in the other view makes it the one drawn in, nothing drawn; navigating — a view being moved)
  std::function<bool(const QPointF&, Qt::MouseButton, bool navigating)> pressFilter;
  std::function<bool(const QPointF&)> ringHere;  // (where the brush's ring may show: the view drawn in)               // (E: the owner's Eraser tool instead of the canvas's own toggle)
  void setEraser(bool on) { eraser_ = on; update(); }
  // (the Paint room's viewport) the picture shown — the map as the canvas draws it, kept up to date part by part — and a call whenever it
  // changed; the brush driven from outside: a point of the texture in 0…1 across and down (phase 0 press, 1 move, 2 release, 3 lifted —
  // off the model; a jump over a UV seam starts a new stroke)
  const QImage& shown() const { return disp_; }
  QRect shownDirty() const { return shownDirty_; }   // (what the last change of shown() touched — null: all of it)
  QImage mapPicture(const QRect& r = {}) { return renderMap(r); }   // (the map shown, of the UV set the document holds now, made anew — r: a part of it)
  std::function<void()> shownChanged;
  void strokeTex(int phase, const QPointF& uv, bool invert, float sizeScale = 1, float pressure = 1, bool pen = false);   // (sizeScale: the brush's size multiplied for this point — a brush sized on the screen; pressure — the pen's on the model, pen: the stroke is the pen's)
  bool eraser() const { return eraser_; }
  void pickGlobal(const QPoint& g) { pick(mapFromGlobal(g)); }   // (V over the texture: the colour under the pointer)
  QPointF texToWidget(const QPointF& t) const { return mapped_ ? xf_.map(t) : own().map(t); }
  QPointF texAt(const QPointF& w) const { return toTex(w); }
  double zoom() const { return mapped_ ? std::sqrt(std::abs(xf_.determinant())) : zoom_; }
  // the texture turned on the canvas (degrees clockwise, any angle the hand likes — no steps); kept per UV set
  void rotateBy(double deg, int snap = 0);   // (snap: only the reference canvas over the viewport uses it)
  void centreOn(const QPointF& tex) { pan_ += QPointF(width() / 2.0, height() / 2.0) - own().map(tex); update(); }
  void setRotation(double deg);
  double rotation() const { return rot_; }
  // where the brush is on the model, shown here as a second ring (texture px; nullopt — off the model): the stroke on the model seen on the
  // texture at once. pointerTex — this canvas's pointer as a texture point (nullopt — it left), for the model to show the same
  void showRemote(std::optional<QPointF> tex, float radiusTex) { remote_ = tex; remoteR_ = radiusTex; update(); }
  std::function<void(std::optional<QPointF>)> pointerTex;
  // the islands over the texture: the dark ground outside them, their outlines, the polygons' wire (the polygons in texture px)
  std::function<const std::vector<QPolygonF>*()> uvWire;
  // (the canvas over the viewport): bare — no picture, no ground of its own (the plane in the viewport shows it, the model stays seen);
  // the mapping texture → widget set from outside, the view moved by the hooks (the wheel: notches; a pan: pixels) instead of pan_ / zoom_
  bool bare = false, ring = true; Qt::CursorShape restCursor = Qt::BlankCursor;
  void setMapping(const QTransform& t);
  QTransform mapping() const { return mapped_ ? xf_ : own(); }
  std::function<void(const QPointF&, double)> navZoom;
  std::function<void(const QPointF&)> navPan;
  std::function<void()> navFit;
  std::function<void(double, bool)> navRotate;   // (degrees clockwise on the screen; true — in 15° steps)
  std::function<void()> navRotateReset;
  std::function<QPointF()> viewCentre;           // (what a rotation turns round)
protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void tabletEvent(QTabletEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void keyReleaseEvent(QKeyEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void showEvent(QShowEvent*) override;
  void leaveEvent(QEvent*) override { hover_ = false; if (pointerTex) pointerTex(std::nullopt); update(); }
  void enterEvent(QEnterEvent*) override;
  bool focusNextPrevChild(bool next) override { return bare ? false : QWidget::focusNextPrevChild(next); }   // (Tab: the panels' key, not the next button)
private:
  PaintSession* s_;
  QImage disp_;                       // the shown map, ARGB32_Premultiplied
  QRect shownDirty_;
  QImage normalCache_; bool normalDirty_ = true, wholePending_ = false;
  double zoom_ = 1; QPointF pan_;     // the texture's top-left in widget px
  bool fitted_ = false, mapped_ = false; QTransform xf_, xfi_;
  bool autoFit_ = true;   // (fitted and not zoomed / panned / turned by hand since: a resize fits the texture again — it fills the panel however it is sized)
  bool panning_ = false, space_ = false, painting_ = false, hover_ = false, checker_ = true, tooling_ = false, eraserTip_ = false;
  QPointF last_, hoverAt_, anchor_, lastEnd_; bool hasLastEnd_ = false, picking_ = false, zooming_ = false, rotating_ = false, rKey_ = false, sizing_ = false, eraser_ = false; float size0_ = 0;
  QPointF lazy_; float lastPressure_ = 1, pLo_ = 1, pHi_ = 0; bool penStroke_ = false;   // (smoothing: where the brush is, on its string behind the pointer — widget px)
  // Shift + LMB: held and dragged — the brush blurs (as 3D-Coat); a click with no drag — a straight stroke from where the last one ended.
  // Which is known only once the pointer moves or is let go: the press waits here
  bool shiftPending_ = false, shiftInv_ = false; QPointF shiftAt_; float shiftP_ = 1;
  int adjMode_ = 0; float op0_ = 1, hard0_ = 0.5f;              // (the RMB drag: 1 — size + opacity (a plain RMB), 2 — size + hardness (Alt+RMB, as Photoshop))
  bool flash_ = false; QTimer* flashT_ = nullptr; float lastSize_ = -1; int undoId_ = 0; QImage* target_ = nullptr;   // (flash_: the brush's size just changed — its ring shown a moment, even with the pointer away)
  QPointF ringAt() const;
  bool pressAt(const QPointF& w, Qt::MouseButton b, Qt::KeyboardModifiers mods, float pressure, bool invert);
  void moveAt(const QPointF& w, Qt::KeyboardModifiers mods, float pressure);
  void releaseAt(const QPointF& w, Qt::KeyboardModifiers mods);
  void zoomDrag(const QPointF& w);
  void hud(const QString& s);
  std::unique_ptr<paint::Stroke> stroke_;
  QRect dirty_; QPointF tex3d_; float texScale_ = 1;
  // (the Recolor tool: the stroke lays a coverage — fxCov_ — and the layer under it is made anew from what it was — fxSnap_ — each time
  // it grows: a texel passed twice is recoloured once, as strongly as the stroke's densest pass over it)
  bool patch_ = false;                 // (the Stamp's patch: the marked texels filled from round them as the stroke goes, and once more, whole, at its end)
  bool fx_ = false; QImage fxCov_; paint::TileSnap fxSnap_; paint::RegionOp fxOp_ = paint::RegionOp::Hue; QColor fxInk_;
  void applied(const QRect& r);       // (a part of the stroke just laid: the effect, the locks and the freeze mask, the picture shown)
  void rebuild(const QRect& r = {});
  QImage render(const QRect& r); QImage renderMap(const QRect& r);
  QPointF toTex(const QPointF& w) const { return mapped_ ? xfi_.map(w) : own().inverted().map(w); }
  double rot_ = 0, rotRaw_ = 0; QString rotSet_; QHash<QString, double> rotBySet_;
  QTransform own() const { QTransform t; t.translate(pan_.x(), pan_.y()); t.rotate(rot_); t.scale(zoom_, zoom_); return t; }   // (the texture's place on the canvas: moved, turned, scaled)
  std::optional<QPointF> remote_; float remoteR_ = 8;
  QImage ov_; QString ovKey_;                                   // (the islands' overlay, made once per islands / setting)
  const QImage& overlay();
  void panBy(const QPointF& d);
  void hoverUpdate(const QPointF& was);
  void begin(const QPointF& w, float pressure, bool invert = false, int how = 0);   // (how: 0 the brush as it is, 1 blurring (Shift held), 2 a straight stroke from the last one's end)
  void move(const QPointF& w, float pressure);
  void end();
  void pick(const QPointF& w);
  void zoomAt(const QPointF& w, double f);
};

// the window around it (the channel picker and the toolbar). like: the room's canvas — its guards (the freeze mask, the locks, the
// selection), symmetry and stroke hooks given to this one too; the tools other than a brush by hand are refused here
QWidget* makePaintCanvasWindow(PaintSession* s, QWidget* parent, const PaintCanvas* like = nullptr);

// the colour just taken (V / Alt): a small loupe beside the pointer for a moment — the texels round the point magnified, the colour
// taken beside them. tex — the texture point; global — the pointer on the screen
void showPickLoupe(PaintSession* s, const QPointF& tex, const QColor& c, const QPoint& global);
// take the colour at a texture point into the brush (PaintSession::pickColor: the texture's own texels, never the screen) and show the
// loupe — false: nothing there (outside the islands)
bool pickInto(PaintSession* s, const QPointF& tex, const QPoint& global);

}  // namespace hm
