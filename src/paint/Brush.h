// The Paint room's brushes: a tip (a gray mask — from an ABR file or round with a hardness), the dynamics, and a mode that says
// what the brush writes — colour, an erase, a relief (Raise / Lower / Flatten — the bump / height map), the flow direction of an
// anisotropic highlight (the stroke's own direction is written), or a layer mask. Stroke stamps a brush along a path into one
// QImage; the Document's layer images are what it is given.
#pragma once
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <functional>
#include <optional>
#include <vector>

namespace hm::paint {

enum class BrushMode { Paint, Erase, Raise, Lower, Flatten, Flow, MaskReveal, MaskHide, Blur, Smudge, Clone, Patch };   // (Blur: the colours under the brush run together; Smudge: pulled along the stroke;
// Clone — the Stamp: what lies at the source (cloneAt) laid under the brush; Patch — the Stamp's patch: the stroke only marks, the owner fills the marked texels from round them)
QString brushModeName(BrushMode m);
QString brushModeKey(BrushMode m);
BrushMode brushModeFromKey(const QString& k);
// the image format a mode writes into: 0 — ARGB32 colour, 1 — Grayscale16 height, 2 — RGBA8888 flow, 3 — Grayscale8 mask
int brushTarget(BrushMode m);

// a response curve 0…1 → 0…1 through a few points (monotone cubic between them) — the pen pressure's effect
struct Curve {
  std::vector<QPointF> pts{{0, 0}, {1, 1}};   // (sorted by x, x from 0 to 1)
  float at(float x) const;
  bool linear() const { return pts.size() == 2 && pts[0] == QPointF(0, 0) && pts[1] == QPointF(1, 1); }
  QJsonArray toJson() const;
  static Curve fromJson(const QJsonValue& v);
};

// the alpha's shape: from a picture (an ABR's, a PNG) or made here at the brush's size — round by its hardness, the cylinder (a band:
// bright along its middle line, soft to its sides — the hair brush), a square, a gradient
enum class TipKind { Image = 0, Round = 1, Cylinder = 2, Square = 3, Gradient = 4 };
QString tipKindName(TipKind k);
QImage proceduralTip(TipKind k, float soft, float flat, float core, int px);   // (Grayscale8, px × px; at angle 0 the band runs along x)

// the pen's tilt now, 0 (upright) … 1 (60° and more) — set by the tablet's events; 1 when the mouse paints (as its pressure)
inline float g_penTilt = 1.f;

struct Brush {
  QString name = "Soft Round";
  QString set;                // the brush set it belongs to (the reference canvas's sets: "Basic" — the built-in ones, an ABR's file name, the user's own); empty — "My brushes"
  QImage tip;                 // Grayscale8, 255 = full (null — a round tip by hardness)
  BrushMode mode = BrushMode::Paint;
  float size = 64;            // px of the texture
  float spacing = 0.12f;      // of the size
  float hardness = 0.5f;      // (a round tip)
  float opacity = 1;          // the stroke's cap
  float flow = 0.6f;          // what one dab adds
  float strength = 0.3f;      // Raise / Lower: the relief a full stroke makes (0…1 of the height range)
  float angle = 0;            // degrees
  bool followStroke = false;  // the tip turned along the stroke
  float sizeJitter = 0, angleJitter = 0, scatter = 0;   // 0…1 (scatter: of the size)
  bool pressureSize = true, pressureFlow = false;
  float smoothing = 0;        // 0…1: Steady Stroke — the brush follows the pointer on a string (that share of 2 × the brush's diameter on the screen), catching up at the end
  float shiftBlur = 0.5f;     // 0…1: how strongly the brush blurs while Shift is held (3D-Coat's Smoothing)
  // what Shift held does to the brush: 0 — a blur round the point (3D-Coat's Smoothing); 1 — a blur along the strand (its UV island's
  // long axis: a smooth passage down the lock, not a blot); 2 — oil: the colour where the stroke begins is taken up and dragged along,
  // mixing little by little with what it passes over (the author, 16.11)
  int shiftMode = 1;
  bool shiftAlong = true;     // (kept for the older brush files: shiftMode 1)
  // the pen's pressure: what it changes, the least it leaves at no pressure (min) and how it rises (the curve)
  bool pressureOpacity = false;
  float pSizeMin = 0.03f, pOpacityMin = 0, pFlowMin = 0;
  int pSizeSrc = 0, pOpacitySrc = 0, pFlowSrc = 0;   // (what drives it: 0 — the pen's pressure, 1 — its tilt)
  Curve pSizeCurve, pOpacityCurve, pFlowCurve;
  // the colour's dynamics: jitter of hue / saturation / brightness (0…1) per dab or once per stroke; wet — how much of the colour under the
  // brush mixes into each dab (0 — none: the colour as it is), load — how fast the brush takes that colour on (0…1)
  float hueJ = 0, satJ = 0, briJ = 0; bool colourPerDab = true;
  float wet = 0, load = 0.5f;
  // the alpha: its kind (Image — the tip picture above), the made ones' profile, and how it is turned — 0 by the Angle only,
  // 1 along the stroke, 2 along the strand under it (the UV island's long axis)
  TipKind tipKind = TipKind::Image;
  float tipSoft = 0.6f, tipFlat = 0.25f, tipCore = 0;
  int orient = 0;

  QJsonObject toJson() const;
  static Brush fromJson(const QJsonObject& o);
  QImage preview(const QSize& s) const;     // a stroke on a dark ground (the brush list's thumbnail)
  QImage preview(const QSize& s, const QColor& ink, bool pen) const;   // (pen: as a pen stroke — pressed and tilted harder to its middle: the pen's settings seen)
};

class Stroke {
public:
  Stroke(QImage& target, const Brush& b, const QColor& color = Qt::white);
  bool valid() const { return ok_; }
  void begin(const QPointF& p, float pressure = 1);
  void move(const QPointF& p, float pressure = 1);
  void end() {}
  QRect takeDirty() { QRect r = dirty_; dirty_ = {}; return r; }
  const QImage* clipIds = nullptr; std::vector<quint32> clipAllow;   // (the stroke kept in its island: only texels whose island (IslandMap::ids) is one of these)
  std::function<float(const QPointF&)> strandAngle; // (orient 2: the strand's direction at a point of the target, degrees; NaN — not known there)
  float softenK = 1;                               // (Blur / Smudge: their strength multiplied — Shift's blur by the brush's shiftBlur)
  bool blurAlong = false;                          // (Blur: the colours run together along the strand — strandAngle — and further, not round the pixel)
  bool oilCarry = false;                           // (Smudge as oil paint: a load of colour carried by the brush, picking up a little of what it passes over at every dab)
  float sizeScale = 1;                             // (the brush's size multiplied: a brush sized on the screen — the texels a screen pixel covers where the stroke is now)
  bool maskInk = false;   // (a mask painted as Photoshop: towards the colour's gray — black hides, white shows; Erase still hides)
  bool lockAlpha = false;                          // (the layer's transparency kept: the colour changes only where something is painted already)
  std::function<std::vector<QPointF>(const QPointF&)> mirrors;   // (Symmetry with several mirrors / turns: every dab at each of these too)
  std::function<QPointF(const QPointF&)> mirror;   // (Symmetry: every dab also at mirror(point) — a left ↔ right mirror)
  // (Clone: where a dab at this point takes its picture from — a texel of the target → a point of cloneSrc; none — the dab is not laid.
  // cloneSrc: the picture copied from, as it was when the stroke began — the layer itself, or every layer composed)
  std::function<std::optional<QTransform>(const QPointF&)> cloneAt; QImage cloneSrc;
  const QImage& coverage() const { return cov_; }  // (what the stroke has covered so far, 0…255 — Patch fills these texels)
  // the target as it was before the stroke. Kept in tiles (TILE px), each copied only when the stroke first reaches it — an 8K
  // layer is not copied whole for a stroke across a corner of it: before() is right inside the tiles touched (every rectangle
  // takeDirty gave lies in them), nothing is known of the rest
  static constexpr int TILE = 256;
  const QImage& before() const { return snap_; }
  struct Tile { QPoint at; QImage img; };
  std::vector<Tile> takeBefore() const;            // the touched tiles as they were (what Ctrl+Z puts back)
private:
  QImage* t_; Brush b_; QColor color_; bool ok_ = false;
  float ink_[3] = {1, 1, 1};                       // (the brush's colour now: the colour, its stroke jitter, what it took on from the canvas)
  QColor dabColour(const QPointF& c, float size);  // (this dab's colour: jitter, the wet mix with what is under it)
  QImage snap_, cov_; int tw_ = 0, th_ = 0; std::vector<char> seen_; std::vector<int> touched_;
  void reach(int x0, int y0, int x1, int y1);      // (the tiles under a dab: kept as they are, their coverage cleared, before it writes)
  QPointF last_, dir_{1, 0}, trail_; float lastP_ = 1, carry_ = 0; bool started_ = false, dirKnown_ = false; QRect dirty_;   // (trail_: the point the stroke's direction is taken from — kept a rope's length behind)
  float load_[4] = {0, 0, 0, 0}; bool loaded_ = false;   // (oilCarry: the brush's load — premultiplied 0…255, alpha 0…1)
  quint32 rng_ = 12345;
  float rnd() { rng_ = rng_ * 1664525u + 1013904223u; return (rng_ >> 8) / 16777216.f; }
  void dab(const QPointF& p, float pressure);
  void dabAt(const QPointF& at, float size, float ang, float pressure, const QColor& col);
};

// the saved brushes (from ABR imports and the built-in ones); one folder: a PNG per tip and brushes.json
class BrushLibrary {
public:
  std::vector<Brush> brushes;
  int builtin = 0;            // (the first ones are made in code; the rest are kept)
  int presets = 0;            // (after them: the user's preset brushes — read from presetFolder(), never written back nor removed here)
  int fixed() const { return builtin + presets; }   // (the ones a "remove" leaves alone)
  // a preset pack of the user's own (their Photoshop brushes): Documents/HairHarpy Brushes — brushes.json and its tips, as the library's
  // own folder; outside the program and outside AppData (a packaged app's AppData writes go to its own copy, a pack put there is unseen)
  static QString presetFolder();
  static QString folder();
  void addBuiltIn();
  void load();
  void save() const;
  int importAbr(const QString& file, QString* error = nullptr, const QString& set = {});     // (the brushes added; −1 on a file that's not an ABR; set — the set they go into, empty: the file's name)
  QStringList sets() const;   // the sets in their order (Basic first, then as the brushes come)
  static QString setOf(const Brush& b) { return b.set.isEmpty() ? QStringLiteral("My brushes") : b.set; }
};

}  // namespace hm::paint
