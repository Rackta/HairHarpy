// The scene: the same items as the HTML version's `items` Map (group, curve, surface, guide, imesh, mesh, tube, ...),
// anchors shared between curves, the hierarchy (roots → children). Every item keeps the JSON record it was read from
// (`raw`), so fields this version doesn't handle yet go back into a saved project unchanged.
#pragma once
#include "scene/Refs.h"
#include "scene/Toon.h"
#include "core/Math.h"
#include <QJsonObject>
#include <QImage>
#include <QRect>
#include <QString>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace hm {

struct Anchor {
  int id = 0;
  Vec3 pos;
  double smooth = 1;
  std::optional<std::pair<int, int>> pass;   // rotated junction: the two neighbour ids passing smoothly through
  QJsonObject raw;
};

// a curve's handle override at one anchor (Alt-dragged handles)
struct HandleOverride { std::optional<Vec3> in, out; };

// anchor id → override in insertion order, as a JS Map (a key set again keeps its place; the order goes into the file)
class OvrMap {
public:
  using Entry = std::pair<int, HandleOverride>;
  using iterator = std::vector<Entry>::iterator;
  using const_iterator = std::vector<Entry>::const_iterator;
  iterator begin() { return v_.begin(); }
  iterator end() { return v_.end(); }
  const_iterator begin() const { return v_.begin(); }
  const_iterator end() const { return v_.end(); }
  iterator find(int k) { return std::find_if(v_.begin(), v_.end(), [k](const Entry& e) { return e.first == k; }); }
  const_iterator find(int k) const { return std::find_if(v_.begin(), v_.end(), [k](const Entry& e) { return e.first == k; }); }
  size_t count(int k) const { return find(k) != v_.end() ? 1 : 0; }
  HandleOverride& operator[](int k) { auto it = find(k); if (it != v_.end()) return it->second; v_.push_back({k, {}}); return v_.back().second; }
  size_t erase(int k) { auto it = find(k); if (it == v_.end()) return 0; v_.erase(it); return 1; }
  iterator erase(iterator it) { return v_.erase(it); }
  size_t size() const { return v_.size(); }
  bool empty() const { return v_.empty(); }
  void clear() { v_.clear(); }
private:
  std::vector<Entry> v_;
};

// a polygon mesh as read from a file / a converted mesh: positions + polygons (CSR: faceStart has nFaces + 1 entries)
struct PolyMesh {
  std::vector<Vec3> pos;
  std::vector<int> faceStart{0};
  std::vector<int> faceVerts;
  std::vector<int> faceGroup;            // polygroup per polygon (may be empty)
  std::vector<QString> groupNames;
  // the file's texture coordinates and materials: a UV per polygon corner (cornerUV, parallel to faceVerts; empty — none)
  // and a material slot per polygon (faceMat, into matNames)
  std::vector<std::array<float, 2>> cornerUV;
  std::vector<int> faceMat;
  std::vector<QString> matNames;
  struct Map { QString name; QByteArray data; };          // a texture image the file carries itself (glTF / GLB, FBX)
  std::vector<std::array<Map, 2>> matMaps;                  // per material: colour, opacity
  std::vector<QString> matColor;                            // per material: the file's colour ('#rrggbb', empty — none)
  std::vector<char> matAlpha;                               // per material: see-through in the file (blend / alpha test)
  std::vector<QString> mtllib;                              // an OBJ's material libraries
  bool flipY = true;
  // what an FBX / glTF / OBJ brings for its shading and subdivision (kept through the Topology room's edits and written
  // into an FBX): a normal per polygon corner (custom normals; empty — none, the normals are made), hard edges (the
  // smoothing: the normals split there), edge creases (0…1, for subdivision); edges as vertex pairs (any order)
  std::vector<std::array<float, 3>> cornerNormal;
  std::vector<std::array<int, 2>> hardEdges;
  struct Crease { int a = 0, b = 0; float w = 0; };
  std::vector<Crease> creases;
  std::vector<std::array<int, 2>> seams;   // the UV seams (the Topology room's UV: where Unfold cuts), vertex pairs
  // UV Sets (the Topology room): each a UV layout of its own — over the polygons whose bit k of faceSets is on, its UVs per
  // corner in setUV[k] (parallel to faceVerts). faceLive: the set whose UV the polygon shows now (cornerUV holds it; the set's
  // own copy is brought up to date before it is read); −1 — none. No names — one set of everything, not made yet
  std::vector<QString> uvSetNames; std::vector<uint32_t> faceSets; std::vector<int8_t> faceLive; std::vector<std::vector<std::array<float, 2>>> setUV;
  std::vector<std::array<float, 3>> col;   // a tint per vertex (an imported mesh's weights shown), empty — none
  std::vector<std::array<float, 3>> vcol;  // polypaint: the file's colour per vertex (sRGB 0…1; empty — none) — shown with the standard material
  int faceCount() const { return int(faceStart.size()) - 1; }
  void addFace(const int* v, int n) { faceVerts.insert(faceVerts.end(), v, v + n); faceStart.push_back(int(faceVerts.size())); }
  Box3 bounds() const { Box3 b; for (auto& p : pos) b.expand(p); return b; }
};

enum class ItemType { Group, Curve, Surface, Guide, Mesh, IMesh, Tube, XMod, Crown, Cap, Skin, Tip, Shell, Even, PInf, Cocoon, Head, Braid, Spiral, Unknown };
ItemType itemTypeFromString(const QString& s);
QString itemTypeName(ItemType t);    // 'Group', 'Curve', ... as the outliner shows them
QString itemTypeIcon(ItemType t);
QString itemTypeKey(ItemType t);     // 'group', 'curve', … as the file writes it

struct Item {
  int id = 0;
  int order = 0;                         // place in the file's item list (the HTML version walked items in this order)
  ItemType type = ItemType::Unknown;
  QString typeStr, name;
  int parent = -1;                       // -1: a root
  bool visible = true;
  std::vector<int> children;             // group / cocoon
  QString color;                         // group colour '#rrggbb' or empty
  bool expanded = false;

  // curve
  std::vector<int> anchors;
  OvrMap ovr;
  bool closed = false, junc = false, para = false;
  int ring = -1;                         // the disc guide it belongs to
  QString conn;                          // crown connector: 'part' | 'low'
  std::vector<int> sharp;                // anchors where this curve alone breaks (a crown connector's corners): its handles there
                                         // zero; the other curves through the anchor (a lock attached there) keep theirs
  double round = 0, fair = 0;
  std::vector<std::array<int, 3>> path;  // an edge curve (EC): the stretches it runs along — [curve, from anchor, to anchor]
  bool hasPath() const { return !path.empty(); }
  bool hasRib = false;
  bool seg = false;                      // a network curve (new logic: cut at the forks)
  bool roundSet = false, fairSet = false;   // (round / fair given — a 0 too)

  // surface (wall between curves a and b)
  int a = -1, b = -1;

  // mesh ('mesh': converted geometry) / imesh ('imesh', 'head': an imported file)
  std::shared_ptr<PolyMesh> poly;
  std::shared_ptr<PolyMesh> base;          // an imported mesh: the file's geometry (poly — as shown, its modifiers applied)
  // an imported mesh inside a Subdivision group (raw "subdiv"): the smooth surface made of poly (the cage — what is picked
  // and edited); null — none. subdGrid: its own wire shown; subdDone: the levels made (fewer than asked — over the face cap)
  std::shared_ptr<PolyMesh> subd; bool subdGrid = false; int subdDone = 0;
  bool headTag = false;

  QJsonObject raw;                       // the record as read (unknown fields kept for saving)
};

// What changed in a picture, by tiles of 64 texels: a stroke projected over several locks touches islands spread over the whole
// texture — their bounding box is nearly all of it, the tiles are only what was painted. Runs: the tiles as rectangles (a row's
// runs of marked tiles, joined with the same run of the rows under it)
struct TileMask {
  static constexpr int T = 64; int w = 0, h = 0, tw = 0, th = 0; std::vector<uint8_t> t;
  void reset(int W, int H) { w = W; h = H; tw = (W + T - 1) / T; th = (H + T - 1) / T; t.assign(size_t(tw) * th, 0); }
  bool empty() const { for (uint8_t x : t) if (x) return false; return true; }
  void clear() { std::fill(t.begin(), t.end(), uint8_t(0)); }
  void mark(const QRect& r) { if (tw <= 0) return; const QRect c = r.intersected(QRect(0, 0, w, h)); if (c.isEmpty()) return;
    for (int y = c.top() / T; y <= c.bottom() / T; y++) for (int x = c.left() / T; x <= c.right() / T; x++) t[size_t(y) * tw + x] = 1; }
  void add(const TileMask& o) { if (o.tw != tw || o.th != th) return; for (size_t i = 0; i < t.size(); i++) t[i] |= o.t[i]; }
  std::vector<QRect> runs() const { std::vector<QRect> out; std::vector<QRect> open;
    for (int y = 0; y < th; y++) { std::vector<QRect> row; for (int x = 0; x < tw;) { if (!t[size_t(y) * tw + x]) { x++; continue; } int e = x; while (e < tw && t[size_t(y) * tw + e]) e++; row.push_back(QRect(x, y, e - x, 1)); x = e; }
      std::vector<QRect> next; for (QRect r : row) { bool joined = false; for (QRect& o : open) if (o.left() == r.left() && o.width() == r.width() && o.bottom() == y - 1) { o.setBottom(y); next.push_back(o); o = QRect(); joined = true; break; } if (!joined) next.push_back(r); }
      for (const QRect& o : open) if (!o.isNull()) out.push_back(o); open = next; }
    for (const QRect& o : open) if (!o.isNull()) out.push_back(o);
    for (QRect& r : out) r = QRect(r.x() * T, r.y() * T, r.width() * T, r.height() * T).intersected(QRect(0, 0, w, h));
    return out; }
};

struct CameraState { QString view = "persp"; Vec3 pos{0, 0, 5}; Quat quat; double zoom = 1; Vec3 pivot; bool valid = false; };

class Scene {
public:
  // (std::map: references stay valid while items / anchors are added — the network edits hold them across that)
  std::map<int, Item> items;
  std::map<int, Anchor> anchors;
  std::vector<int> roots;
  QJsonObject fileRaw;                   // the whole project record minus what's parsed (settings, refs, rooms, ...)
  CameraState camera;
  QString path;
  QString room = QStringLiteral("hair");     // the room shown: hair | topo (Topology: the head's meshes are meshes like any)
  int revision = 0;                      // bumped on every change (renderers rebuild what they cache)

  void clear();
  Item* item(int id) { auto it = items.find(id); return it == items.end() ? nullptr : &it->second; }
  const Item* item(int id) const { auto it = items.find(id); return it == items.end() ? nullptr : &it->second; }
  Anchor* anchor(int id) { auto it = anchors.find(id); return it == anchors.end() ? nullptr : &it->second; }
  const Anchor* anchor(int id) const { auto it = anchors.find(id); return it == anchors.end() ? nullptr : &it->second; }
  const std::vector<int>& childrenOf(int parentId) const;

  bool isVisible(const Item& it) const;   // the item and every group above it shown (and kept by an isolation)

  bool isVisibleBase(const Item& it) const;   // (without the Paint room's own hiding)

  bool noPaint(const Item& it) const;         // (the NoPainted label: on it or on a folder it is in)
  // isolation (Ctrl+H — the selection, Shift+H — and the head, Ctrl+Shift+H / Solo — the working group); runtime only
  struct Isolation { std::set<int> keep; bool head = false; int group = -1; bool solo = false; };
  std::optional<Isolation> iso;
  bool isoKept(const Item& it) const;
  bool isDescendant(int id, int ancestor) const;
  QString tintOf(const Item& it) const;   // the colour of the topmost coloured group above it (or empty)
  std::vector<const Item*> curves() const;
  Box3 bounds() const;                    // head / imported meshes, else the anchors

  // anchor id → curves using it (rebuilt by reindex(); CurveMath needs it to find shared anchors fast)
  std::unordered_map<int, std::vector<int>> curvesAt;
  void reindex();

  // new items and anchors: ids go on from the highest one of the file (the HTML version's UID), items after the last
  // one in order (a Map's insertion order)
  int nextId = 1, nextOrder = 0;
  // the references (Refs.h): the planes (in the undo history), the board (its items as the file keeps them)
  std::vector<RefPlane> refPlanes; QJsonObject refBoard; int nextRefId = 1;
  // the Paint room's map shown on the meshes: the picture (as the texture canvas shows it), its version, the UV set it is painted
  // in ("" — the project's single texture: every polygon) — not in the file, not in the undo history
  // (others: the UV sets painted before, not the one shown now — each one's picture as it was left, with its revision: the paint of
  // every set is seen on the model at once)
  // (flat: the map painted, its own copy — not shared with the canvas, so a stroke's next step does not copy 64 MB of a 4K picture;
  // full: the picture is new as a whole — the renderer uploads all of it; else dirty: the part changed since the renderer last took it)
  struct PaintView { QImage flat; qint64 rev = 0; QString set; bool on = false; bool full = true; TileMask dirty; std::map<QString, std::pair<QImage, qint64>> others;
    std::map<QString, TileMask> othersDirty; std::set<QString> othersFull; } paintView;   // (the other sets: the tiles changed since the renderer took them; full — the picture anew)
  // the shader mode (HairHarpy Toon): on / off (the button in Topology and Paint — not in the file), the library (fileRaw "toon",
  // read into lib on a change: rev), the toon's maps of each UV set made from the Paint room's layers (slot → the picture and its
  // revision; a null picture — the set has none of that map)
  struct ToonView { bool on = false; qint64 rev = 0; toon::Lib lib; std::map<QString, std::array<std::pair<QImage, qint64>, toon::SlotCount>> maps; qint64 mapsRev = 0;
                     std::map<QString, QImage> files; } toonView;   // (files: the pictures the materials name — the face SDF — read by path)
  std::map<int, RefSketch> refSketches; int nextCanvasId = 1;   // (the canvases, by their ids — RefPlane::canvas: not in the undo history)
  std::vector<RefMarker> refMarkers; int nextMarkerId = 1; bool refMarkersShown = true; qint64 refMarkRev = 0;
  Item& addItem(ItemType type, int parent, const QString& name);   // (appended to the parent's children)
  Anchor& addAnchor(const Vec3& p);
  void removeFromParent(int id);
};

// a curve of the network (new logic): not a service, ring, connector, closed or flat-lock curve
// a mesh Convert made (or an older file's converted mesh, not a preview): an ordinary mesh from now on — the Topology
// room, the Inspector, UV and Paint take it as any other; its geometry and polygroups kept as an edited mesh's (a packed
// source); what it was made of (fromCocoon, srcSurfs) stays in its record
void plainMesh(Item& it);

inline bool segNet(const Item& h) { return !(h.junc || h.hasPath() || h.ring >= 0 || !h.conn.isEmpty() || h.closed || h.hasRib); }

}  // namespace hm
