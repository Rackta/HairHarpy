// The Paint room's tools on the model and on the texture alike. A painting tool (the brush, the eraser, Blur, Smudge, Recolor — and
// the freeze mask painted with them) is laid the way PaintSession::drawMode says: a stroke by hand; a line through clicked points
// (Enter or a click on its last point lays it, Backspace takes a point off, Esc drops it; a point can be dragged before that); a
// region — a lasso, a rectangle, an ellipse (Shift — a square / a circle), whole polygons of the mesh passed over — that takes the
// tool's effect at once. The other tools: Fill (a polygon, its part, its polygroup, the object, a UV island, a closed contour of the
// freeze mask, the whole layer); the polygons' selection (Shift adds, Ctrl takes off; H and its kin then work on the selected); the
// topological symmetry (a click marks the red polygon, Ctrl + click the blue one — two neighbours across the middle line; the layer
// is then copied from one side onto the other through the mesh's own topology); the text (a click — where it starts).
#pragma once
#include "view/Viewport.h"
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QTransform>
#include <Qt>
#include <functional>
#include <map>
#include <set>
#include <vector>

class QPainter;

namespace hm {

class PaintCanvas;
class PaintSession;

class PaintTools {
public:
  PaintTools(PaintSession* s, Viewport* vp, PaintCanvas* cv);
  void setTool(const QString& t);
  // what a click of the Fill tool fills: face | part | group | object | layer — polygons; island — the UV island under the click;
  // contour — the free patch round the click, up to the freeze mask's lines and the island's edge. With the freeze mask painted
  // (PaintSession::freezePaint) the same click freezes that instead of colouring it; inv (Ctrl) — erases / frees
  QString fillKind = "part";
  bool fillGradient = false;            // (drag: from the colour to the second colour along it)
  bool fillSpheres = false;
  bool fillCurve = false;               // (the "Along a curve" variant: a curve drawn on the model from root to tip — a live gradient along it, on the selected strands)
  void fillAll();                       // (Alt+Backspace: the active layer filled with the colour wherever it is not frozen, in the UV set shown)             // (the Spheres variant: a click puts a colour sphere — on a spheres layer, made if the active one is not)
  float fillSoft = 3.f;                 // (the fill's edge smoothed over this many texels: the pixel staircase of a fill by polygons rounded off, the edge crisp)
  float fillTol = 0.12f; bool fillContig = true, fillAllLayers = false;   // (the "Similar colour" fill: how far a colour may be from the clicked one; joined only; taken from every layer)
  void fillAt(const QPointF& a, const QPointF& b, bool inv = false);
  // the active layer's colour spheres (a live "spheres" layer) with Fill: a click on the model — a new sphere there in the brush's
  // colour; a drag that begins inside a sphere — the sphere moved where it ends; Ctrl + click — the sphere there removed
  void spheresAt(const QPointF& a, const QPointF& b, bool inv);
  // the active live gradient's ends: where each is on the screen now (nullopt — behind the camera / no such layer); a point set from a
  // window point (on the model — the surface there; off it — the plane through its old place that faces the camera)
  std::optional<QPointF> gradEndScreen(int k) const;
  bool setGradEnd(int k, const QPointF& m);
  bool topoMask = true;                 // (the topological symmetry carries the freeze mask across with the layer)
  // the fill stays live: while Fill is in hand and nothing else was done since, a change of the brush's colour (or opacity) recolours
  // the last fill — the colour is tried on the whole part without Ctrl+Z and a new click. fillAsLayer: the click makes a Fill layer
  // over the active one instead, its mask — what was clicked: that colour can be changed any time later (the Layers panel)
  bool fillAsLayer = false;
  void relive();
  // Piece (the texture window): outline a piece of the active layer with a lasso — it floats: drag it to move, drag outside it to turn
  // (Shift — 15° steps), drag a corner to scale; Enter / Apply lays it into the layer (pieceNewLayer: onto a new one), Esc drops it.
  // pieceCut: the place it was taken from is cleared. A drawing carried from one UV island onto another by hand
  bool pieceCut = false, pieceNewLayer = false;
  bool pieceOn() const { return piece_.on; }
  void pieceApply(); void pieceDrop(); void pieceFlip(bool across); void pieceTurn(double deg);
  void pieceNudge(const QPointF& d) { if (piece_.on) piece_.c += d; }   // (checks: moved by so many texels, scaled so many times)
  void pieceScale(double k) { if (piece_.on) piece_.sc *= k; }
  QString selKind = "face";             // face | part | group | rect | lasso (the last two: the polygons seen inside the outline dragged)
  int textSize = 48; QString textFont;  // (the text: its height — screen px on the model, texture px on the texture)
  void layCurve();                      // the line's points so far → laid with the tool in hand
  void cancel();                        // a line / region being made dropped
  int linePoints() const;               // (the points of the line being made)
  double lineWidthAt(double t) const;   // (the line's width along it, of the brush's size: PaintSession::lineStart / lineEnd / lineTaper)
  QString topoCopy(int dir);            // 0 — red → blue, 1 — blue → red, 2 — the two sides swapped → what was done
  std::function<QString(const QString& title)> askText;   // (the text typed: the owner's dialog)
  QString gesture() const;              // (what the left button does now: a painting tool — its draw mode; another tool — itself)
private:
  PaintSession* s_; Viewport* vp_; PaintCanvas* cv_;
  bool vdrag_ = false, tdrag_ = false, hoverOn_ = false, inv_ = false, grabMoved_ = false; QPointF va_, vb_, ta_, tb_, vhover_; std::vector<QPointF> vpts_, tpts_; int selOp_ = 0, grab_ = -1;
  std::vector<Viewport::PaintAnchor> anch_;              // (the line's points on the surface)
  // (the line laid last, still editable (the author, 18.11: «the first one lay wrong»): its points stay shown; a point dragged or one put
  // into it takes the line back (its undo steps, while they are the last) and lays it again. A click elsewhere on the model, Esc or
  // another tool lets it go)
  std::vector<Viewport::PaintAnchor> laid_; int laidUndo_ = -1, laidSteps_ = 0; bool grabLaid_ = false, inserted_ = false;
  void relay();
public:
  QString lineState() const { return QString("line: points %1, laid %2 (steps %3, undo id %4)").arg(anch_.size()).arg(laid_.size()).arg(laidSteps_).arg(laidUndo_); }
private:
  std::map<int, std::set<int>> faces_;                   // (Polygons mode: the mesh's polygons passed over so far)
  bool onView(int phase, const QPointF& m, Qt::KeyboardModifiers mods);
  bool onTex(int phase, const QPointF& t, Qt::KeyboardModifiers mods);
  bool key(int k);
  std::vector<QPointF> figure(const QPointF& a, const QPointF& b, const QString& kind) const;   // a rectangle's / an ellipse's outline between two corners
  void strokeTex(const std::vector<QPointF>& pts);
  QImage texCoverage(const std::vector<QPointF>& outline, QRect* box);
  QImage viewCoverage(const std::vector<QPointF>& outline, QRect* box);
  void applyCov(const QImage& cov, const QRect& box, bool inv);
  std::vector<QPointF> lasso_;
  struct Live { int layer = -1, undoId = 0; QImage cov; QRect box; QColor ink; } live_;   // (the last fill: where it went, the undo step that holds the layer as it was)
  // (the floating piece: its picture, where its middle is in the texture, its turn and scale; what was grabbed: 1 moved, 2 turned, 3 scaled)
  struct Piece { bool on = false; QImage img, cov; QRect box; int layer = -1; QPointF c, c0, grab; double ang = 0, sc = 1, ang0 = 0, sc0 = 1; bool fx = false, fy = false; int mode = 0; } piece_;
  QTransform pieceXf() const;
  void text(const QPointF& at, const QPointF& ex, const QPointF& ey);
  std::vector<Vec3> curve3_; std::vector<QPointF> curveScr_; void layCurveGradient();   // (the curve being drawn: its points on the surface, and on the screen for the overlay)
};

}  // namespace hm
