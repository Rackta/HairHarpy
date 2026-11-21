// The Paint room's work on a part of a picture — no brush dabs: the freeze mask's own operations (made solid, a closed contour
// filled), a texel held back by the mask, and the effects laid over a region at once (a lasso, a rectangle, whole polygons of the
// mesh; the Recolor brush's coverage): paint, erase, blur, sharpen, recolour, lighter / darker, more / less saturated.
// Core only: QImage in, QImage out.
#pragma once
#include "paint/Brush.h"
#include "paint/PaintDoc.h"
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <vector>

namespace hm::paint {

// ---- the freeze mask (Grayscale8: 255 — frozen) ----
// "to 100 %": every texel frozen at least `threshold` (1…255) becomes fully frozen, the rest free. Then the contour is rounded
// (smoothPx: the stairs a hard edge has are taken off — nothing thinner than that survives) and its edge is given a width (softPx:
// the passage from frozen to free, in texels; 0 — a hard edge)
QImage maskSolid(const QImage& m, int threshold, float smoothPx, float softPx);
// a gray mask's edge (the author, 17.11: a fill by polygons leaves a hard pixel edge in 3DCoat — smoothed by hand there):
// op 0 — moved out by px, 1 — moved in, 2 — smoothed: the staircase rounded off over px, the edge staying crisp (a 1-texel ramp),
// 3 — only a band px wide either side of the edge.
// im (optional): what grows never reaches an island the mask did not touch (at: where m's top-left lies in the island map's texture)
enum class EdgeOp { Expand = 0, Contract = 1, Smooth = 2, Border = 3 };
// the Stamp's patch (as Photoshop's healing): the texels of dst inside box that cov marks (> 0) filled from round them — the colours of src
// outside the mark, within pad of box, spread inward (a push-pull pyramid: the nearest colours weigh most); im: only the UV islands the
// mark lies on lend their colours. Mixed by cov. false — nothing round the mark to fill it from
bool healRegion(QImage& dst, const QImage& src, const QImage& cov, const QRect& box, int pad, const IslandMap* im = nullptr);
QImage maskEdge(const QImage& m, EdgeOp op, float px, const IslandMap* im = nullptr, QPoint at = {}, QSize tex = {});
// the free region round a texel, up to the mask's lines (texels frozen at least `threshold`) and — with the islands given — up to the
// edge of the UV island it lies in: 255 inside, 0 elsewhere, grown by `grow` texels (onto the lines' soft edge). box — its bounds.
// Null: the texel is frozen itself, or off the islands
QImage floodFree(const QImage& m, const QPoint& at, int threshold, const IslandMap* isl, int grow = 2, QRect* box = nullptr);
// a Grayscale8 picture blurred by a box of that radius, `passes` times (a soft edge for a coverage)
QImage boxBlur8(const QImage& g, int radius, int passes = 2);

// a row of texels kept as they were: hold[x] says how much of the picture before the stroke stays (0 — none, 255 — all).
// Colour (ARGB32, straight alpha) is mixed premultiplied: an erase half held back leaves the colour as it was at half its
// density — never a darker one, never single texels
void keepTexels(QImage& target, const QImage& before, int y, int x0, int x1, const uchar* hold);

// ---- an effect over a region ----
enum class RegionOp { Paint, Erase, Blur, Sharpen, Hue, Lighten, Darken, Saturate, Desaturate };
QString regionOpKey(RegionOp o);
QString regionOpName(RegionOp o);
RegionOp regionOpFromKey(const QString& k);
// the layer (ARGB32) inside a coverage (Grayscale8, its top-left at covAt in the layer; 255 — in full) takes the effect, read from
// `src` — the layer as it was — so a texel reached twice is still changed once. r — the part of the layer to work on.
// ink: Paint — the colour (its alpha — the opacity); Hue — the tone and saturation given. strength 0…1
void applyRegion(QImage& layer, const QImage& src, const QImage& cov, const QPoint& covAt, const QRect& r, RegionOp op, const QColor& ink, float strength);

// a picture as it was before a change, kept tile by tile as the change reaches them (an 8K layer is not copied whole)
class TileSnap {
public:
  static constexpr int TILE = 256;
  void start(const QImage& src);                      // (nothing kept yet)
  void reach(const QImage& src, const QRect& r);      // (the tiles under r, not kept yet: copied from src — call before writing there)
  const QImage& image() const { return snap_; }       // (right inside the tiles reached)
  std::vector<Stroke::Tile> tiles() const;            // (what Ctrl+Z puts back)
  bool empty() const { return touched_.empty(); }
private:
  QImage snap_; int tw_ = 0, th_ = 0; std::vector<char> seen_; std::vector<int> touched_;
};

}  // namespace hm::paint
