// The Paint room's state, one per window: the layer document, the brush in hand, the saved brushes, the bake's maps, the lighting /
// toon settings, the Photoshop / 3D-Coat bridge. The panels and the texture canvas all read and change it and listen to its signals;
// the project keeps its records under P.paint, its pixels in a "<project>.paint" folder next to the file.
#pragma once
#include <memory>
#include "paint/Bake.h"
#include "paint/Bridge.h"
#include "paint/Brush.h"
#include "paint/MatGraph.h"
#include "paint/PaintDoc.h"
#include "paint/Region.h"
#include "paint/Shading.h"
#include <QHash>
#include <functional>
#include <map>
#include <optional>
#include <QMap>
#include <QObject>

namespace hm::paint { struct FxFrame; }
namespace hm {

class PaintSession : public QObject {
  Q_OBJECT
public:
  explicit PaintSession(QObject* parent = nullptr);
  paint::Document doc;
  paint::BrushLibrary lib;
  paint::Brush brush;                         // the one in hand (a copy of a saved one, then edited)
  QColor color = Qt::white, color2 = Qt::black;   // (white at first — the author, 17.11)   // (the second colour: X swaps them, D — black and white)
  int layer = -1;                             // the active layer
  bool editMask = false;                      // the brush writes into the active layer's mask
  bool showMask = false;                      // the active layer's mask shown in place of the map (double-click / Alt+click on its picture in the layer's row, as Photoshop)
  int gradPick = 0;                           // a live gradient's point waiting for a click on the model: 1 — the start, 2 — the end (any tool)
  int sphereSel = -1;                         // the colour sphere picked in the active spheres layer (the Layers panel, the Fill tool)
  QString tool = "brush";                     // the tool in hand (the left column's): brush | eraser | blur | smudge | recolor | fill | text | select | toposym
  // how a painting tool (brush, eraser, blur, smudge, recolor — and the freeze mask painted with them) is laid:
  // stroke — by hand; polyline — a line through clicked points (lineSmooth: a curve through them; drawFlat: the points lie on the
  // screen and the line is carried onto the model from the view it is laid in — off: they lie on the surface, the line follows it);
  // lasso / rect / ellipse — what is outlined takes the tool's effect at once (drawThrough: what is hidden behind, too;
  // shapeOutline — a rectangle / an ellipse: only its outline, as a stroke); faces — whole polygons of the mesh passed over
  QString drawMode = "stroke"; bool drawFlat = false, drawThrough = false, lineSmooth = false, shapeOutline = false;
  // drawThrough is also the brush's own mode on the model (Y: a tap switches it, held — only while the key is down): off — the
  // stroke paints only what the camera sees; on — the walls behind the seen surface too, as far behind as the brush's radius
  // (both sides of a thin lock at once, the far side of a sleeve) — not the hair farther back. The ring says which (doubled, amber)
  void setThrough(bool on, bool keep = true);
  float lineStart = 1, lineEnd = 1, lineTaper = 0.25f;   // (a line's width at its two ends, of the brush's size, and the share of its length each end takes to get there)
  // the Stamp (the author, 18.11): 0 — the symmetric copy (the mirrored side under the brush, Symmetry on), 1 — from an anchor
  // (Ctrl + click sets it; the texture is carried from there, kept at the same offset stroke after stroke), 2 — the patch (a spot
  // painted over is filled with the colours round it). stampAll: the copy taken from every layer as seen, not the active one alone
  int stampMode = 0; bool stampAll = false; std::optional<QPointF> stampAnchor, stampOff;
  paint::RegionOp recolorOp = paint::RegionOp::Hue;      // (the Recolor tool: hue | lighten | darken | saturate | desaturate | sharpen)
  void saveDrawPrefs() const;
  // the freeze mask: a gray picture per UV set (the set's size; white — frozen): the brushes and the tools leave the frozen texels as
  // they are (a gray one — partly). freezePaint: the brush paints the mask itself (Ctrl / the eraser — takes it off); kept with the project
  int cavity = 0;                             // the brush held by the baked curvature: 0 — off, 1 — it paints in the hollows only, 2 — on the ridges only; 3 — near the scalp only (the marked scalp's nearness)
  // (cavity 4 — Shine: the brush paints only where the light catches the surface as seen now — its strength there; the light set round
  //  the view: shineAz degrees to the side, shineEl up; shineRough — how wide the highlight is. 19.11: "режим на кисть — прорисовать металл локально")
  float shineRough = 0.4f, shineAz = 30, shineEl = 40;
  float scalpReach = 0.06f;                   // (near the scalp: how far from it, a share of the painted meshes' size)
  bool freezePaint = false, freezeShown = true;
  QHash<QString, QImage> freeze;
  QImage* freezeOf(bool make);                // the shown set's mask (null — none; make: an empty one made)
  bool freezeAny() const;                     // the shown set has something frozen
  void freezeClear(); void freezeInvert();
  // the mask's own operations, each a Ctrl+Z step. Made solid ("to 100 %"): every texel frozen at all becomes fully frozen, the contour
  // rounded by freezeSmooth texels and given an edge freezeSoft texels wide. The closed contour round a texture point filled (remove:
  // the frozen patch there freed). A coverage frozen / freed. What the active layer holds (painted) or leaves empty, frozen. A mask kept
  // under a name and brought back (how: 0 — it alone, 1 — added to the mask there is, 2 — taken off it).
  // freezeHard — the mask's brush: hard, in full, whatever the pen presses (off: the brush as it is — a soft mask can be painted)
  bool freezeHard = true; float freezeSmooth = 2, freezeSoft = 1.5f;
  void freezeStep();                          // (the mask as it is now, onto the undo stack)
  void freezeSolid();
  float freezeEdgePx = 4;                     // (how far the edge is moved / smoothed, texels)
  void freezeEdge(int op);                    // (0 — expand, 1 — contract, 2 — smooth, 3 — only the border: paint::EdgeOp; a Ctrl+Z step)
  bool freezeFillAt(const QPointF& tex, bool remove);
  bool freezeAdd(const QImage& cov, const QPoint& at, bool remove);   // (cov: Grayscale8, its top-left at `at` in the mask)
  bool freezeFromLayer(bool painted);
  QMap<QString, QImage> freezeKept;           // (key: the UV set + '\x1f' + the name)
  QStringList freezeNames() const;            // (the shown set's kept masks)
  void freezeSave(const QString& name); bool freezeLoad(const QString& name, int how); void freezeDrop(const QString& name);
  void saveFreezePrefs() const;
  // the mask as a region of the active layer — what is frozen, or (free) what is not: the layer's paint there erased; copied (cut:
  // moved) onto a new layer above it → that layer's id; an adjustment layer put over it, clipped to it, working in that region alone
  // (its mask) → its id. −1 / false: no colour layer in hand, or nothing in the region
  bool regionErase(bool free);
  int regionLift(bool free, bool cut);
  int regionAdjust(paint::AdjustKind k, bool free);
  // what the canvas shows — and with it the room's mode: a map's key (paint::maps(): color, shade, shadow, spec, rim, outline,
  // emission, opacity, matid, height, bakemask, flow) | normalBump (the Normal mode's result: the bake through its mask + the painted
  // bump) | bake:<name>. The layers shown, the layer a "＋" makes and what the brush writes all follow it
  QString view = "color";
  bool onModel = true;                        // the map shown on the meshes, the brush paints on them (the canvas window's "On the model")
  paint::Channel modeChannel() const;         // the map the mode paints (normalBump: the bump)
  void setView(const QString& key);           // (the mode switched: the topmost layer of its map becomes the active one)
  // the UV set painted in: its name ("" — the project's single texture) and size. One list of layers serves every set; switching keeps
  // each layer's pixels of the other sets aside
  void setSet(const QString& key, int w, int h);
  QImage normalResult() const;                // the normal map as it will be written (null: neither a bake nor a bump)
  QMap<QString, QImage> baked;                // the bake's maps: normal, ao, height, position, curvature
  paint::BakeSettings bakeS;
  int highId = -1, lowId = -1;                // (scene items)
  float bumpStrength = 1;
  paint::LightRig rig = paint::LightRig::studio();
  paint::ToonSettings toon;
  std::function<void()> sendProjection;       // (the Bridge panel's "view to Photoshop": the main window's)
  // every paint layer of the map and the UV set shown, as one layered PSD: written into the bridge's folder and opened in the editor
  // (saveTo given: written there, no editor — checks); wire — the picture of a hidden "Wireframe" layer put on top. Saved in the
  // editor, the layers come back (takeLayers): a layer known by its name takes its new pixels, blend, opacity and eye; a new one is
  // added over the one under it; one that was sent and is not in the file any more (merged, deleted there) is removed here. The fill
  // and adjustment layers and the groups are not sent and stay where they are. → what was done, or why not
  QString sendLayers(const QImage& wire = {}, const QString& saveTo = {});
  QString takeLayers(const QString& path);
  // another project's (or an autosave's) Paint layers: listed, then the chosen ones brought over the active layer — with their names,
  // blends, opacity, clipping, masks and pixels in every UV set of this project that the other one has too (18.11, recording Dress_3:
  // a second 3D-Coat opened on an old save, ≈10 min to carry one layer over)
  // layer effects (paint/LayerFx.h): an effect layer over a layer or a group, its pixels made again in the background a moment after
  // the source stops changing (a stroke going: not at every dab)
  int addEffect(int srcId, const QString& type);
  void requestFx(int delayMs = 350);
  // a live shadow other objects cast onto the UV set shown (19.11, recording Cloth_2: the hair's shadow on the shoulders painted with
  // the hair hidden; the author: "живая тень объекта", its own light each): a Multiply layer of its own, made again in the background
  // when its light, softness or casters change; casters — scene objects by name (hidden ones cast too). -> the layer, -1
  int addShadow(const QStringList& casters);
  // a layer split into its shape and its colour (19.11, recording Cloth_2, 13:35: Emerald → EmeraldMask + a clipped colour copy by
  // hand, 11 clicks and two dialogs; the author: a command in the layer's menu): the layer becomes the base "<name>Mask", a copy
  // named as it was clipped over it — the colour is repainted freely with a big brush, the shape stays. -> the colour layer, -1
  int splitShapeColour(int id);
  // (the main window's: the named objects' triangles, as the bake reads them — shared while the scene's shapes stay)
  std::function<std::shared_ptr<const paint::TriMesh>(const QStringList& names)> castersOf;
  struct FileLayer { int id = -1; QString name, kind; int depth = 0; };
  std::vector<FileLayer> layersInFile(const QString& projectPath, QString* error = nullptr) const;
  QString takeLayersFrom(const QString& projectPath, const std::vector<int>& ids);
  // a texture made elsewhere (3D-Coat, Photoshop, Substance…) brought in as paint layers of the map and the UV set shown, over the
  // active layer: a PSD — each of its pixel layers with its name, blend, opacity and eye (the ones empty in it left out); a picture
  // (PNG, JPG, TGA, BMP, WEBP) — one layer. Another size than the texture's: scaled to it. → what was done, or why not
  // grow: asked when the file is bigger than the texture — 1: the UV set's texture made the file's size (its layers scaled up with
  // it; the caller has given the set that size), 0: the file scaled down, -1: nothing done
  QString importLayers(const QString& path, std::function<int(int fileW, int fileH)> grow = {});
  std::function<void()> sendLayersHook;       // (the main window's: it makes the wire's picture)
  std::function<bool()> strokeGoing;          // (the room's canvas: a stroke is being laid — what comes back from the editor waits for its end)
  void whenIdle(std::function<void()> f);
  struct PsdSent { QString set, path; paint::Channel ch = paint::Channel::Color; std::vector<int> ids; QStringList names; } psdSent_;
  QStringList texIn;                          // the UV sets whose "Model texture" layer was made from the meshes' own textures (once each)
  paint::MatGraph material;                   // the material builder's graph (empty — not made yet: the toon preset when the builder opens)
  paint::Bridge bridge;

  void ensureDoc(int size = 2048);
  paint::Layer* active() { return doc.layer(layer); }
  void setLayer(int id) { if (layer != id) { layer = id; if ((editMask || showMask) && (!active() || active()->mask.isNull())) { editMask = showMask = false; emit brushChanged(); } emit activeChanged(); } }   // (a layer with no mask: the brush back on its picture)
  // the brush's mode made to fit the active layer (a Paint brush on a Height layer raises, on a Flow layer writes the direction…)
  paint::BrushMode effectiveMode() const;
  // [ ] / Shift+[ ] from anywhere in the room: the size ×0.85 / ÷0.85, the hardness ∓0.1 (a HUD says the value)
  void stepBrush(bool up, bool hardness);
  // each painting tool keeps its own brush (as 3D-Coat: the eraser's size is not the brush's): rememberTool — the brush in hand kept as
  // the current tool's; recallTool — a tool switched to gets back what it had (the first time: the brush in hand). Kept in the settings
  void rememberTool();
  void recallTool(const QString& t);
  // the colour of the Color map at a texture point — the mean of the texels round it (pickSize: 1, 3 or 5 across) that hold paint;
  // nothing there (outside the UV islands, all transparent) — no colour. Never the screen: no cursor, no overlay, no background
  // The pipette's settings: pickSize 0 — the mean under the whole brush (radiusTex texels; the brush's own when not given) — for a
  // texture made of many small spots; pickFrom — "all": every visible layer, "layer": the active layer alone, "screen": the pixel of
  // the screen under the pointer (the owner's: any place of the screen, a reference included — not the empty viewport, not the empty
  // space round the UV islands)
  std::optional<QColor> pickColor(const QPointF& tex, double radiusTex = -1) const;
  int pickSize = 3; QString pickFrom = "all";
  // the UV islands of the set painted (the viewport's; null — none known): the pipette takes nothing outside them, a stroke may be kept
  // in the island it began on (islandLock), the alpha may lie along the strand (its island's long axis)
  std::function<const paint::IslandMap*()> islands;
  bool islandLock = false;
  bool ovDark = true, ovEdge = true, ovWire = false; QColor ovColor = QColor(11, 16, 48, 215);   // (the texture's island overlay: the dark ground outside, the outlines, the wire)
  // the alphas: a brush keeps its own tip, and another alpha can stand in for it for a while — the rest of the brush stays (3D-Coat's way
  // and Photoshop's both). Keys: "made:<TipKind>" (round, cylinder, square, gradient — made at the brush's size), "lib:<set>/<name>" — the
  // tip of a kept brush. The favourites ring (A held) shows alphaFavs
  QStringList alphaFavs;
  bool alphaStandIn = false;                  // (the brush's own tip is set aside — restoreTip brings it back)
  QString alphaKey = "own";                   // (the alpha in hand: "own", "round:<hardness>", "square:<softness>", "made:<kind>", "file:<path>", "lib:…")
  static QString alphaFolder();               // (the user's own alphas: Documents/HairHarpy Brushes/Alphas)
  QStringList userAlphas() const;             // ("file:<path>" of each picture there)
  bool applyAlpha(const QString& key);
  void restoreTip();
  QImage alphaThumb(const QString& key, int px) const;
  QString alphaLabel(const QString& key) const;
  void saveFavs() const;

  QJsonObject toJson() const;
  void fromJson(const QJsonObject& o);
  void resetForProject();                     // (another project comes: the layers, the undo steps, the baked maps, the freeze masks of the one before dropped)
  bool saveFiles(const QString& projectPath) const;
  // the same in the background: what changed since it was last written beside that file — the layers of every UV set, the freeze
  // masks — is encoded and written off the UI thread; painting goes on meanwhile. own: the project's own file (false — a copy: an
  // autosave). One at a time: a request made while one runs waits for it (the last one asked wins). done — back on the UI thread
  void saveFilesAsync(const QString& projectPath, bool own = true, std::function<void(bool)> done = {});
  bool saving() const { return saving_; }
  int edits = 0;                              // (bumped by every change of the pixels or the layers: what "unsaved" and the autosave look at)
  void loadFiles(const QString& projectPath);
  static QString sideFolder(const QString& projectPath) { return projectPath + ".paint"; }

  // one undo step of the canvas: the layer's (or mask's) image before a change — the whole of it (img), or only the tiles a
  // brush stroke touched (tiles; img null): a stroke on an 8K layer keeps a few hundred KB, not the layer's 256 MB
  // (set: the UV set whose pixels the step holds — the steps of every set stay in one list; taking one back shows its set first)
  // (multi: a stroke of the projected brush — the layer's picture as it was in every UV set the stroke passed over: one step, put
  // back in all of them at once)
  // (list: the whole layer list as it was — a delete / merge / mask command's step; active: the active layer then; extra: the bytes only
  //  this step keeps alive — the pictures the command dropped or changed)
  struct Undo { int layer = -1; bool mask = false; QImage img; std::vector<paint::Stroke::Tile> tiles; int id = 0; QString set; std::vector<std::pair<QString, QImage>> multi;
    std::shared_ptr<std::vector<paint::Layer>> list; int active = -1; qint64 extra = 0;
    qint64 bytes() const { qint64 n = img.sizeInBytes() + extra; for (const auto& t : tiles) n += t.img.sizeInBytes(); for (const auto& m : multi) n += m.second.sizeInBytes(); return n; } };
  // a command that changes the layer list or drops pixels (delete, merge down, a mask applied / removed, convert): the list kept before
  // op runs, one Ctrl+Z step when op says it changed something (op → false: nothing done, no step)
  bool layersEdit(const std::function<bool()>& op);
  bool stepSwap(std::vector<Undo>& from, std::vector<Undo>& to);
  std::vector<Undo> undo, redo;
  bool showSetOf(const Undo& u);
  QByteArray layersSig_;                      // (the layers' records as they were last seen: what tells a real change of the list)
  bool saving_ = false; struct SaveAsk { QString path; bool own = true; std::function<void(bool)> done; bool on = false; } saveNext_;
  std::map<QString, paint::Brush> toolBrush;  // (the brush each painting tool had when it was put down)
  QImage ownTip_; paint::TipKind ownKind_ = paint::TipKind::Image; float ownHard_ = 0.5f, ownSoft_ = 0.6f;   // (the brush's own tip while an alpha stands in)
  int pushUndo(Undo u);                       // (onto the stack, the redo dropped; the oldest steps let go past 40 or past the memory the stack may hold) → its id
  Undo* undoById(int id);                     // (a step still on the stack, else null)
  static qint64 undoBudget();                 // bytes the undo stack may hold: an eighth of the machine's memory, 512 MB … 4 GB
  bool undoStroke();
  // Match colour (the author, 18.11: an AI layer's tones fitted to the old painting by Hue/Saturation for half a minute): the layer's colours
  // moved so their mean and spread (in Lab) are those of what lies under it, at the same texels — then evened by hand. A Ctrl+Z step;
  // the result says what was done or why not
  QString matchColourBelow(int id);                          // true — something was undone
  bool redoStroke();
  // (layer effects: the debounce, what each effect layer was last made from by the UV set, the surfaces' frames by the set and the light)
  class QTimer* fxTimer_ = nullptr; std::map<std::pair<int, QString>, quint64> fxKey_; std::map<QString, std::pair<quint64, std::shared_ptr<const paint::FxFrame>>> fxFrames_; std::map<QString, std::shared_ptr<const paint::ShadowFrame>> shFrames_; int fxGen_ = 0;
  void runFx();
signals:
  void layersChanged();                       // the list / properties (structure, names, blend…)
  void activeChanged();
  void pixelsChanged(const QRect& r);         // a part of the composed maps (null — all)
  void brushChanged();
  void settingsChanged();                     // lighting / toon / bridge
  void bakeChanged();
  void viewChanged();                         // the mode (the map shown) or the UV set
  void throughChanged();                      // (drawThrough switched — the bar's button, the ring)
  void setPixels(const QString& set, const QRect& r);   // (a UV set that is not shown: its pixels changed there — null: all of it)
  void setLeaving();                         // (another UV set is about to be shown: this one's pixels are still in place)
};

}  // namespace hm
