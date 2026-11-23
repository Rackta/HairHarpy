// The Paint room's document: a stack of layers over one texture size — paint layers (colour / height / flow), fill and
// adjustment layers, groups, each with an optional mask. Maps composed from it: Colour (sRGB, straight alpha), Height (a bump
// map: 0.5 = no relief; height layers hold DELTAS from it, so a layer needs no alpha) and Flow (the anisotropy direction: r, g =
// the direction's x, y encoded 0…255 around 128, a = coverage). Core only — no GPU, nothing of the viewport.
#pragma once
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <unordered_map>
#include <array>
#include <vector>
#include <cmath>
#include <limits>

namespace hm::paint {

// the UV islands of the set painted, as texels: each texel holds its island's number (0 — none), the islands grown a few texels into
// the empty texels round them (their padding: a stroke kept in its island still reaches the edge's bleed); and each island's long axis
// (degrees, texture space: x right, y down) — "along the strand"
struct IslandMap {
  QImage ids;                 // (RGB32: the island's number in the low 24 bits)
  std::vector<float> angle;   // (by number; [0] unused)
  int at(int x, int y) const { return ids.isNull() || x < 0 || y < 0 || x >= ids.width() || y >= ids.height() ? 0 : int(reinterpret_cast<const quint32*>(ids.constScanLine(y))[x] & 0xFFFFFFu); }
  int at(const QPointF& p) const { return at(int(std::floor(p.x())), int(std::floor(p.y()))); }
  float angleOf(int id) const { return id > 0 && size_t(id) < angle.size() ? angle[size_t(id)] : std::numeric_limits<float>::quiet_NaN(); }
};

// the maps a set of layers is painted for. The first three are kept as they were (their numbers are in saved projects); the rest are
// the maps of the anime / toon pipelines (Guilty Gear Xrd's ILM, Genshin's light map, MToon, Unity Toon Shader, lilToon): each is a
// mode of the Paint room with its own layers. Height is what the Normal mode paints — a relief, read as a normal map afterwards
enum class Channel { Color, Height, Flow, Shade, ShadowMask, Specular, Rim, Outline, Emission, Opacity, MatId, BakeMask, OverHair, Count };
// how a map's layers are kept: colour (ARGB32), a relief (Grayscale16 deltas), a direction (RGBA8888) — a gray map is kept as colour
// and painted with grays (its layers blend as any colour's), written out as one channel
enum class Storage { Colour, Relief, Direction };
struct MapInfo {
  Channel ch; const char* key; const char* name; const char* suffix;   // key: in the project and the canvas's view; suffix: the exported file's
  Storage storage; bool gray; QRgb ground;                             // ground: the map where no layer covers it (what the shader reads as "nothing here")
  const char* about;
};
const std::vector<MapInfo>& maps();                     // in the order the room shows them
const MapInfo& mapInfo(Channel ch);
Channel channelFromKey(const QString& key, bool* ok = nullptr);
QImage::Format formatOf(Channel ch);
enum class LayerKind { Paint, Fill, Adjust, Group };
// (kept in the project by their keys — the order here is free; blendOrder(): as the list shows them, Photoshop's groups)
enum class Blend { Normal, Multiply, Screen, Overlay, Add, Subtract, SoftLight, Darken, Lighten,
                   ColorBurn, LinearBurn, ColorDodge, HardLight, VividLight, LinearLight, PinLight, Difference, Exclusion, Divide, Hue, Saturation, Color, Luminosity };
const std::vector<Blend>& blendOrder();
// GradientMap: the lightness of what is under it read through a gradient (Layer::grad) — painted in gray, coloured by the map
enum class AdjustKind { BrightContrast, HueSat, Levels, Invert, Threshold, Posterize, GradientMap, Curves };

QString blendName(Blend b);
QString blendKey(Blend b);
Blend blendFromKey(const QString& k);
QString adjustName(AdjustKind a);
QString adjustKey(AdjustKind a);
AdjustKind adjustFromKey(const QString& k);

struct Layer {
  int id = 0;
  QString name;
  LayerKind kind = LayerKind::Paint;
  Channel channel = Channel::Color;   // (a Paint / Fill layer works on one map)
  Blend blend = Blend::Normal;
  float opacity = 1;
  bool visible = true, locked = false;
  int parent = -1;                    // the Group it lies in (-1: the top level)
  QImage image;                       // Colour: ARGB32 · Height: Grayscale16 (32768 = no relief) · Flow: RGBA8888
  QImage mask;                        // Grayscale8, 255 = shown (null — no mask)
  // (image / mask are the pixels in the UV set shown now; the other sets' wait here by the set's name. One list of layers serves every
  // set — as in 3D-Coat — and a layer has pixels only in the sets it was painted in: "Lips" costs nothing in the body's set)
  struct Pixels { QImage image, mask; };
  std::map<QString, Pixels> other;
  bool maskOn = true;
  bool clip = false;                  // clipped to the layer below: shown only where that one has something (its shadow, its highlight); an adjustment — works only there
  std::vector<std::pair<float, QColor>> grad;   // a Gradient Map's stops: where (0 — the darkest … 1 — the lightest) and the colour (empty — black to white)
  bool lockAlpha = false;             // the brush changes the colour where the layer is painted — never where it is empty
  QColor fill = QColor(128, 128, 128);// a Fill layer: the colour; for Height — the gray's delta (value 128 ± ...)
  float fillHeight = 0;               // a Fill layer on Height: the delta, −1…1
  AdjustKind adjust = AdjustKind::BrightContrast;
  float p1 = 0, p2 = 0, p3 = 0;       // adjustment's parameters (see Document::adjustParams)
  // Curves (as Photoshop's): [0] — all three channels, [1] [2] [3] — red, green, blue alone; each sorted points 0…1 → 0…1, a
  // smooth curve through them (empty — straight). A channel's own curve first, then the common one (18.11, recording Dress_3:
  // the whole PSD sent to Photoshop for Curves on one layer)
  std::array<std::vector<QPointF>, 4> tones;
  // an effect layer (see paint/LayerFx.h): its pixels made from the layer / group `fx.src` — a stroke, an inner shadow, a bevel —
  // again whenever that one changes; empty — an ordinary layer. Baked: the record dropped, the pixels kept
  QJsonObject fx;
  static float toneAt(const std::vector<QPointF>& pts, float x);   // (the smooth curve through the points — monotone cubic)
  // a live gradient (a Fill layer on a colour map): the colour of each texel read from `grad` by where the surface under it lies
  // between the two points g0 → g1 of the scene (0 at g0, 1 at g1, the same colour beyond) — the points, the stops, the blend and
  // the opacity stay editable; nothing is baked (Substance's 3D Linear Gradient; the author's tint over the whole hairdo, 16.11)
  bool gradLive = false;
  float g0[3] = {0, 1, 0}, g1[3] = {0, 0, 0};
  // the live layer's kind: "linear" — the gradient between g0 and g1; "spheres" — colour spheres in the scene: each colours the
  // surface inside its radius, full up to its hardness and fading to nothing at the radius; where spheres overlap their colours mix
  // by their weights (the author, 16.11: a light one on the crown, darker ones at the hair's edges — more than a line gives). A
  // sphere's link (0 — none): spheres of one link share their colour, radius and hardness
  QString liveKind = QStringLiteral("linear");
  // "curve": the gradient runs along a curve drawn on the model (root → tip of a strand, the author 17.11): each texel takes the place
  // on the curve nearest to it (0 — the curve's start … 1 — its end); the layer's mask holds the strands it lies on
  std::vector<std::array<float, 3>> curve;
  struct Sphere { float c[3] = {0, 0, 0}; float r = 1, hard = 0.3f; QColor col = QColor(255, 255, 255); int link = 0; };
  std::vector<Sphere> spheres;
  // (per UV set: where each texel lies between g0 and g1 — 0: no surface there, 1…65535: 0…1 — made again only when the points or
  // the scene's geometry change; a change of colours, blend or opacity is only a look-up through it)
  struct GradT { QImage t; float a[3] = {0, 0, 0}, b[3] = {0, 0, 0}; qint64 rev = -1; QImage rgba; quint64 skey = 0; };   // (rgba: the spheres' colours, ARGB32 — skey: of which spheres)
  mutable std::map<QString, GradT> gradT;
  // the UV sets the layer is shown in (empty — every set): a Fill layer made over what was clicked lives in that set only — its
  // mask is that set's, and with no mask in the other sets it would cover their whole textures
  QStringList onlySets;
};

class Document {
public:
  Document() = default;
  Document(int w, int h) { reset(w, h); }
  void reset(int w, int h);
  int width() const { return w_; }
  int height() const { return h_; }
  bool empty() const { return layers_.empty(); }

  const std::vector<Layer>& layers() const { return layers_; }     // bottom → top
  Layer* layer(int id);
  const Layer* layer(int id) const;
  int indexOf(int id) const;

  int addPaint(Channel ch, const QString& name = {}, int above = -1);
  int addFill(Channel ch, const QColor& c, const QString& name = {}, int above = -1);
  int addAdjust(AdjustKind k, const QString& name = {}, int above = -1, Channel ch = Channel::Color);   // (it changes the map it was made in)
  int addGroup(const QString& name = {}, int above = -1);
  int duplicate(int id);
  static QString layerFile(const QString& kind, int id, const QString& set) { return fileOf(kind, id, set); }   // (a layer's / mask's file in a project's .paint folder)
  int adopt(Layer l, int above) { l.parent = -1; return push(std::move(l), above).id; }   // (a layer brought from another document: a new id, over `above`)
  void remove(int id);                                  // (a Group takes its layers with it)
  void restoreLayers(std::vector<Layer> ls);            // (the list put back as it was kept — an undo step of a delete / merge / mask command; the UV set shown must be the one it was kept in)
  void move(int id, int delta);                         // (among its neighbours: +1 up, −1 down)
  bool mergeDown(int id, int* into = nullptr);          // (a paint layer onto the paint layer under it, same map; into: that layer's id)
  void addMask(int id, bool white = true);
  void removeMask(int id);
  void invertMask(int id);
  void applyMask(int id);                               // (the mask burned into a paint layer's alpha)
  // a fill layer — a flat colour, a live gradient, colour spheres — made an ordinary paint layer: its pixels as it shows them now, in
  // every UV set it shows in (its mask burned in), the blend and the opacity kept
  bool rasterize(int id);

  // what an adjustment's p1…p3 mean: BrightContrast (−1…1, −1…1), HueSat (hue −180…180, sat −1…1, light −1…1), Levels
  // (black 0…1, white 0…1, gamma 0.1…4), Threshold (level 0…1), Posterize (steps 2…32)
  static void adjustDefaults(AdjustKind k, float& p1, float& p2, float& p3);
  // a gradient's colours for lightness 0…255 (256 × r, g, b in 0…1) — the stops in any order, the same colour before the first and after the last
  static std::vector<float> gradientLut(const std::vector<std::pair<float, QColor>>& stops);

  // the composed map (rect: only that part is recomposed into the cache and returned whole)
  QImage compose(Channel ch, const QRect& rect = {}) const;
  // during a stroke on a top-level paint layer: what lies under it does not change — its mix is kept by 64-texel tiles of the
  // set shown and only the layer and those above it are mixed again (-1: off, the kept tiles dropped)
  void cacheBelow(int layerId);
  QImage composeFlat(Channel ch) const { return compose(ch); }
  // the map as it is written out: over its ground, opaque — a gray map as Grayscale8, colour as RGB (colour keeps its alpha instead
  // when keepAlpha), the relief and the direction as composed
  QImage flatten(Channel ch, bool keepAlpha = false) const;
  bool hasLayers(Channel ch) const;                      // (a visible paint / fill layer of that map)
  void ensurePixels(int id);                             // (a paint layer with no pixels in the set shown: a blank picture made for it — when it is first painted on)

  // the UV sets: the one shown (its name — "" the project's single texture — and size) and the ones kept aside. Switching parks every
  // layer's pixels under the old set's name and brings the new set's (read from the project's folder the first time)
  struct SetInfo { QString key; int w = 0, h = 0; };
  const QString& activeSet() const { return set_; }
  const std::vector<SetInfo>& sets() const { return sets_; }
  void setActiveSet(const QString& key, int w, int h);
  // the other UV sets without showing them — the projected brush paints into every set it passes over, the model shows them all:
  // a set's size (told beforehand — declareSet — for a set never shown); a colour paint layer's picture in a set (make: a blank one
  // where the layer has none there — null: no such layer / set); the map of a set composed. The pointer holds until the set shown changes
  QSize setSize(const QString& key) const;
  void declareSet(const QString& key, int w, int h);
  QImage* pixelsIn(int id, const QString& set, bool make = false);
  QImage composeSet(const QString& set, Channel ch, const QRect& r = {});
  bool hasPixelsIn(const QString& set) const;
  void ensureLoaded(const QString& set);     // (a set not shown yet since the project was opened: its pixels read from the project's folder)

  QJsonObject toJson() const;                            // (layers' records, no pixels)
  QJsonObject packJson() const;                          // (with the pixels: PNGs in base64 — kept inside a project record)
  bool unpackJson(const QJsonObject& o);
  bool fromJson(const QJsonObject& o);
  bool saveTo(const QString& dir) const;                 // (pixels: one PNG per layer / mask into dir)
  // the same in three steps — the writing can run on another thread: what to write (the pictures as they are now), written, noted.
  // mine: dir is the project's own folder (the document reads its other sets from it afterwards); false — a copy elsewhere (an autosave)
  struct SavePlan { struct Job { QImage im; QString path; qint64 key = 0; }; std::vector<Job> jobs; QSet<QString> keep; QString dir, set; bool ok = false, sameDir = false, mine = true; qint64 bytesEach = 0; std::map<QString, bool> loaded; };
  SavePlan savePlan(const QString& dir) const;
  static std::vector<char> saveWrite(const SavePlan& p);
  bool saveFinish(const SavePlan& p, const std::vector<char>& ok) const;
  bool loadFrom(const QString& dir);

  int nextId() const { return nextId_; }
  QString freeName(const QString& base, const QString& sep = QStringLiteral(" ")) const;   // "<base> <n>", the smallest n not taken
  QString smartName(int id) const;                        // a name from the layers under it and its blend / kind: SleevesOver_1
  int layerAt(Channel ch, int x, int y) const;            // the topmost shown paint layer with paint at that texel (−1: none)
  bool isBlank(const Layer& l) const;                     // a paint layer with nothing painted in the UV set shown
  int solo = -1;                                          // (≥ 0: that layer composed alone — with the groups round it and what lies in it)
  // where the surface under every texel of a UV set lies (w × h × 3 floats each: the place and the normal; a normal of 0 — no surface
  // there) and a number that changes when the scene's geometry or UVs do: given by the room (the document knows nothing of the
  // scene). Read by the live gradients — on the composing thread, before the bands go to the cores, and only when their points or that
  // number changed
  std::function<bool(const QString& set, int w, int h, const float*& pos, const float*& nrm)> positionsOf;
  std::function<qint64(const QString& set)> geomRevOf;
  bool soloLets(const Layer& l) const;
private:
  mutable std::map<int, std::pair<qint64, bool>> blank_;  // (isBlank: a layer → its picture's key and the answer)
  int w_ = 0, h_ = 0, nextId_ = 1;
  std::vector<Layer> layers_;
  QString set_; std::vector<SetInfo> sets_; QString dir_; std::map<QString, bool> loaded_;   // (dir_: where the pixels were read from / written to; loaded_: the sets read from it)
  void noteSet(const QString& key, int w, int h);
  void loadSet(const QString& key, bool parked);
  static QString fileOf(const QString& kind, int id, const QString& set);
  mutable std::map<QString, qint64> saved_;   // (a layer's file → the picture's key when it was written there or read from it: saveTo skips what did not change)
  Layer& push(Layer l, int above);
  QImage blank(Channel ch) const;
  struct Px { float r, g, b, a; };
  void composeScope(Channel ch, int parent, const QRect& r, std::vector<Px>& out, int startAt = -1, int stopAt = -1) const;   // (startAt: from that layer on; stopAt: up to it, not including)
  void readyGradients(Channel ch) const;                   // (the live gradients shown in this set: their texel maps made if stale)
  int cacheLayer_ = -1; Channel cacheCh_ = Channel::Color; QString cacheSet_;
  mutable std::unordered_map<qint64, std::vector<Px>> cacheTiles_;
};

}  // namespace hm::paint
