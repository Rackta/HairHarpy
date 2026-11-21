// Projection painting in the Paint room: a stroke on the model is laid on the screen first — the brush's dabs round and even there —
// and carried into the texture through the polygons the camera sees: every texel takes what the stroke holds at its place on the
// screen. A stroke crosses UV seams whole and is the same width wherever the UV is stretched. Symmetry: the mirrored side takes it
// through the mirrored view. On the same screen picture: a stencil — a gray picture hung over the viewport the brush paints through;
// a strip — a pattern repeated along the stroke in place of the dabs; and the view itself sent to an external editor (Photoshop) and
// what was changed there carried back onto the model from the camera it was taken with.
#pragma once
#include "view/Viewport.h"
#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QString>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class QWidget;
namespace hm::paint { class Stroke; }

namespace hm {

class PaintCanvas;
class PaintSession;

class PaintProj {
public:
  PaintProj(PaintSession* s, Viewport* vp, PaintCanvas* cv, QWidget* vpBox);
  QString stat, statAll;                            // (checks: the last step of a stroke; every step since it was last read)
  bool on = true;                                   // strokes on the model are projected (off — the dabs laid in the texture, as before)
  bool input(int phase, const QPointF& m, bool invert);
  bool would() const;                               // (a press now would begin a projected stroke: it paints into every UV set under it — no set has to be shown for it)
  // (a UV set that is not the one shown: its locked, hidden and frozen texels put back as they were — the owner's)
  std::function<void(const QString& set, QImage& target, const QImage& before, const QRect& r)> clipSet;
  int lockId_ = 0;                                      // (the island the stroke began on — kept in it; 0 — free)   // 0 press (false: not a stroke this takes — the older way), 1 move, 2 release
  // the stencil
  QString stencilFile, stripFile; double stScale = 1, stAngle = 0, stX = 0, stY = 0; bool stTile = true;
  bool setStencil(const QString& file);             // ("" — off)
  bool setStrip(const QString& file);
  void sync();                                      // (the stencil's picture over the viewport shown / moved / hidden)
  // the view to an external editor and back
  // The view goes as a PSD of two layers: "Colour" — the texture as it is, no light — and "Light" over it (Multiply): the room's
  // light, switched on and off there like 3D-Coat's Lightmap. Saved — every layer but "Light" composed is what comes back.
  QString sendView(const QString& saveTo = {});   // (saveTo: the file written there, no editor opened — checks; .psd — the layered file, else the colour)
  QString takeBack(const QImage& img);
  QString takeBackFile(const QString& psd);       // (the layered file saved in the editor)
  static QString viewFileName() { return QStringLiteral("hh_view_projection.psd"); }
  // the faces the camera saw at a slant: what comes back fades out between these |cos| — full at angleFull, none at angleNone
  double angleFull = 0.5, angleNone = 0.17;        // (60°, 80°: as Substance's Angle + Hardness, Blender's normal falloff)
  bool skipEdge = true, checkSeen = true;          // (checks: off — as before 18.11)
  double edgePx = 4;                               // (what comes back fades in over this many pixels from the edge of the silhouette)
private:
  PaintSession* s_; Viewport* vp_; PaintCanvas* cv_; QWidget* box_; QWidget* over_ = nullptr;
  QImage scr_, prev_, before_, stencil_, strip_; std::unique_ptr<paint::Stroke> st_; int layer_ = -1; bool erase_ = false, active_ = false; QPointF last_; double arc_ = 0;
  bool thr_ = false; double thrK_ = 0, thrO_ = 0;      // (the stroke paints through: a texel behind the seen surface counts while it lies within the brush's radius of it — reach = thrK_ × depth + thrO_)
  struct SetSt { QImage before; bool touched = false; };   // (a UV set the stroke passed over: the layer's picture there before the stroke)
  // (prev_: the stroke on the screen as the texture last took it — a texel whose value there has not changed is not written again)
  std::map<QString, SetSt> sets_; std::vector<int> pick_;
  QElapsedTimer strokeT_; int steps_ = 0; qint64 texelsAll_ = 0, stepMax_ = 0; QRect box_s_;   // (the stroke going: its steps, the texels written, its slowest step, where on the screen)
  std::map<QString, TileMask> pend_; class QTimer* flushT_ = nullptr; void flush();   // (what the stroke changed and has not shown yet, by the UV set)
  QImage sent_; Viewport::PvView sentV_; QString sentSet_; std::vector<float> sentEdge_;   // (sentEdge_: per pixel, how far it lies from the edge of the silhouette or of a surface in front)   // (the colour picture sent; what the camera saw then — every polygon, the depth and the polygon at every pixel)
  void project(const QRect& screenDirty);
  QRect stripSegment(const QPointF& a, const QPointF& b);
  float stencilAt(const QPointF& p) const;
  friend class StencilOver;
};

}  // namespace hm
