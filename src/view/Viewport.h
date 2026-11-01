// The 3D viewport: a QVulkanWindow (swapchain, depth, MSAA from Qt) with our own Vulkan pipelines — lit meshes, lines
// (curves, wireframes, floor, rings), anchor points. Redrawn on demand.
// Navigation: Alt+LMB orbit, MMB pan, Alt+RMB dolly, wheel — zoom toward the cursor, F — frame the selection.
// The Cursor tool (as the HTML version's): a click on an anchor selects it (Shift — adds, Ctrl — toggles) and a drag
// moves the selected anchors in the plane facing the camera (Shift, one anchor — it slides along the curve nearest the
// cursor; released near another anchor — the weld menu); a click on a curve adds / takes it off; on a wall, mesh or
// shell — selects it (Shift — adds); Ctrl + a stroke over curves — selects / deselects them; a drag over empty space —
// a lasso over the anchors (Shift — adds). RMB on a curve — a new anchor there, dragged at once; an RMB stroke over
// empty space — an anchor wherever it crosses a curve (the selected curves, if any), the shape kept.
// Every edit is committed as the HTML version does: the network kept up (cut at the forks, merged, joined, named) and a
// snapshot of the scene for Ctrl+Z / Ctrl+Y.
#pragma once
#include <array>
#include <atomic>
#include <unordered_map>
#include "scene/AutoFix.h"
#include "scene/Convert.h"
#include "scene/Crown.h"
#include "scene/Lobes.h"
#include "scene/LockExtrude.h"
#include "scene/MeshCurves.h"
#include "scene/Rib.h"
#include "scene/WallTools.h"
#include "scene/MeshMods.h"
#include <functional>
#include <optional>
#include "scene/Display.h"
#include "scene/HairState.h"
#include "scene/Scene.h"
#include "scene/Topo.h"
#include "scene/TopoEdit.h"
#include "scene/Uv.h"
#include "view/Camera.h"
#include "view/Gizmo.h"
#include "view/Picking.h"
#include <QColor>
#include <QDateTime>
#include <QJsonObject>
#include <QTimer>
#include <QVulkanWindow>
#include <map>
#include <set>
#include <string>

class QFileSystemWatcher;

namespace hm {
// (profiling: HM_PROF set — what took how long, appended to %TEMP%/hm_prof.txt)
void prof(const char* what, qint64 ms);
struct ProfScope { const char* w; qint64 t0; explicit ProfScope(const char* what); ~ProfScope(); };
namespace ui { class ThinkScope; }
struct SideGuard;                                        // (ViewportTopoTools.cpp: a reference's side the built mesh lies on)

class Viewport : public QVulkanWindow {
  Q_OBJECT
public:
  explicit Viewport(Scene* scene);
  QVulkanWindowRenderer* createRenderer() override;

  Scene* scene() const { return scene_; }
  Camera& camera() { return cam_; }
  // split screen (the HTML version's panes): up to 4 windows, each with its own camera — the one under the pointer is
  // the active one (cam_: its camera; the others keep theirs in panes_). Layouts: 2 · 2+1 · 1+2 · 4
  bool split() const { return split_; }
  const QString& splitLayout() const { return layout_; }
  void setSplit(bool on);
  void setSplitLayout(const QString& l);
  int activePane() const { return act_; }
  void activatePane(int i);
  std::vector<int> shownPanes() const;
  QRect paneRect(int i) const;                          // logical px in the viewport
  const Camera& paneCamera(int i) const { return i == act_ ? cam_ : panes_[size_t(i)]; }
  void setPaneView(int i, const QString& v);
  void swapPanes(int i, int j);
  void backToSingle(int pane);                          // one window again: this one, with its camera
  int paneW() const { return std::max(1, paneRect(act_).width()); }
  int paneH() const { return std::max(1, paneRect(act_).height()); }
  QPoint toGlobal(const QPointF& m) const;              // a point of the active pane → the screen
  QPointF fromGlobal(const QPoint& g) const;
  void snapToNearestView();                             // (Shift while orbiting: the nearest standard view)
  void viewFromAxis(int axis, int sign);                // the view axes' click: look along that axis (animated)
  QString paneLabel(int i) const;
  // reference planes (ViewportRefs.cpp): the one active in the 🖼 menu — the gizmo works on it (its pivot unlocked: on
  // the pivot only); Ctrl+Alt+drag moves its picture; selecting anything in the scene lets it go
  RefPlane* refPlane(int id);
  int activeRefPlane() const;                           // (-1 — none, or hidden)
  void setActiveRefPlane(int id);
  bool refPivEdit() const { return refPivEdit_; }
  void setRefPivEdit(bool on);
  void refPlanesEdited(bool commitNow);                 // (a change from the menu: drawn again; commitNow — an undo step)
  int refRevision() const { return refRev_; }
  const Camera& refHomeCamera(const RefPlane& rp) const;   // the pane the plane rests in (its view's, else the active one)
  std::function<void()> onFileDrag;
  std::function<void()> beforeHeavy;                     // (the main window's recovery copy, before a heavy operation)
  // the floor (a view preference, not the project's): height offset (a share of the model's height), opacity, at the
  // world origin, the world axes' opacity
  struct Floor { double dy = 0, op = 1, ax = 0.6; bool zero = false; } floor;
  struct ViewGrid { bool on = false; double step = 1; };   // (a view's snapping grid: shown, its step — scene units)
  ViewGrid viewGrid(const QString& view) const; void setViewGrid(const QString& view, const ViewGrid& g);
  double floorY() const;                                // (the floor's height as it is drawn)
  // the element snap: where the element taken hold of (at from) goes with the pointer at mp — a vertex, a point of an edge,
  // of a polygon of the shown meshes (not the moving vertices of mesh m), or the view's grid; none — nothing near
  std::optional<Vec3> elemSnap(const QPointF& mp, int m, const std::set<int>& moving, const Vec3& from);

  std::optional<Vec3> esnapHit_;                          // (the element snap's target now — drawn)
  void setFloor(const Floor& f);                      // (files dragged over the viewport: the main window's drop boxes)
  HairState& hair() { return hair_; }
  const Selection& selection() const { return sel_; }
  const DisplayState& display() const { return ds_; }
  const std::vector<ShownAnchor>& shownAnchors() const { return anchors_; }
  bool isItemSelected(int id) const { return sel_.hasItem(id); }
  int snapCurve() const { return ad_.snap; }
  struct Overlay { std::vector<std::pair<double, double>> pts; QColor color; bool closed; };   // (logical px)
  std::vector<Overlay> overlays() const;
  void uvPinOverlay(std::vector<Overlay>& out) const;   // (the UV Puppet's pins: a needle and a round head each, in screen pixels)
  void uvGizmoOverlay(std::vector<Overlay>& out) const; // (the UV Editor's Move / Rotate / Scale gizmo at the selection's pivot, in screen pixels)
  bool uvGizmoOn() const;                                // (shown: the UV mode, a component selection, the Move / Rotate / Scale tool)
  int uvGizmoHit(const QPointF& mp) const;              // (its handle under the pointer: 0 none, 1 U, 2 V, 3 the middle, 4 the ring)
  std::array<double, 2> uvGizmoPivot() const;           // (the selection's box middle)
  QPointF uvGizmoPx() const { const auto p = uvGizmoPivot(); return uvToPx(p[0], p[1]); }   // (where it is on the screen — checks)
  std::array<double, 4> uvFramePx() const { const auto b = const_cast<Viewport*>(this)->uvSelBox(); if (!b) return {0, 0, 0, 0}; const QPointF a = uvToPx((*b)[0], (*b)[3]), c = uvToPx((*b)[2], (*b)[1]); return {a.x(), a.y(), c.x(), c.y()}; }   // (the frame: left, top, right, bottom px — checks)
  double worldPerPixelAt(const Vec3& p) const;          // (the size of a screen pixel at p — scene units)
  void armOnce(const QString& a);                         // (a menu button: its action done by the next press in the viewport — the weld distance picked, the normals turned or copied)
  void esnapOverlay(std::vector<Overlay>& out) const;   // (the element snap: its target ringed; while a drag goes, the vertices / edges it could take near the pointer)

  void setSelectedItems(const std::vector<int>& ids);   // from the outliner
  void setSelectedAnchors(const std::vector<int>& a) { sel_.items.clear(); sel_.anchors = a; onSelChange(); }
  void setHoverCurves(const std::set<int>& ids);        // (a scheme's hover: pale lines over everything)
  // the Settings' sliders: what changed is rebuilt for the next frame, no undo step until commitLive (on release)
  void liveAnchors(const std::vector<int>& anchors);     // (their curves and the curves next to them)
  void liveWalls(const std::set<int>& walls);
  void liveCocoons(const std::set<int>& cocoons);
  void liveView() { staticRev_++; ringsDirty_ = true; requestUpdate(); }   // (only how it is drawn)
  void commitLive(bool topology = false);
  void selectItem(int id, bool add);                     // a click (Shift / Ctrl — add / take off)
  void sceneChanged(bool reframe);                       // after a load (reframe) / a visibility change
  void frameSelection();
  std::optional<Vec3> pickPoint(const QPointF& windowPos) const;   // (checks: the surface under a point of the window)
  void fitToBox(const Box3& box);                        // a new model framed (fitToBox: the scale, centre, a reset view)
  void setViewMode(const QString& v);
  void undo();
  void redo();
  // the history (the Inspector's History slider): each step's name, a jump to it, a step taken out — what it did undone in
  // every later step too (where a later step changed the same thing again, that later change stays)
  int historySize() const { return int(history_.size()); }
  int historyIndex() const { return histIdx_; }
  QString historyLabel(int i) const { return i >= 0 && i < int(history_.size()) ? history_[size_t(i)].label : QString(); }
  void historyGoTo(int i);
  bool historyDelete(int i);
  void noteStep(const QString& s, bool done = false);   // (the name of the step being made — done: of the one just made, unless it has one)
  void clearStepNote() { stepLabel_.clear(); }
  void convertSelected();                                // Convert to geometry (the selected cocoons)
  // Check connections (the selected cocoons — none: all): the places where locks to be welded don't meet, shown as pins
  // until cleared; report — the report window asked for (weldIssues)
  std::optional<LockWeldResult> checkConnections(bool report);
  const std::vector<WeldProblem>& weldSpots() const { return spots_; }
  int weldHot() const { return weldHot_; }
  void setWeldHot(int i) { weldHot_ = i; overlayRev_++; requestUpdate(); }
  void clearWeldSpots() { spots_.clear(); weldHot_ = -1; overlayRev_++; requestUpdate(); }
  void setWeldSetting(const QString& key, const QJsonValue& v);   // weld / lockWeld (a commit, as the HTML version's)
  // the loop settings (Loop step, Deviation, …): those of the selected cocoons' lock groups, else the project's
  QJsonValue convValue(const QString& key) const;
  void setConv(const QString& key, const QJsonValue& v, bool commitNow);
  std::vector<int> selectedCocoonGroups() const;
  // the tool in use: cursor, move, rotate, gizmo, brush, draw, cut, modifier, guide, crown, edge
  const QString& tool() const { return tool_; }
  void setTool(const QString& t);
  void lockExReset();                                    // the Lock Extrude's session ends: what it made stays as it is
  int createPrimitive(const QString& type);              // the "+" menu: a primitive (scene/Primitives) at the camera's pivot — selected
  void primSet(int id, const QString& key, double v, bool commitNow);   // a primitive's setting: the mesh made again (commitNow — an undo step)
  void primConvert(int id);                              // the primitive a plain polygonal mesh (its settings dropped)
  const lockex::Params& lockExParams() const { return lx_.P; }
  bool lockExActive() const { return lx_.on; }
  void lockExSet(const lockex::Params& P);               // (the panel's settings: the extrusion made again with them)
  void syncCursor();                                     // (a painting brush on: the arrow with the brush beside it)
  int cursorKind_ = -1;
  // the gizmo: World / Local, X / Y / Z locked; its lines for the renderer (every frame — they follow the camera)
  void setGizmoLocal(bool on);
  bool gizmoLocal() const { return gzLocal_; }
  void setAxisLock(int k, bool locked);
  bool axisLocked(int k) const { return gz_.lock[k]; }
  void updateGizmo();
  std::vector<Gizmo::Line> gizmoLines() const;
  std::optional<QPointF> gizmoScreen() const;            // (checks: where the gizmo's centre is on screen)
  // the brush: radius (px), hardness (0…1), mode (screen / surface — the project's settings.brushMode)
  double brushR = 70, brushHard = 0.3;
  QString brushMode() const;
  void setBrushMode(const QString& m);
  void brushResize(double k);                            // [ / ] (×0.85 / ×1.15), Ctrl + wheel
  void panelsLater();                                    // (the panels drawn anew once the wheel / drag rests a moment — not on every notch)
  QTimer* panelsLater_ = nullptr;

  // ◍ Shell modifiers (the 'guide' tool, ViewportShell.cpp): the tab — Density / Parent influence (painted along a
  // cocoon), Line / NURBS disc (guides); the brushes' state; what they go on (shellTargets)
  QString shTab = "tip";
  struct ShellBrush { bool on = false; double strength = 1; } tipBrush, pinfBrush;
  QJsonObject tipDefaults{{"dens", 1.5}, {"v", 2}}, pinfDefaults{{"str", 1}};
  bool shellBrushActive() const;                         // the Density / Influence brush paints (a modifier of it on a cocoon)
  void shellChanged() { overlayRev_++; requestUpdate(); }  // (the tab / a brush switched: what is shown)
  std::vector<int> shellTargets() const;
  std::vector<int> addShellMods(const QString& type, const QJsonObject& def, const QString& name);
  void clearMasks(const std::vector<int>& mods);
  void createDiscs();                                    // the disc tab's Create: across each targeted cocoon, else a scene disc
  QJsonObject guideDefaultsNow() const;                  // settings.guideDefaults with the radius filled (4 loop steps)
  void setGuideDefaults(const QJsonObject& d);
  void touchShellMods(const std::vector<int>& mods);     // (their cocoons rebuilt — live)
  struct MaskSeg { Vec3 a, b; float ca[3], cb[3]; };
  std::vector<MaskSeg> maskLines() const;                // the painted masks shown along the cocoons
  struct GuideDraw { std::vector<Vec3> pts; bool closed = false, sel = false, off = false; int id = -1; };
  std::vector<GuideDraw> guideLines() const;             // the guide lines (a ring round its cocoon) and the discs' rings
  int pickGuide(const QPointF& m) const;

  // 〰 Curve modifiers (the 'modifier' tool, ViewportMod.cpp): the tab (Profile / Extra), the profile being applied, the
  // weight brush (Mask 0…1, Thickness / Twist −1…1: paint / invert / erase / smooth; Add or Absolute), the built modifiers
  QString modTab = "profile", modPreset = "Quad";
  std::vector<std::array<double, 2>> modProfile;          // (empty: the preset's)
  struct WeightBrush { bool on = false; QString kind = "mask", mode = "add"; double strength = 1, intensity = 0, twist = 0.5; } maskBrush;
  bool weightBrushOn() const;                            // the Extra tab's brush paints (something of its kind is there)
  struct TubeDraw { int n = 0, NR = 0; std::vector<Vec3> pos; std::vector<std::array<float, 3>> col; int item = -1; bool guides = true, wire = false, ringLines = false; QColor color; double opacity = -1; std::shared_ptr<PolyMesh> mesh; };   // (mesh: a surface of its own polygons, not a grid of rings)   // (item: the braid a strand / band is of — its key is its own; guides: the lines along it; color: its own)
  const std::map<int, TubeDraw>& tubes() const { return tubes_; }
  int tubesRevision() const { return tubesRev_; }
  void liveTubes() { tubeSig_.clear(); overlayRev_++; requestUpdate(); }   // (a modifier's settings changed: rebuilt)

  // Convert window helpers (ViewportRef.cpp): the reference cube — its height becomes the loop step (drag it, its top face
  // — the height); ◎ Pick — a circle at the cursor, a click sets its diameter as the weld distance
  struct RefCube { Vec3 base; double size = 1, height = 1; };
  std::optional<RefCube> refCube;
  void toggleReference();
  void setRefHeight(double h) { if (refCube) { refCube->height = h; overlayRev_++; requestUpdate(); } }
  bool mergePick = false;
  double pickR = 12;
  void setMergePick(bool on);
  double pickWorld(const QPointF& m) const;              // (the circle's diameter in the scene's units there)

  // ⚠ dents of the selected walls (an anchor of a side jutting in toward the other side): red rings, a list in Settings
  const std::vector<walltools::Kink>& kinks() const { return kinks_; }
  // a guide's influence zone, shown for 1.5 s (Radius / Blend moved): light green — full, dark — none
  void showZone(int guide);
  std::vector<CocoonSystem::ZoneLine> zoneLines() const;

  // ▬ Flat locks (ViewportRib.cpp): the Curve tool's Lock mode (its width ribR — the circle at the cursor, px), the built
  // ribbons, the Extrude / Offset / Glue brush on the selected flat locks
  QString drawMode = "curve";
  double ribR = 30;
  struct RibBrush { bool on = false; QString kind = "ext"; double strength = 1, intensity = 0.5; } ribBrush;
  bool ribDrawOn() const { return tool_ == "draw" && drawMode == "lock"; }
  std::vector<int> ribCurvesSel() const;                 // the selected flat locks (not volume branches)
  bool ribBrushOn() const;
  struct RibDraw { std::vector<std::vector<Vec3>> rings; bool tube = false, pointTip = false; std::vector<std::array<float, 3>> col; };
  const std::map<int, RibDraw>& ribs() const { return ribs_; }
  int ribsRevision() const { return ribsRev_; }
  void liveRibs() { ribSig_.clear(); overlayRev_++; requestUpdate(); }
  void ribConvert(int c);

  // ╱ Edges (the 'edge' tool, ViewportEdge.cpp): on an imported / converted mesh's own polygon edges — LMB an edge,
  // double click the loop, Shift add, Ctrl deselect (drag — a box), Ctrl+Shift the shortest path; → curves; and
  // Mesh → curves & surfaces (its settings: m2c, kept in the app's settings)
  struct EdgeSel { int mesh = -1; std::vector<int> list; int last = -1; } edges;
  meshc::M2C m2c;
  void clearEdges();
  int convertibleMesh() const;                           // the selected imported / converted mesh, else the edges' mesh
  void edgesToCurves();
  void meshToCurves(int mesh);
  struct EdgeSeg { Vec3 a, b; bool hover; };
  std::vector<EdgeSeg> edgeLines() const;                // the selected edges (and the one under the pointer)

  // ▦ The Topology room (ViewportTopo.cpp, ViewportTopoEdit.cpp) — its own logic: the rooms are scenes of their own (their
  // items, history, selection, tool); Meshes (the group and all it holds) is shared, taken along on a switch. The
  // modes (object / poly / edge / vert) on the active mesh (the imported mesh selected last), the Cursor's picking,
  // loops, paths, the box, drags; the gizmo on the selected elements; every geometry change a new 'pmesh' source
  // (the file stays — undo goes back by itself); Snap (onto the other meshes, their snap masks) and Symmetry (X = 0)
  const QString& room() const { return scene_->room; }
  void switchRoom(const QString& name);
  void loadRooms();                                      // (a project opened: Hair shown, the other rooms wait as records)
  QJsonObject roomsJson() const;                         // P.rooms (the rooms other than Hair; Meshes kept once — with Hair)
  void hairItemsFor(Scene& out) const;                   // (saving from another room: the Hair room's items, Meshes as now)
  QString topoMode = "object";
  QString lastEditMode_ = "vert";                         // (the edit mode last used — PolyPen goes back to it from Object)
  struct TSel { int id = -1; std::set<int> f, e, v; int last = -1; int ver = 0; const PolyMesh* B = nullptr; };
  const TSel& topoSel() const { return tsel_; }
  // (uv: the UV mode; keepUv: Preserve UVs; selTool — what a drag of the Select Cursor does: cursor (paints the selection),
  // rect, lasso; fill / outline — a click selects a region / an open outline; selThrough — the hidden side too)
  int mirrorWeldAns = -1;                               // (tests: Mirror's weld question answered — 1 yes, 0 no)
  // (loop / ring: stop before a picked element, an n-gon, a triangle; soft: Soft Selection on — its radius in centimetres,
  // surface — only what is joined to the selection, volume — everything of the mesh within the radius)
  // Symmetry's settings (ViewportSym.cpp) — the Topology room's and the Hair room's: planar / topology / radial, the axes,
  // the space, where the plane lies (context), a point on the plane kept on it, the twin's tolerance (scene units), the
  // planes shown
  struct SymOpts { QString type = "planar", space = "local", context = "auto"; bool ax[3] = {true, false, false}; int radial = 6; bool restrict_ = true, planes = true; double tol = 1; Vec3 custom;
    QJsonObject toJson() const; void fromJson(const QJsonObject& o); } symo;
  struct SymSet { std::vector<Affine> T; std::vector<std::vector<int>> pr; std::vector<std::pair<Vec3, Vec3>> planes; bool radial = false; Vec3 axC, axN; double tol = 1; };   // (each mirror / turn, each vertex's twin under it; the planes: point, normal — the mesh's own space)
  SymSet symSet(int m, const std::vector<Vec3>& base) const;
  std::vector<int> symFirstPairs(int m, const std::vector<Vec3>& base) const;   // (the first mirror's twins — one partner)
  void symApply(const SymSet& S, std::vector<Vec3>& P, const std::vector<int>& moved, const std::vector<Vec3>& base) const;
  struct SymCache { QByteArray key; SymSet S; }; mutable SymCache symCache_;
  void symChanged();                                     // (a setting changed: found again, drawn again, kept)
  struct TSet { bool snap = false, sym = false, uv = false, keepUv = false, selThrough = false; QString selTool = "cursor";
                bool esnap = false; QString esnapMode = "vert"; double esnapPx = 14;   // (the element snap: what is dragged — the element taken hold of — onto a vertex / an edge / a polygon / the view's grid within esnapPx)
                bool loopStopSel = false, loopStopNgon = false, loopStopTri = false, soft = false; double softR = 2; QString softMode = "surface";
                // (the gizmo's axes: world, local, normal, camera, custom; the move brush: screen, surface, topo — the
                // object under its centre / the selection only; its smoothing strength; relax: the stars kept even)
                QString gzOrient = "world", brushMode = "screen"; double brushStr = 0.5; bool relaxStars = true; bool brushAll = false;   // (brushAll: every visible mesh under it — else only the active one)
                bool gzFree = false; double gzStep = 1; QString alAxis = "x", alTo = "sel";   // (the gizmo: a drag moves the gizmo itself; gzStep: its Shift step when moving, cm; Align: which axis / plane, to what)
                QString gzWhat = "all"; bool gzSlide = false;   // (the gizmo moves the selection as one / each component about its own centre (and normal); Move slides along the edges — in parallel)
                QString brushKind = "move"; } tset;                // (what LMB does with the brush: move / smooth / relax / inflate / shrink)
  void setGzOrient(const QString& o);
  // the mesh's own gizmo (Object mode): its place and axes kept in its Transform (p.cx…: the pivot, p.gq: the axes as a turn of
  // the mesh's own) — Alt + drag, Align; Bake makes them the mesh's own axes
  Quat gizmoAxes() const { return gzFrame(); }
  Quat meshOwnQ(int m) const;                            // the mesh's own axes (its Transform's turn), world
  Quat meshGizmoQ(int m) const;                          // its gizmo's axes as a turn of those (p.gq; none — the same)
  void storeMeshGizmo(int m, const std::optional<Vec3>& pivotWorld, const Quat& axesWorld);   // (one undo step)
  QString gzAlign(const QString& axis, const QString& to);   // axis x / y / z or the plane xy / yz / xz; to vertex / edges / normal / view / world
  // ---- the Inspector's highlights (ViewportInspect.cpp): on every shown mesh, both rooms ----
  // (planarTol: how bent a polygon must be to be lit — 0 any bend, 1 only the worst: a corner off its plane by up to half
  // its longest side)
  struct Insp { bool ngons = false, open = false, nonPlanar = false, nonManifold = false, interior = false, hard = false, doubles = false; double planarTol = 0.06;
                bool any() const { return ngons || open || nonPlanar || nonManifold || interior || hard || doubles; } } insp;
  void loadInspect();
  void inspChanged();                                    // (kept, drawn anew)
  struct InspC { const PolyMesh* key = nullptr; const PolyMesh* shown = nullptr; double tol = -1; int heavy = 0;   // (heavy: which slow checks were made — 1 the double walls, 2 the interior)
                std::vector<int> ngons, nonPlanar, open, nonMan, interior, hard, doubles; std::vector<Vec3> interiorPts; };
  mutable std::map<int, InspC> inspC_;
  const InspC& inspOf(int m) const;
  // ---- the Maranspector in the Hair room (ViewportInspectHair.cpp) ----
  struct HInsp { bool near = false, twisted = false, degen = false; double twistDeg = 30; } hinsp;
  void loadHairInspect();
  void hinspChanged();
  double weldDistance() const;                           // (Auto weld points: set, else a quarter of the loop step)
  struct ConvSum { int locks = 0, polys = 0, tris = 0, verts = 0; };
  ConvSum convertSummary();                              // what Convert would make of the selected cocoons
  QByteArray convKey_; qint64 convAt_ = 0; ConvSum convSum_;
  // ---- the move brush (ViewportTopoBrush.cpp): LMB moves what is under it, Shift smooths, Shift+Ctrl relaxes ----
  // (side: Symmetry on — the side of X the stroke is on; a vertex across it with a pair is left to the mirror)
  struct TBM { std::vector<Vec3> base, nrm; Affine M, Mi; std::vector<int> vs; std::vector<double> w; std::shared_ptr<const topo::Bvh> surf; std::vector<int> pairs; std::vector<std::array<float, 2>> uv0; int side = 0;
               bool offSide(int v) const { return side && size_t(v) < pairs.size() && pairs[size_t(v)] >= 0 && pairs[size_t(v)] != v && base[size_t(v)].x * side < -1e-9; } };
  struct TBrushDrag { int kind = 0; Vec3 hit0, n, nC; QPointF p0; double Rw = 0; std::map<int, TBM> meshes; bool moved = false; } tb_;
  void tbPress(const QPointF& mp, bool shift, bool ctrl, int kind = -1, double radPx = 0, int only = -1);   // (kind / radius / mesh forced: PolyPen's RMB smoothing)
  void tbMove(const QPointF& mp);
  void tbRelease();
  void tbModePie(const QPoint& global);                  // RMB: the brush's mode (move / smooth / relax / inflate / shrink)
  // ---- the modelling tools (ViewportTopoTools.cpp): tpen — PolyPen, tloop — Loop, tknife — Knife, tchain — Smooth
  // EdgeChain, tweld — Weld, tplane — Plane Cut, tbevel — Bevel / Extrude, tinset — Inset, tbridge — Bridge; their
  // settings (kept between runs: topo/model), what they draw, the last edit made again when its settings change ----
  struct TTools {
    double penWeld = 10, penFill = 60, penSmooth = 80;   // (px: auto weld; the fill's search; RMB held — the brush's circle)
    QString penBrush = "relax";                          // (PolyPen's RMB brush: relax / smooth / inflate / pinch — Ctrl+wheel)
    int loopCuts = 1; double loopPct = 0.5; bool loopParallel = false;
    double knifeWeld = 8; bool knifeThrough = false;
    double chainSmooth = 0; bool chainEqual = false;
    QString weldMode = "point";                          // point / pair
    QString weldPos = "mid";                             // (in pairs) mid — the middle; side — onto one side
    QString planeMode = "all"; int planeCuts = 1; double planeSpacing = 1; bool planeSel = false, planeSelCuts = false;
    double bevOffset = 1, bevBevel = 0; int bevSeg = 1; bool bevTop = true, bevBottom = false, bevSelSides = false, bevSelTop = true, bevSelBottom = false, bevSmart = false, bevProp = false;   // (bevSmart: the walls against the polygons round it left out — its rim there stays, stitched to them)
    double insOffset = 0.3, insDepth = 0; int insSeg = 1; bool insGroups = true; bool insScale = false; double insCircleR = 0.5; int insCircleN = 8; bool insCircleHole = false;
    int brSeg = 0; double brTension = 0; int brSpin = 0; bool brReverse = false, brKeep = false;
    QString fillMode = "quads";                          // (Fill Hole: quads / quadtri / fan / tris / ngon)
    int fillSub = 0; double fillFlow = 0;                // (its loops inside the hole; Edge Flow — 0 flat … 1 the shape carried on)
    double optWeld = 0.01;                               // (Optimize: vertices nearer than this welded — scene units, cm: 0.1 mm)
    bool fixSplit = true;                                // (FixMesh: the separate parts made meshes of their own)
    bool fixWeld = true, fixDoubles = true, fixManifold = true, fixOrient = true;   // (FixMesh: its steps — points in one place welded, doubled walls out, non-manifold sides split, each part turned one way)
    bool optMidSide = false;                             // (Optimize: the vertices in the middle of a side taken out too)
    QString optMode = "safe";                            // (Optimize: safe — no weld that makes edges of three polygons; merge — every weld, the inner walls it makes taken out)
    int mirAxis = 0, mirCount = 6; QString mirType = "mirror"; bool mirReplace = false, mirNormals = true, mirLocal = false;   // (mirReplace: the copy in place of the other side's region; mirNormals: the corners' own normals mirrored onto the copy)
    // (Mirror / the virtual symmetry: axis X Y Z, mirror / radial / radialMirror / radialHalf, copies)
    double normLen = 1;                                  // (the Normals tool: the shown normals' length, × a 20th of the mesh's size)
    bool normShow = true; double normOp = 1;             // (the shown normals: on; their opacity)
    double normSmooth = 0.6; QString normMode = "shape"; bool normHardBorder = true;   // (Smooth: how far the shape is smoothed; shape / sphere / face; the border hard)
    double normNose = 1.0;                                    // (Face: the middle band's share — the nose read; 0 — one ellipsoid)
    bool nBrush = false; double nBrushVal = 1.0;              // (the normals brush: on; the averaging's share it gives — 0 the standard shading, 1 fully averaged)
  } ttools;
  // the Normals tool's brush (ViewportTopoTools3.cpp): the normals under it given the averaging's share set (0 or Ctrl —
  // the standard shading back), Shift — smoothed into each other; the stroke one undo step
  bool nBrushOn() const { return tool_ == "tnormals" && ttools.nBrush; }
  std::map<int, Vec3> normTarget(const Item& it, const PolyMesh& d, const std::vector<int>& fs, bool ownN) const;   // (the averaged normals the mode makes)
  void nBrushStart(int m, char how);
  void nAlignDab(const QPointF& mp, bool full);
  void nAlignEnd();
  struct { int m = -1; bool on = false, moved = false; char how = 'p'; std::shared_ptr<PolyMesh> before; std::vector<std::array<float, 3>> cur, def;
           std::vector<Vec3> tgt; std::vector<char> hasT, ck; std::vector<std::vector<int>> vc, nb; } nw_;   // (ck: the corners it may change — the selected polygons')   // (vc: a vertex's corners; nb: its neighbours)
  // PatchTool (ViewportPatch.cpp): curves drawn on the references — their crossings the corners, the contours they close
  // (with the built mesh's open rim) filled with a grid. div — how many edges the curve makes; mode — how its vertices are
  // laid: 'e' evenly, 'a' adaptive (denser where it bends), 'm' as the opposite side of its patch (the step repeated)
  struct PatchCurve { std::vector<Vec3> a; int div = 6; char mode = 'e'; double mw = 0, aw = 0; double edgeLen = 0; std::vector<Vec3> L, la; QByteArray lk; bool nearLay = false, modeAuto = true; std::vector<int> vids; };   // (vids: an edge curve — Shift + drag along the open edges: its anchors those vertices of the built mesh, divided at each)   // (mw, aw — how much Match and Adaptive weigh against Equal, 0…1 — mode the button shown; modeAuto — Match while an end is on the mesh's rim, else Equal, till a mode is picked; L — on the surface as the view saw it when its anchors last changed; lk — those anchors; nearLay — from an older file without L: laid by the view once all of it is in sight)   // (edgeLen: drawn after another curve was picked — its edges as long as that one.s, the count from its length)
  struct PatchSt {
    std::vector<PatchCurve> c; int draw = -1, selC = -1, selA = -1; bool dragA = false, moved = false; QPointF press;
    qint64 lastT = 0, wheelT = 0; QPointF lastP;                     // (the last click: a second one soon after, near it — a double click)
    int trace = -1; std::vector<std::vector<int>> trNb; std::vector<Vec3> trW; std::vector<char> trOn;   // (Shift + drag: the edge curve being traced, the rim's neighbours, the vertices in the world; trOn — the rim it began on)
    bool ring = false; QPointF ringA;                                  // (Shift + drag off the open edges: a stroke across a part of the model — a ring round it there)
    bool zoneAlt = false, toDense = false, triFan = false, rimShort = false;               // (the other zone where two could be filled; detail to the denser side; rimShort: the mesh side taken by its shortest edges, not the rim)
    // (kind: p — Patch, the zones the curves close; l — Loft, a surface through the curves in turn, a curve drawn between two
    // a section between them (loftRows: the rows between two curves, by their pair — the wheel over that span); s — Strip, each curve
    // the middle of a band stripW wide, stripAcross polygons across it)
    char kind = 'p'; double stripW = 1.0; int stripAcross = 1; std::map<std::pair<int, int>, int> loftRows; std::vector<std::pair<int, int>> spanOf;
    QByteArray key; std::vector<Vec3> P; std::vector<int> vOf; std::vector<std::vector<int>> F;   // (the preview: points (vOf — a vertex of the built mesh, −1 new), polygons)
    std::vector<std::vector<Vec3>> sampled, zone; int target = -1, patches = 0; QString note; bool zoneChoice = false;   // (each curve on the surface; the rim used; zoneChoice — another zone can be taken)
    struct Step { std::vector<PatchCurve> c; int draw = -1, selC = -1, selA = -1; }; std::vector<Step> undo, redo; bool dragPushed = false;   // (Ctrl+Z / Ctrl+Y while it is in use: the curves' own steps)
    bool show = true; double fillOp = 0.14;                            // (the preview shown; its polygons' opacity — kept past a project opened)
    bool ordering = false; std::vector<int> order;                     // (Loft: the curves' order as clicked — ordering: the clicks set it now)
  } pt_;
  void patchOrderClick(int c);                           // (Loft's order: curve c next — clicked again: it and the ones after it out)
  struct ViewLabel { QPointF p; QString text; QColor c; };   // (a word on the active window — logical px of the pane)
  std::vector<ViewLabel> viewLabels() const;
  void patchChanged() { pt_.key.clear(); ttTouch(); emit topoState(); }
  void patchTouch() { pt_.key.clear(); ttTouch(); } void patchBuild(); void patchLoftStrip(); void patchLift(); bool patchSpanWheel(int steps);
  void setMouseAt(const QPointF& p) { mouse_ = p; }      // (the harness: where the pointer is taken to be)
  void patchRedetect();                                 // (Update: the zones found anew — Switch zone and the curves' picked modes back to automatic)
  QString patchCommit();                                 // (the preview made real: into the mesh being built, none — a new one)
  void patchDeleteCurve(); void patchDeleteAll();
  void patchPush();                                      // (the curves as they are — a step to come back to)
  int patchRimVertexAt(const QPointF& mp, bool ownRim = false) const; bool patchTraceStart(const QPointF& mp); void patchTraceMove(const QPointF& mp); void patchTraceEnd();   // (Shift + drag: an edge curve along the open edges)
  void patchRingEnd(const QPointF& a, const QPointF& b);   // (the stroke from a to b: the plane it and the view span cut through the model there — a closed curve round it)
  std::optional<Vec3> patchSnapAt(const QPointF& mp) const { return patchSnap(mp, -1); }   // (where a click would put an anchor — the harness)
  void patchSplitBy(int c);                              // (curve c ended: the curves it crosses, or its ends lie on, cut there — two curves of each)
  Vec3 patchOnRefsAt(const Vec3& p) const { return patchOnRefs(p); }   // (the harness: the nearest surface)
  void patchLoad();                                      // (the curves from the project — its patchCurves; the steps dropped)
  void patchStore();                                     // (the curves into the project — saved with it)
  bool patchUndoRedo(bool redo);                         // (→ a step of the curves taken back / again)
  // Lobe Brush (ViewportLobes.cpp): a mesh's polygons painted into lobes — the layers kept with the mesh (lobes: their
  // names, lobeF: each polygon's layer, lobeAct: the one painted, lobeFlip:
  // the lobes turned round); the active one white, the rest in their colours. LMB paints, Ctrl erases, Shift fills up to
  // the crevices, Ctrl+Shift picks the lobe under the pointer. The retopology the user makes on one lobe (in any tool)
  // shown on the other lobes (ghost), made real on Apply
  struct LobeGhost { int id = 0, mi = -1; bool mirror = false; std::vector<Vec3> P; std::vector<std::vector<int>> F; };   // (a copy on lobe id: mirrored or not, its points, its polygons)
  struct LobeSt { int m = -1; std::vector<int> lab; QString labKey; const PolyMesh* B = nullptr; bool painting = false, erase = false; QPointF last; int paintRev = 0; qint64 shown = 0;
    double sens = 0.5; bool overwrite = false;   // (sens: how readily a seam / hollow counts as a crevice, 0…1)
    const PolyMesh* cB = nullptr; std::vector<float> conc; std::vector<char> cr; double crSens = -1;   // (the crevices: each polygon's concavity, those taken as crevices at crSens)
    QByteArray drawKey; std::vector<std::pair<QColor, std::vector<Vec3>>> drawn; QString lastFill;
    bool ghost = true; int src = -1; QByteArray mapKey, ghKey; std::vector<lobes::LobeMap> maps; std::vector<LobeGhost> gh; std::vector<Vec3> ghLines; int ghTemplate = 0, ghFaces = 0; QByteArray fitKey; std::map<int, std::vector<lobes::Fit>> fits; } lb_;   // (the copies: of the mesh src, the template lobe, its polygons)
  int lobeTarget() const;                                // (the painted mesh: shown; −1 none)
  std::vector<QString> lobeNames(int m) const; int lobeActive(int m) const;
  const std::vector<int>& lobeLabels(int m);             // (each polygon's lobe — decoded once per change)
  std::vector<int> lobeCounts(int m);                    // (each lobe's polygons, by its number)
  void lobeSetActive(int k); QString lobeNew(); QString lobeDelete(int k); void lobeRename(int k, const QString& name); QString lobeClear();
  const std::vector<lobes::LobeMap>& lobeMaps(int m);   // (each lobe laid onto a square — made again when the lobes change)
  int lobeSource() const;                                // (the mesh being built: the picked one, not the high-poly; else the last)
  bool lobeFlipped(int m, int k) const; void lobeFlip(int k);   // (lobe k's copy turned round)
  void lobeGhostBuild(); QString lobeGhostInfo(); QString lobeApply();
  QString testLobeCrev(double q); QString testLobeTemplate(int src, int k); QString testNearVerts(int m, double px); QString testMirrorNormals(int m); QString testMirrorHalf(); QString testReplace(double x, double r); QString testMoveSel(const Vec3& d); QString lastReplaceInfo_; QString testPenFill(const Vec3& a, const Vec3& at); QString testFreePoint(const QPointF& mp);   // (tests: screen points)
  struct TTPt { int v = -1, ea = -1, eb = -1, anchor = -1; double t = 0; Vec3 w; };   // (a vertex / on the edge ea–eb at t / an anchor / free at w)
  void loadModelPrefs();
  void saveModelPrefs() const;
  bool isModelTool() const;
  bool selectsAsCursor() const { return tool_ == "cursor" || tool_ == "tbridge" || tool_ == "tchain" || tool_ == "tweld" || tool_ == "tshaper" || tool_ == "tslide" || tool_ == "tnormals" || tool_ == "toptimize" || tool_ == "tmirror" || tool_ == "tfixmesh"; }   // (the tools that select as the Select Cursor — its selection kind, its Ctrl)
  bool topoToolPress(QMouseEvent* e);
  bool topoToolMove(const QPointF& mp, Qt::KeyboardModifiers mods);
  void modifiersChanged();                               // (Shift / Ctrl pressed or let go: the tool's preview at the pointer made anew — no move needed)
  void refreshSceneBox();                               // (the head marked / unmarked: the centre and the size the tools take — every window's camera — from the head again, the view kept)
  int hoverGroup() const { return hoverGroup_; }        // (the Hair room's Cursor, Ctrl+Shift held: the group under the pointer — lit, a click picks it)
  int groupUnder(const QPointF& m) const;
  void groupHover(const QPointF& m, Qt::KeyboardModifiers mods);
  bool topoToolRelease(const QPointF& mp);
  bool topoToolRmb(QMouseEvent* e);                      // (a tool's own RMB / Shift+RMB)
  bool topoToolKey(int key, Qt::KeyboardModifiers mods); // (Esc, Enter, [ / ])
  bool topoToolWheel(int steps, Qt::KeyboardModifiers mods);
  int penBrushKind() const { const QString& k = ttools.penBrush; return k == "smooth" ? 1 : k == "inflate" ? 3 : k == "pinch" ? 5 : 2; }   // (tb_.kind of PolyPen's RMB brush)
  void topoToolOverlay(std::vector<Overlay>& out) const;
  QString modelReapply();                                // a setting changed: the last edit of this tool made again with it
  QString penDeleteAnchors();
  int penAnchorCount() const { return int(tt_.anchors.size()); }
  bool chainActive() const { return tt_.ec.m >= 0; }
  QString chainCreate();
  QString chainApply(bool commitIt);
  QString knifeCut();
  void kSave() { tt_.kUndo.push_back({tt_.kc, tt_.kcur}); tt_.kRedo.clear(); }   // (the knife curves before a change: Ctrl+Z brings them back)
  bool kUndoRedo(bool redo);
  int ttVertNear(int m, const QPointF& mp, double px) const;   // (the mesh's vertex nearest the pointer on screen within px, -1)
  QString ttState() const { return QString("chain %1 anchors %2 hov v%3 e%4-%5 a%6 drag %7 le %8 prev %9").arg(tt_.chain.size()).arg(tt_.anchors.size()).arg(tt_.hov.v).arg(tt_.hov.ea).arg(tt_.hov.eb).arg(tt_.hov.anchor).arg(tt_.drag).arg(tt_.le).arg(tt_.loopPrev.size()) + QString(" last %1 m%2 same %3 active %4").arg(tt_.lastOp).arg(tt_.lastM).arg(tt_.lastM >= 0 && scene_->item(tt_.lastM) && (scene_->item(tt_.lastM)->base == tt_.lastAfter)).arg(topoMesh()); }
  QString weldNow(const QPointF* at);
  QString bridgeNow();
  QString insetCircles();
  QString fillApply();                                   // Fill Hole: the hole(s) previewed filled
  QString tSelectNgons(bool add);                        // the active mesh's polygons of more than four sides selected
  // (ViewportTopoTools3.cpp) the polygons' normals, the mesh's cleaning, its symmetry
  QString tAlignNormals();                               // the selected polygons (none: all) turned to face one way — each piece outward
  QString tReverseNormals();                             // the selected polygons (none: all) turned round
  QString tRelaxMesh();                                  // Ctrl+R: the selected vertices (none: all) evened out on the surface, the rim kept
  QString tCollapseEdges();                              // the selected edges each into a vertex (joined ones — one)
  QString tSnapToGeom();                                 // the selected vertices (none: all) onto the reference meshes' surface
  QString tFixMesh();                                    // FixMesh: collapsed and doubled polygons out, parts that only touch cut apart, each part one way round, the parts made meshes (an option)
  QString tOptimize();                                   // vertices in the middle of a side between two polygons gone, those in one place welded
  QString testSymGrid(); QString testChain();
  QString tMirrorReplace(); QString tApplyIdentical();
  Affine mirFrame(int m) const;                          // (Mirror's frame: Local — the object's gizmo, its axes and centre, world; World — none)   // (Mirror → Replace: the selection in place of the other side; the selection onto every identical part of the high-poly)
  QString tMirrorMesh();                                 // a copy by the axis / round it (the side found by itself), the seam welded
  QString vsymCreate(const QString& type);               // the selected meshes into a group with a virtual symmetry (a preview)
  QString vsymApply(int group);                          // the group's symmetry made real geometry
  QString tNormalsSmooth();                              // the selected polygons smooth (one normal per vertex), hard at their border
  QString tNormalsSet(const QString& dir);                 // the selected vertices' normals one direction: +x … -z, cam (toward the camera), one (their average)
  QString tNormalsFromBack();                          // the selected edges' vertices: the normal of the back wall round them (the polygons turned away from the camera)
  int tSelectOpenEdges();                                // (checks: every open edge of the active mesh selected)
  QString tNormalsCopy(int from);                        // the normals copied from another mesh's surface
  int vsymGroupOf(int item) const;                       // (the group with a virtual symmetry holding it — −1)
  // ---- selection sets (an item's raw "selSets": {k: f / e / v, n: the count it was made for, d: the mask, name}) ----
  QString tSelSetCreate();                               // the selection → a new set of the active mesh
  QString histInfo() const { QString s = QString("idx %1 of %2:").arg(histIdx_).arg(history_.size()); for (auto& h : history_) s += QString(" %1/%2").arg(h.tsel.f.size() + h.tsel.e.size() + h.tsel.v.size()).arg(h.tmode); return s; }
  std::vector<int> tSelSetList(int m, int idx, char* key) const;   // a set's elements (none — made for other geometry)
  void tSelSetApply(int m, int idx, const QString& op);  // set / add / remove: the set picked (its mode taken)
  void tSelSetDelete(int m, int idx);
  bool tSelSetTouched(int m, int idx) const;              // something of the selection (its kind) is in the set — its button lit
  struct SsHover { int m = -1, idx = -1; } ssHover;     // (the set under the pointer in the panel: lit in the viewport)
  void setSsHover(int m, int idx) { if (ssHover.m == m && ssHover.idx == idx) return; ssHover = {m, idx}; topoRev_++; requestUpdate(); }
  // ---- Soft Selection ----
  void softKey(bool down);                               // B: a tap — on / off; held — the pointer's sideways drag sets the radius
  struct SoftAdj { bool on = false, moved = false, has0 = false; double x0 = 0, r0 = 0; } softAdj_;
  struct SoftW { int m = -1, ver = -1; const PolyMesh* B = nullptr; qint64 nv = -1, nf = -1; double R = 0; QString mode; std::vector<int> vs, near; std::vector<double> w; };
  mutable SoftW softW_;
  const SoftW& softOf(int m) const;                      // the vertices the soft selection moves too, their weights
  void setSelTool(const QString& t);
  struct TDisp { bool unlit = false, noTex = false, pdoOn = true; double op = 1, pdo = 0; } tdisp;   // (pdoOn: the PDO switched on — its amount kept when off)
  struct SmOpts { bool cover = true, show = true, thru = false, lasso = false; double dist = 0.02; } smo;   // (lasso: the brush draws outlines)
  QString tunit = "cm"; bool tlocal = false;   // (the table: Local — along the object's own gizmo axes, a component selection about its pivot)
  Affine meshFrame(int m) const;                         // (the object's own gizmo: its axes and pivot, world)
  void saveTopoPrefs() const;
  int topoMesh() const;                                  // the active mesh (−1)
  std::vector<int> topoMeshes() const;                   // shown imported meshes, not references
  bool topoHeavy(int m) const;
  void setTopoMode(const QString& k, bool quiet = false);
  char tKey() const { return topoMode == "poly" ? 'f' : topoMode == "edge" ? 'e' : 'v'; }
  void tselApply(int m, const std::vector<int>& list, const QString& op, char key = 0, bool sym = true);
  void heavyWork(const QString& what, const std::function<void()>& fn);   // (an edit of the selection: on a high-poly mesh the "Let me think" window over the wait)   // (sym false: not mirrored — a polygroup picked whole)
  void tselClear();
  std::vector<int> tSelVerts(int m, char key = 0) const;
  struct SelVC { int m = -1, ver = -1; char key = 0; size_t n = 0; const topo::TTopo* T = nullptr; std::vector<int> r; std::shared_ptr<const topo::TTopo> keep; }; mutable SelVC selVC_;
  int tSelectFaces(int m, const std::vector<int>& faces, const QString& op = "set");
  bool topoDeselect();                                   // Ctrl+D (→ handled)
  bool topoEscape();
  void topoDisplayChanged() { staticRev_++; topoRev_++; requestUpdate(); }
  int topoRevision() const { return topoRev_; }
  // the polygroups: picked with Ctrl+Shift (Object mode), the list's picks (its nodes), the one picked last in the list
  struct TgPick { int id = -1; const PolyMesh* B = nullptr; std::set<int> set; } tgPick;
  struct TgSel { int id = -1, gi = -1; } tgSel;
  struct TgList { int id = -1; std::set<qint64> set; qint64 last = -1; } tgList;
  int tgFocus = -1; struct TgRen { int id = -1; qint64 n = -1; } tgRen;
  std::set<int> smPickedGroups(int m) const;
  void topoChanged() { topoRev_++; requestUpdate(); }
  // the edits (each one an undo step; a message for the toast, "!…" — nothing done)
  QString tgDelete(int m, int gi);
  QString tgDuplicate(int m, int gi);
  QString tgMerge(int m, int gi, int gj);
  QString tgMoveSelHere(int m, int gi);
  QString tgAddGroup(int m, int after);                  // a new empty polygroup (in the list after that one; −1 — at the end)
  // the selection's edits and walks (ViewportTopoOps.cpp; each one undo step, a message for the toast, "!…" — nothing done)
  std::vector<int> tSelFaces(int m) const;               // the polygons the selection means (vertices / edges: those all of whose corners are picked)
  QString tDetachSel();                                  // the selected polygons a mesh of their own
  QString tSplitSel();                                   // the selected polygons no longer welded to the rest (their polygroup kept)
  QString tSplitToGroup();                               // the selected polygons into a new polygroup
  QString tHardEdges(bool hard);                         // the selected edges (polygons: all their edges; vertices: the edges between them) hard / soft
  QString tMelt();                                       // the selected edges (polygons: those inside them) melted — the polygons on both sides one; vertices dissolved
  QString tDeleteSel();                                  // Del in a component mode: the picked elements' polygons deleted (not the mesh)
  QString tMirrorGeo();                                  // a mirrored copy (across X = 0) in the same polygroup — welded at the middle when asked
  QString tGrowSel(bool shrink);
  QString tMirrorSel(int dir);                           // the selection of one side copied onto the other (+1: +X → −X)
  QString tConvertSel(const QString& mode);              // the selection carried into that mode, then the mode switched
  QString tSelectAll();                                  // Ctrl+A: every component of the mode
  QString tInvertSel();                                  // Ctrl+I: the selection inverted
  QString visApply(int m, const QString& hid);          // a visibility layer shown: its hidden polygons hidden (empty — the whole model); one undo step
  QString visSelect(int m, const QString& hid);         // a visibility layer's polygons (those it shows) selected
  QString tHideSel();                                    // H: the selected polygons hidden (not the mesh)
  QString tUnhideAll();                                  // every hidden polygon / polygroup of the room shown
  bool anyHidden() const;
  std::vector<int> fillRegion(int m, int f, bool inv = false) const;   // the polygons reached from f: up to a cut and the selection's frame (inv: the selected piece)
  bool topoClickTool(const QPointF& mp, bool shift, bool ctrl);
  std::vector<int> tPickable() const;
  std::set<int> tTouchedFaces(int m) const;              // the polygons the selection is on
  bool tIsolateSel();                                    // Ctrl+Shift+H in a component mode: only the selected shown / all back                    // the meshes a click can pick (a component mode: only the active one)   // Fill / Outline: a click (→ handled)
  std::vector<int> rimLoop(int m, int e) const;          // the open outline through a rim edge
  int hardEdgeNear(int m, const QPointF& mp, double px = 12) const;   // the hard edge nearest the pointer on screen (Select Hard Edges)
  std::vector<int> hardLine(int m, int e) const;        // the line of hard edges through e: on through each vertex two hard edges meet at
  int rimEdgeNear(int m, const QPointF& mp, double px = 40) const;   // the mesh's border edge nearest the pointer on screen (Outline: no need to hit it exactly)
  std::vector<int> rimAll(int m) const;                  // every border edge of the mesh (Outline: all its open holes and edges, near or far)
  QString tgDetach(int m, int gi);
  QString tgRename(int m, int gi, const QString& name);
  void tgSetVisible(int m, const std::vector<int>& gs, bool show);
  QString tGroupBorder(int m, int gi);
  QString tSelectIdentical(int m, int gi);
  QString tSelectSymmetric(int m, int gi);
  QString topoBakeXform(int m);
  void setMeshPivot(int m, const Vec3& c);
  Vec3 meshPivotLocal(int m) const;
  bool tgIsolateToggle();                                // Ctrl+H on picked polygroups (→ handled)
  void tgTreeEdit(int m, const std::function<void(std::vector<topo::TgNode>&)>& fn, bool commitNow = true, bool quiet = false);   // (quiet: only kept — no undo step, the panels not rebuilt)
  // the snap mask (m.smask: run lengths over its polygons)
  std::vector<char> smArr(int m) const;
  int smCount(int m) const;                              // (its polygons in the snap mask — counted once per mask)
  std::vector<int> groupCountsOf(int m) const;             // (polygons per polygroup — once per geometry)
  mutable std::map<int, std::pair<double, int>> smCount_; mutable std::map<int, std::pair<const PolyMesh*, std::vector<int>>> groupCnt_;
  void smSet(Item& m, const std::vector<char>& a);
  QString smCreate(int m);
  QString smRemoveGroups(int m, const std::set<int>& gs);
  QString smAddGroups(int m, const std::set<int>& gs);   // their polygons into the snap mask (kept as they are: no covered parts left out)
  QString smMirror(int m, int dir);
  QString smClear(int m, bool invert);
  QString smHiddenOff(int m);                            // the hidden parts (hidden polygroups, polygons hidden with H) taken off the snap mask
  QString smGroups(int m, bool add);                     // the picked polygroups added to / taken off the snap mask
  void smPickedOff(int m);                               // (a mask made of the picked polygroups: their picking and selection dropped)
  // the transform table: P / R / S of the active mesh, or the selection's centre / size (component modes)
  struct TtTarget { int kind = 0; int m = -1; std::vector<int> verts; std::vector<Vec3> cbase; };   // kind: 0 none, 1 obj, 2 comp, 3 nothing selected, 4 a group
  TtTarget ttTarget() const;
  std::array<std::optional<std::array<double, 3>>, 3> ttValues(const TtTarget& t) const;
  void ttSet(TtTarget& t, int r, int c, double v);
  void ttDone(const TtTarget& t);
  void carryUnder(int id, const Affine& D);              // (what is under an item — objects, groups — given its change D)
  void turnGroupsUnder(int id, const Quat& R);           // (only the axes of the groups under an object: a turn of it by R)
  // a mesh item from an edit-format record (Topology's import, a detached polygroup)
  int topoNewMesh(int parent, const QString& name, const PolyMesh& d, int after = -1);   // (after: put right under that item, when it shares the parent)
  // what the room draws over the active mesh (in world space): its edges, the selection, the hovered element, the
  // picked polygroups, the snap masks; and how the meshes look (the active one purple / orange, opacity, PDO, unlit)
  struct TopoDraw {
    struct Lines { std::vector<Vec3> p; QColor c; float w = 1; bool noPdo = false; float xray = 0; };   // (noPdo: drawn where it is — not pulled nearer with the active mesh, as the Topology Ghost on the references; xray: what hides it shown through, this faint)
    struct Tris { std::vector<Vec3> p; QColor c; float op = 0.3f; float lift = 0; };   // (lift: pulled this much nearer the camera — a proxy's triangles over the surface they stand for)
    struct Pts { std::vector<Vec3> p; std::vector<QColor> c; float size = 5; bool xray = false, noPdo = false; };   // (xray: over everything)
    std::vector<Lines> lines; std::vector<Tris> tris; std::vector<Pts> pts;
    int edgesOf = -1; QColor edgesC; float edgesW = 1;   // (every edge of this mesh: drawn from its own wire)
    int pointsOf = -1;                                   // (every vertex of this mesh: the renderer's buffer — the picked ones white)
  };
  const TopoDraw& topoDraw();
  void topoSymPlanes(TopoDraw& D, int m) const;         // (Show Planes: the planes / the radial axis over mesh m)
  // (the Hair room's symmetry: the dragged anchors' twins — mirrored as they move)
  struct HairSym { std::vector<Affine> T; std::vector<std::tuple<int, int, int>> tw; std::vector<std::pair<int, Vec3>> start; std::vector<std::pair<Vec3, Vec3>> planes; bool radial = false; Vec3 axC, axN; std::vector<int> twinIds() const; } hsym_;   // (tw: the dragged one's index, its twin, the map)
  HairSym hairSymTwins(const std::vector<int>& list) const;
  void hairSymApply(const HairSym& H);
  void hairSymPlanes(TopoDraw& D) const;
  void hairInspectDraw(TopoDraw& D) const;               // (the Hair room's highlights into the overlay)
  // (the Hair room's checks on: they follow every change of the hair — the anchors dragged, the walls built anew)
  long long hairInspKey() const { return room() == "hair" && (hinsp.near || hinsp.twisted || hinsp.degen) ? (long long)scene_->revision * 1000003LL + overlayRev_ : 0; }
  bool topoTwoTone(int id) const;                        // (the active mesh in a component mode)
  double topoOpacity(int id) const;                      // (the selected meshes: Display → Opacity)
  double topoPdo(int id) const;                          // (the selected meshes: PDO, a share of the distance)
  bool topoWire(const Item& it) const;                   // (a mesh's own wire in the room)

  // ▦ UV (ViewportUv.cpp) — the Topology room's UV mode (tset.uv): the active mesh with a checker by its UV, its seams;
  // the UV Editor (a window in the 'uv' view): the active mesh's UV (and the other selected meshes' — dim), picked and
  // moved by the mode (polygons, edges, UV points), Alt / MMB pan, Alt+RMB / the wheel zoom; the edits (seams, Unfold,
  // Pack, turns) — each one undo step. Preserve UVs (tset.keepUv): moved vertices keep their place in the texture
  struct UvOpts { bool checker = false, c3d = false, s3d = false, tex = true, stretch = false; double dispOp = 0.6; bool rotate = true, strUp = true, boxMid = false, strSpread = true; int warpN = 3; double density = 64, pad = 0.01, angle = 50, strAmt = 0.7; bool symUnfold = true, symTint = true, pivShells = false, diag = false, stretchAngle = false; double symAxis = 0.5; } uvo;   // (symUnfold: a shell lying across the mesh's X = 0 unfolded symmetric)   // (pivShells: the gizmo turns / scales each shell about its own middle) (the UV symmetry is the Topology one — tset.sym: the mirrored shells follow across U = symAxis; symTint: the −X mirror shells burgundy)   // (strAmt / strUp: Straighten — how far, upright; boxMid: the transform's place — the box's middle, else its corner)
  bool uvMode() const { return room() == "topo" && tset.uv; }
  void setUvMode(bool on);
  void setPreserveUv(bool on);
  void saveUvPrefs() const;
  int uvRevision() const { return uvRev_; }
  void uvChanged() { uvRev_++; staticRev_++; topoRev_++; overlayRev_++; requestUpdate(); }   // (an option: drawn anew)
  bool uvShown(int id) const;                            // (its UV in the editor, its checker in 3D: the active mesh, the selected ones)
  bool uvDispShown(int id) const;                        // (the object's UV checker / stretch in 3D — in every mode: the active mesh, the selected ones)
  static QColor stretchColor(double q);                 // (the stretch's colour: q — the UV area to the 3D area against the mesh's; red squeezed, grey right, blue stretched — full at ×0.4 / ×2.5)
  // ---- Topology Ghost (ViewportGhost.cpp): a source mesh's topology laid onto the other meshes — the references (else
  // every shown mesh but the edited one; their snap masks honoured) — a ghost of its polygons to build by: each vertex
  // along its smoothed normal (missed — the nearest point), relaxed on the surface; its confidence by how far it went.
  // Works with the source hidden. The Reveal Brush makes its polygons real; PolyPen and the moves snap to it ----
  struct GhostOpts { double dist = 4; int relax = 3, density = 1; bool trim = true, show = true; } gho;   // (dist: how far it reaches, in the source's edge lengths; density 1 / 2 — subdivided)
  struct Ghost { int src = -1; std::vector<Vec3> P; std::vector<char> ok; std::vector<float> conf; std::vector<int> fo, fv; std::vector<char> fok; std::vector<std::pair<int, int>> edges; double avgL = 1;
                 std::vector<int> vis; std::vector<std::pair<int, int>> visE; };   // (vis / visE: the points and edges of its polygons shown — what snapping and drawing look at)
  const std::vector<Ghost>& ghosts() const;              // (made anew when a source, a target or a setting changes)
  bool ghostAny() const;                                 // (a mesh is a ghost source)
  void loadGhostPrefs();
  void saveGhostPrefs() const;
  void ghostChanged() { ghostKey_.clear(); topoRev_++; overlayRev_++; requestUpdate(); }
  bool ghostVertAt(const QPointF& mp, double px, Vec3& out) const;    // (the ghost's vertex nearest the pointer on screen)
  bool ghostEdgeAt(const QPointF& mp, double px, Vec3& out) const;    // (the nearest point of a ghost edge)
  bool ghostFaceAt(const QPointF& mp, std::vector<Vec3>& out) const;  // (the ghost polygon under the pointer, the nearest)
  QString ghostInfo() const;
  bool uvPaneShown() const;
  void openUvEditor();                                   // a window shows the UV Editor (the screen split in two if it isn't)
  struct UvDraw {
    struct Lines { std::vector<std::array<float, 2>> p; QColor c; float w = 1; };
    struct Tris { std::vector<std::array<float, 2>> p; std::vector<QColor> c; float op = 0.3f; };   // (a colour per triangle)
    struct Pts { std::vector<std::array<float, 2>> p; std::vector<QColor> c; float size = 5; };
    std::vector<Lines> grid, lines; std::vector<Tris> tris; std::vector<Pts> pts; int texMesh = -1, texMat = -2; bool checker = false; float op = 1;   // (checker: the labelled checker under the UV — before the texture; op: its / the texture's opacity)   // (texMat: the picked UV Mat — its texture under the UV; −2 — the first one there is)
  };
  const UvDraw& uvDraw();
  const QByteArray& uvDrawSig() const { return uvSig_; }
  const UvDraw& uvHoverDraw();                           // (the element under the pointer: drawn over, rebuilt alone)
  const QByteArray& uvHoverSig() const { return uvHovSig_; }
  QString uvStatus();                                    // shells, turned over, the tile filled
  QString uvMarkSeams(bool on);                          // the selected edges (polygons: their outline)
  QString uvSew();                                       // the UV sewn along the selected edges: both sides' points brought together, no seam there
  QString uvSeamsByAngle(double deg);
  QString uvSeamsFromBorders();
  QString uvClearSeams();
  std::vector<char> uvMinusXFaces() const;               // Symmetry: the −X polygons with a mirror twin — burgundy
  QString uvAutoSeams();                                 // the closed shells of the selection (none — all): seams that open each into one piece
  QString uvUnfold(bool all, bool symPairs = false);       // (symPairs: a mirror pair's shells — one unfolded, the other its mirror; as with Symmetry on)
  QString uvSymmetrize();                                // Shift+Y: the selected side's UV put on its mirror (the mesh's X): across U = symAxis, a shell across the plane — its own middle
  std::vector<uv::Feature> uvFeaturesOf(int m) const;   // the face's features (lips, lids — raw "uvFeatures": [{a, b, c, s}]): their points face each other in UV
  QString uvFeatureKey(int mode);                        // L (0): the selected loop / the holes of the selected shells — open pairs; Shift+L (1) — closed (shut); Alt+L (2) — taken off
  void setSymmetry(bool on);                              // the Topology room's Symmetry (Shift+X) — the UV Editor's too: on / off, saved, told
  // the UV Editor's hotkeys mode (ViewportUvKeys.cpp): C U O W, Alt+V / Alt+T / Alt+Shift+C; pins; the hover preview
  bool uvKeys = false;
  char uvBrush = 0; double uvBrushR = 40, uvBrushK = 0.5;  // the UV brush on: 'r' Relax, 'p' Pin; its radius (screen px), the Relax's step
  void setUvBrush(char kind);                            // (the same again — off)
  QString uvBrushTest(char kind, double u0, double v0, double u1, double v1, bool mod);   // (tests: a stroke from UV a to b)
  void uvTestHover(int f) { thov_ = {}; thov_.m = topoMesh(); thov_.f = f; topoRev_++; requestUpdate(); }   // (tests: the pointer over polygon f in 3D — the UV preview)
  void setUvKeys(bool on);
  bool uvKey(int key, Qt::KeyboardModifiers mods);       // (→ handled)
  QString uvSeamKey();                                   // C: edges — seams; polygons — cut off into a shell of their own
  QString uvPinStraight(bool vertical);                  // Alt+V / Alt+T: straightened up / across and pinned
  QString uvUnpin();                                     // Alt+Shift+C
  uv::Pins uvPinsOf(int m) const;
  const std::vector<std::array<float, 2>>& uvPreviewLines();   // (the shell under the pointer in 3D as Unwrap would lay it: line pairs)
  struct { const PolyMesh* B = nullptr; int stamp = -1, cur = -1; std::vector<int> shell, mc; std::vector<std::array<float, 2>> lines; } uvPrev_;   // (mc: the mirror corners — a symmetric unfold previewed so)
  QString uvOptimize();                                  // Optimize (RizomUV's): the selection's shells (none — all) relaxed as they lie — no seams cut, not packed
  QString uvToRizom(bool ask = false, bool launch = true);                   // the active mesh to RizomUV (an OBJ, its UV); its saves bring the UV back (ask: pick rizomuv.exe again)
  QString uvFromRizom(const QString& path);
  QFileSystemWatcher* rzWatch_ = nullptr; int rzMesh_ = -1; qint64 rzStamp_ = 0; std::vector<int> rzTile_;   // (rzTile_: per polygon the RizomUV tile its UV Mat went to — its U that much along)
  QString uvPack(bool all);
  QString uvTurn(int quarters);                          // (+1: a quarter turn anticlockwise)
  QString uvFlip(bool u);
  QString uvSelectShells();
  QString uvStraighten(bool alongU);                      // the selected edges laid straight along U / V (their points)
  QString uvStraightenStrands(bool all);                  // hair strands: their shells unbent by uvo.strAmt (upright when uvo.strUp)
  QString uvRectangleStrands(bool all);                   // hair strands: their shells laid out as strips of one width (spread apart when uvo.strSpread)
  // the UV Editor's frame and warp (ViewportUvWarp.cpp): on the selection — a frame (drag inside: move; its corners / sides: scale,
  // the other side held — Shift: evenly; just outside a corner: turn about the middle) or a warp mesh (a grid of anchors
  // over the selection's box, uvo.warpN cells a side: drag an anchor — the UV bends smoothly with it, as Photoshop's warp)
  QString uvTfStart(char kind);                         // 'f' frame, 'w' warp (again — off)
  void uvTfStop();
  char uvTfKind() const { return uvTf_.on ? uvTf_.kind : 0; }
  QString uvTfTestDrag(int handle, double dxPx, double dyPx, bool shift = false);   // (checks: a drag of a handle — the frame's 0–9, the warp's anchor — by pixels)
  QString uvPuppetTest(int steps, bool bend = false, bool arc = false);
  QString uvTfInfo();                                    // (tests: the Frame / Warp / Puppet state)
  QString uvSymCheck() const;                            // (tests: the moving corners' mirrors against their mirrored places)
  bool uvSymAxisFromSel();                               // (the axis through the selection's middle)
  void uvSymApply(std::vector<std::array<float, 2>>& U) const;   // (the UV symmetry: the mirrored partners of the moving corners follow, across U = symAxis)
  struct UvSymMap { const PolyMesh* B = nullptr; size_t n = 0; std::vector<int> part; }; mutable UvSymMap uvSymMap_;   // (each corner's mirror corner — by the mesh's X symmetry)
  QString uvPuppetSynth(int fanAt);                      // (checks: a straight strip, pins at its ends and middle, the far end turned 60° round the middle; fanAt: a fan of thin triangles round the middle pin)                       // (checks: two pins at the ends, one turned 90° round the other — the edges' stretch, folded triangles)
  std::optional<std::array<double, 4>> uvSelBox();       // the selection's UV box (u0, v0, u1, v1) — none: nothing selected
  QString uvSetBox(double u, double v, double w, double h);   // the selection moved / sized to that box (u, v: its corner, or middle — uvo.boxMid)
  QString uvSnapEdge(const QString& side);              // left / right / top / bottom / centre / fit: the selection against the tile's edges, uvo.pad off them
  // lining up (ViewportUvAlign.cpp): the selection — UV points (Vertex / Edge mode) or whole shells (Polygon mode)
  QString uvAlign(const QString& side);                 // left / right / top / bottom / hmid / vmid: to the selection's edge / middle
  QString uvDistribute(bool alongU);                    // the selected shells spaced evenly between the outer two
  QString uvNudge(int du, int dv);                      // moved by texels of the texture (arrows; Shift — 10)
  QString uvGrid();                                     // the selected shells' quads as a grid
  QString uvStraightenLoop();                           // the selected chain of edges on a line, spaced as in 3D
  QString uvOrientEdge();                               // the shell turned so the selected edge lies level / upright
  QString uvOrientUpright();                            // the selected shells turned so the 3D up goes up V
  QString uvStack();                                    // the selected shells over the last picked one
  QString uvUnstack();
  QString uvSnapTogether();                             // along the selected seam: the smaller shell onto the bigger, sewn
  // the texel density (texture pixels per scene unit — cm): of the selected shells (−1 none); taken as the set's wanted
  // one; the selected shells scaled to the wanted one, each in its place
  double uvDensityOfSel();
  QString uvDensityGet();
  QString uvDensitySet();
  // the checks (ViewportUvAlign.cpp): the active mesh's shown UV, each UV Mat's tile on its own, for the set's texture —
  // kept till the UV changes; a kind's polygons selected (Polygon mode)
  const uv::CheckReport& uvCheck();
  struct { const PolyMesh* B = nullptr; int rev = -1; QByteArray sig; uv::CheckReport r; } uvCheck_;
  QString uvCheckSelect(const QString& kind);           // flipped / overlap / outside / padding / density / asym
  // each shell's own settings (raw "uvShellAttr": [{k: its anchor polygon's vertices, sorted; lock, noTurn, scale, group}]):
  // per polygon (all of a shell's alike); the selected shells' — lock / noturn (toggled), scale <x>, group (a new one), ungroup
  std::vector<uv::ShellAttr> uvShellAttrs(int m) const;
  QString uvShellSet(const QString& what, double v = 0);
  // ▦ UV Sets and UV Mats (ViewportUvSets.cpp): a mesh's materials — UV Mats (each a set of polygons with its own texture:
  // the texture manager's sets) — grouped into UV Sets (raw "uvSets": [{ name, mats: [mat names; "" — Default] }]): the
  // mats of one set share the 0…1 tile, overlapping (a head, a body, the arms, the legs — each its own texture); no
  // tiling. The picked set / mat (uvPicked): the UV Editor shows, picks, frames and packs only its polygons. A mat deleted
  // keeps its slot (raw "uvMatGone": its name) — the file's maps stay on their slots
  struct UvMat { int idx = -1; QString name; int faces = 0; };   // (idx −1: Default — the polygons with no material)
  struct UvSetL { QString name; std::vector<int> mats; int faces = 0; };   // (faces: the polygons its UV layout covers)
  std::vector<UvMat> uvMats(int m) const;
  mutable std::map<int, std::pair<QByteArray, std::vector<UvMat>>> uvMatsC_;
  std::vector<UvSetL> uvSets(int m) const;
  mutable std::map<int, std::pair<QByteArray, std::vector<UvSetL>>> uvSetsC_;   // (uvSets / uvMats: made once per geometry and its lists)
  int uvSetOfMat(int m, int mat) const;
  struct UvPickS { int id = -1, set = -1, mat = -2; } uvPicked;
  bool uvSetIso = false;                                 // (a picked UV Set isolated: only its shells shown in the UV Editor; else its polygons tinted, the rest shown too)
  std::set<int> uvSetsOfSel(int m) const;                // the UV Sets the selected polygons lie in (their rows lit)
  QString uvSetFromShells(int set);                      // the selected shells (whole) into a new set (−1) or into that one   // (mat −2: none — the whole set, or everything)
  void uvPickSet(int m, int set);                        // (again — let go)
  void uvPickMat(int m, int mat);
  std::vector<char> uvHidden(const Item& it, const PolyMesh& B) const;   // hidden, or out of the picked set / mat
  std::vector<int> uvMatFaces(int m, const std::set<int>& mats) const;
  std::vector<std::vector<int>> uvMatGroups(int m) const;   // the shown polygons (the picked set / mat) by UV Mat — Pack all / Unfold all: each mat its own tile
  int uvSelectMats(int m, const std::set<int>& mats);    // their polygons selected (Polygon mode)
  // the texture a UV Set is laid out for (raw "uvTex": { set name — "": the mesh's own, no set: { w, h, pad } }): its size
  // in pixels (a rectangle too: the UV kept texel-square in it), the padding between the shells and off the edges in pixels
  struct UvTex { int w = 2048, h = 2048; double pad = 8, td = 0; double asp() const { return double(h) / w; } };   // (td: the texel density wanted — texture pixels per scene unit (cm); 0 none)
  UvTex uvTex(int m, int set = -2) const;               // (set −2: the picked one; −1: the mesh's own)
  uv::PackOpts uvPackOpts(int m) const;                  // (Pack / Unfold / Optimize: that texture — its size, its padding, turns allowed)
  QString uvSetTex(int m, const UvTex& t);               // (the picked set's — else the mesh's own; an undo step)
  bool uvShowPad = false;                                // (the padding round each shell drawn in the UV Editor)
  QString uvSetNew(int m, const std::vector<int>& faces = {}, const QString& name = {});   // a new set; the selected polygons (or these) go into it, under that name (none — "UV Set n")
  QString uvSetApply(int m, int set);                    // the selected polygons' mats into the set (a part of a mat: a new mat)
  QString uvSetRename(int m, int set, const QString& name);
  QString uvSetDelete(int m, int set);                   // its mats to the first other set
  QString uvMatNew(int m);                               // a new mat in the picked set (else the first); the selected polygons get it
  QString uvMatApply(int m, int mat);                    // the selected polygons get the mat
  QString uvMatRename(int m, int mat, const QString& name);
  QString uvMatDelete(int m, int mat);                   // its polygons to Default
  QString uvMatToSet(int m, int mat, int set);
  QString uvSetDuplicate(int m, int set);               // a copy of its UV layout (its own from then on), right under it
  QString uvSetMerge(int m, int into, int other, bool otherFirst);   // one layout of both — where they overlap, the UV of the one first (otherFirst: of other)
  QString uvSetMove(int m, int from, int to);            // its place in the list (the one higher wins a merge)
  QString uvSetTakeOut(int m, int set);                  // the selected polygons out of the set's layout
  QString uvMatMerge(int m, int from, int into);         // into's polygons get from (it wins); into goes
  void topoMeshEdited(int m, std::shared_ptr<PolyMesh> nb, const std::shared_ptr<PolyMesh>& old);   // (an edit that keeps the topology: written, one undo step)

  // ◠ Crown (the 'crown' tool, ViewportCrown.cpp): a click on the head places one (its whorl there), Create fits one on
  // the top of the head; the built crowns' meshes; new crowns symmetric (the tool's option)
  bool crownSym = true;
  bool crownBangs = false;                               // (the preset of new crowns: the bangs — the crown turned round, shorter)
  bool crownCont = false;                                // (new crowns made with their continuation)
  void crownContinue(int k);                             // the continuation: curves and walls down from the lower connector
  void createCrownAt(const std::optional<QPointF>& m);  // (nullopt: fitted on the top of the head)
  const std::map<int, crown::CrownMesh>& crownMeshes() const { return crowns_; }
  int crownsRevision() const { return crownsRev_; }
  void crownSymAsk(int k, const QPoint& global);        // which side is copied onto the other — a pie

  // ✂ Scissors (ViewportCut.cpp): Curve — RMB picks curves, a stroke across them → the pie (split / delete the upper /
  // the lower part); Split ends — a click on a curve: from there to its tip it becomes a Y (the spread: splitR px)
  QString cutMode = "curve";
  double splitR = 30;
  void setPieHost(QWidget* w) { pieHost_ = w; }
  bool startFillAt(const QPoint& global);               // ◎ Detect the fill outline here (ViewportFill.cpp)
  // a pie item's preview: lines drawn over everything — A cyan (what is there), B orange (what the item does)
  void setPieViz(const std::vector<std::vector<Vec3>>& a, const std::vector<std::vector<Vec3>>& b) { pieVizA_ = a; pieVizB_ = b; overlayRev_++; requestUpdate(); }
  std::vector<int> snapSrc;                              // (⇶ Snap to path: the curves last copied with ⧉ SEL)
  std::optional<Vec3> liftOffHead(const Vec3& mid, double span) const;   // (a new curve's middle point kept off the head)
  bool lockSel = false;                                  // ⧉ Lock select: a click on a curve / wall picks every curve of its lock
  Box3 baseBox(const PolyMesh* B) const;                 // (a geometry's box — kept)
  struct BoxC { const PolyMesh* B; int rev, srev; size_t n; Box3 box; }; mutable std::vector<BoxC> boxCache_;                                      // (tests: where a step's time went)
  bool hairSnap = false;                                 // (the Hair room's Snap: dragged anchors stick to the reference meshes)
  void snapAnchors(const std::vector<int>& list);
  bool keyB = false;                                     // (B held: LMB drag — the selected anchors' smoothness)
  // NURBS surface mode (ViewportSurfCut.cpp): the wall being cut, the cut's ends on its curves and its anchors on it
  bool surfCutting() const { return scut_.has_value(); }
  bool surfCutEdgeReady() const { return scut_ && !scut_->pts.empty() && (scut_->edge || scut_->start); }   // (anchors on the wall: Enter cuts — ends not on a curve carried to the edges)
  bool surfCutFinish();
  QString surfCutStartAt(double r, double t);            // (the harness: the start put on a curve at the wall's row r, across t)                                  // (Enter / the panel's Cut: the cut as it stands; nothing yet — a hint)   // (a cut begun on the wall / its edge: Enter cuts it from edge to edge)
  int surfCutWall() const { return scut_ ? scut_->sf : -1; }                 // (the wall being cut and its two curves: violet, as in the HTML version)
  bool surfCutCurve(int c) const { return scut_ && (scut_->A == c || scut_->B == c); }
  QString surfCutInfo() const;                           // (checks: the NURBS cut's state)
  void surfCutSetPts(int sf, const std::vector<std::pair<double, double>>& rt);   // (checks: a cut from edge to edge on wall sf, its anchors in the wall's coordinates r, t — as fractions 0…1)
  bool surfCutReady() const;
  void applySurfCut();
  void cancelSurfCut();
  bool openSurfCutPie(const QPoint& global);           // (Shift+RMB while cutting: only the cut's menu)
  bool surfCutKey(int key);                            // Enter / Backspace / Esc while cutting → handled
  void startSurfCut(int sf);
  struct SurfCutEnd { int side = 0, c = -1, a = -1, seg = 0; double u = 0; Vec3 pos; std::map<int, Vec3> vh; };   // a < 0: not put in yet
  struct SurfCutQ { double r = 0, t = 0; };
  struct SurfCutState { int sf = -1, A = -1, B = -1; std::optional<SurfCutEnd> start, end, hoverEnd; std::optional<SurfCutQ> hoverPt; std::vector<SurfCutQ> pts; bool edge = false; };   // (edge: begun on the wall / its top or bottom edge — the cut runs from edge to edge, its ends carried there)
  QString surfCutWhere(const QString& what, const SurfCutEnd* E, std::optional<SurfCutQ> q) const;   // (a cut's click: where it went — the status line)

  // edits (ViewportEdit.cpp): each commits an undo step
  void deleteSelected();                                 // Delete: the selected anchors, else the selected items
  void deleteAnchors(const std::vector<int>& anchors, bool keepShape);
  void recalcAnchors(const std::vector<int>& anchors);   // ↺ Recalculate handles
  void mergeCoincidentCurves(const std::vector<int>& curves);
  void reverseCurves(const std::vector<int>& curves);
  void createEmptyGroup(const Vec3* pivot = nullptr);  // (pivot: the group's own point — where the gizmo stands when it alone is picked; moved with it)
  void setGroupPoint(int gid, const Vec3& p, bool carry);   // (the group's own point to p; carry — its contents go along)
  // collision (the Hair room): a mesh's shape for the drawn curves — its geometry, a capsule round it or another mesh — and the lift off it
  static double collOffset(const Item& it); static QString collType(const Item& it);
  struct CapMesh { const PolyMesh* src = nullptr; size_t n = 0; double box = 0; std::array<double, 6> prm{}; std::vector<Vec3> pos; std::vector<uint32_t> tris; Box3 bx; };
  const CapMesh& capsuleOf(int id) const;
  // the capsule as a mesh of its own (raw collCap: the mesh it stands for): placed, turned, scaled with the gizmo as any
  // mesh; not drawn — its rings in orange; never picked in the viewport (select it in the outliner or the panel)
  int capsuleItem(int owner) const;                      // (−1: none — the fitted capsule)
  int makeCapsuleItem(int owner, bool refit);            // (made, or its shape made again from the settings — refit: its place reset too)
  std::vector<Vec3> capsuleWorld(int cap) const;         // (its points where the gizmo put them)
  std::vector<std::vector<Vec3>> capsuleRings() const;   // (the selected capsule's — drawn in the world)
  std::vector<PickMesh> collisionMeshes(bool withHead, std::map<int, double>& off) const;
  bool showColl = false;                                 // (the collision shapes shown — while the mesh's Collision block is open)
  static Item& newGroup(Scene& s, int parent, bool topoRoom);   // (an empty "Group N" at 0, 0, 0 — in the Hair room's Curves column a colour of its own)
  std::optional<Vec3> selectionCentre() const;           // (the selected vertices' / polygons' middle, else the picked objects' — the gizmo's place)
  std::vector<int> mergeableMeshes() const;              // (the picked meshes that can be merged into one)
  QString mergeMeshes(const std::vector<int>& ids, const Vec3& pivot, const std::optional<Quat>& axes = std::nullopt);   // (one mesh of them — world space, their polygroups kept — its pivot there, its gizmo's axes those given)
  // any edit of the scene: fn changes it (and the selection), → a message for the toast ("" — none; "!…" — nothing done)
  void runEdit(const std::function<QString(Scene&, Selection&, const CurveCache&)>& fn, bool topology = true);
  void toggleHide();                                     // H
  void toggleIsolate(bool withHead);                     // Ctrl+H / Shift+H
  void toggleIsolateGroup();                             // Ctrl+Shift+H
  int workingGroup() const;
  // ◎ Solo: Ctrl+Shift+H that follows the selection (the group you work in is the only one shown); kept in the settings
  void setSolo(bool on);
  bool solo() const { return solo_; }
  QString soloTopo = "mesh";                             // the Topology room's Solo: mesh — only the selected meshes; folder — the selected one's folder; high — the mesh and its high-poly (the references under it)
  void setSoloTopo(const QString& k);
  std::set<int> highPolyOf(const std::set<int>& meshes) const;   // the references the meshes lie on (most of their vertices near the reference's surface)
  bool escape();                                         // Esc (the drawing finished, …) → handled
  void finishDrawing();
  int drawingCurve() const { return drawing_; }
  // Auto-fix (the report's button): the places tried, the fixes kept shown (blue — after, orange — before) for review;
  // → the fixes made. Any other edit ends the review (the fixes stay as they are shown)
  int autoFix(const std::vector<WeldProblem>& problems, const std::function<void(double)>& onStep);
  bool reviewing() const { return wfix_.has_value(); }
  const std::vector<WeldFix>& fixes() const { static const std::vector<WeldFix> none; return wfix_ ? wfix_->fixes : none; }
  void fixShow(int i, bool on);                          // before / after of one place
  void fixDecide(int i, bool accept);
  void fixAll(bool accept);
  void endFixReview();                                   // (the report closed / checked again: kept as shown)
  int changeCount() const { return changes_; }          // (proj.change: every commit, undo, redo)
  QJsonObject cameraJson() const;                        // the project's camera record { view, pos, quat, zoom, pivot }

  // for the renderer, read and cleared once per frame
  void beforeFrame();                                    // pending edits → rebuilt walls / cocoons
  int staticRevision() const { return staticRev_; }
  int overlayRevision() const;                         // (the UV Puppet's pins shown: the UV view's pan and zoom too — they stay on their points)
  bool takeAllWalls() { bool b = allWalls_; allWalls_ = false; return b; }
  std::set<int> takeDirtyWalls() { std::set<int> s; s.swap(dirtyWalls_); return s; }
  bool takeRingsDirty() { bool b = ringsDirty_; ringsDirty_ = false; return b; }

  bool showWire = false;
  int framesDrawn = 0, lodFrames = 0;                    // (checks: frames made, of them with a LOD shown)
  // the frame counter (the overlay's, top right): when the last frames were made, how long the last one took
  void noteFrame(double ms) { lastFrameMs = ms; frameAt_[size_t(frameHead_++ % frameAt_.size())] = QDateTime::currentMSecsSinceEpoch(); }
  double lastFrameMs = 0;
  double fps() const {                                   // (over the last second of frames; 0 — none lately: the view rests)
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); int n = 0; qint64 first = now;
    for (qint64 t : frameAt_) if (t && now - t <= 1000) { n++; first = std::min(first, t); }
    // (a few frames now and then — a click, a hover: the view at rest, drawn on demand — no rate to speak of: the frame's own cost tells)
    if (n < 6 || now - *std::max_element(frameAt_.begin(), frameAt_.end()) > 250) return 0;
    return (n - 1) * 1000.0 / std::max<qint64>(1, now - first);
  }
  std::array<qint64, 240> frameAt_{}; qint64 frameHead_ = 0;
  void spinFrame(bool end = false) { spinNav_ = !end; if (end) requestUpdate(); }   // (checks: the camera moved by a script — navigating)
  bool spinNav_ = false;
  // heavy meshes (ViewportHeavy.cpp): a LOD shown while the camera moves, a hierarchy for picking — made in the background
  static long long lodTris();                            // (1 000 000 triangles; HM_LOD_TRIS)
  bool navigating() const;                               // (orbit / pan / dolly / the wheel just now / an animated turn)
  std::shared_ptr<const PolyMesh> lodOf(int id) const;  // (ready and still of its geometry — the file's space)
  int maskOccluded(int m, int g) const;                  // (tests: masked polygons of polygroup g hidden from the camera by a surface in front)
  QString testDoubles(int m); QString testHoleLoops(int m); QString testNonManifold(int m) const; QString testHardSpot(int m) const;   // (tests: the holes Fill Hole finds — their sizes)
                            // (tests: the double walls of mesh m — their polygons, corners)
  QString testProxyParts(int id) const;                  // (tests: the small separate parts of a high-poly mesh — how many its mask proxy keeps paintable)
  int maskProxyTris(int id) const;                      // (tests: the snap mask's proxy of a high-poly mesh — its triangles, 0 none yet)
  // a drag's change for the renderer: the shown copy (poly — prev: the one it replaced, same content) and where it changed
  struct LiveMove { int m = -1; std::shared_ptr<PolyMesh> poly; const PolyMesh* prev = nullptr; std::vector<int> verts, corners; };
  std::vector<LiveMove> takeLiveMoves() { std::vector<LiveMove> v; v.swap(liveMoves_); return v; }
  std::vector<LiveMove> liveMoves_;
  // (checks: the selected vertices dragged by steps × dist along X, each step drawn; → ms per step, the step's parts)
  QString testDrag(int steps, double dist);
  QString testExtrude() { const int m = topoMesh(); return m >= 0 && tExtrudeSel(m) ? QString("extruded") : QString("!"); }   // (tests: Ctrl + a gizmo handle's extrude)
  QString testRevealAll() { const auto& Gs = ghosts(); for (size_t g = 0; g < Gs.size(); g++) for (size_t f = 0; f < Gs[g].fok.size(); f++) if (Gs[g].fok[f]) revealFaces_.insert({int(g), int(f)}); return revealApply(); }   // (tests: the whole ghost revealed)
  // the model's numbers (the overlay's card, bottom left): the selected meshes as shown — none selected: every shown
  // mesh; the edges counted in the background for a big mesh (edges < 0: not yet)
  struct MeshStats { long long meshes = 0, verts = 0, edges = 0, faces = 0, quads = 0, tris = 0, ngons = 0, triangulated = 0; bool all = true; };
  MeshStats modelStats(); QString pickedInfo() const;   // (pickedInfo: what is picked — "3 vertices", "1 polygon"; the stats card's last line)
  struct StatSlot { std::weak_ptr<PolyMesh> poly; std::shared_ptr<std::atomic<long long>> edges; long long quads = 0, tris = 0, ngons = 0; };
  std::map<const PolyMesh*, StatSlot> statEdges_;
  int heavyPending() const { int n = 0; for (auto& [id, s] : heavy_) n += !s.bvh || (s.job && s.job->wantLod) || (!s.lod && s.wantLod); return n; }
  bool showAllCurves = false;                            // curves of every group, not just those being worked on
  QString weldChoice;                                    // (checks without a user: weld / reverse / join / none — taken without the menu)

signals:
  void statusText(const QString& s);
  void viewChanged(const QString& view);
  void selectionChanged(const std::vector<int>& items);
  void sceneEdited();                                    // items came / went / were renamed: the outliner anew
  void changed();                                        // the change count moved (the title's *)
  void weldIssues(const std::vector<hm::WeldProblem>& problems, bool report);   // Check connections / Convert found these
  void pieRequested(const QPoint& global);               // Shift+RMB: the actions pie
  void opsPieRequested(const QPoint& global);            // Ctrl+RMB (the Topology room): the edits of the selected
  void brushChanged();                                   // radius / hardness changed in the viewport
  void toolChanged(const QString& tool);
  void soloChanged(bool on);
  void topoState();                                      // (Topology: the mode, the selection, the picked polygroups changed)
  void roomChanged(const QString& room);
  void historyChanged();                                // (a step made, undone, jumped to or taken out)
  void historyOverwritten(int n, bool bySlider);                        // (an edit made back in the history: the n later steps gone)

protected:
  void mousePressEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  QPointF paneOrigin() const { return paneRect(act_).topLeft(); }
  void choosePane(const QPointF& windowPos);           // (no drag going on: the pane under the pointer becomes active)
  void resizeEvent(QResizeEvent* e) override;
  bool event(QEvent* e) override;

private:
  struct AnchorDrag { std::vector<int> list; std::vector<Vec3> starts; Vec3 ref, n; std::optional<Vec3> startHit; bool moved = false, inserted = false; int snap = -1; };
  // (with the project's weld settings and marked places: weld, lockWeld, weldOk, weldSnap — undone as edits)
  struct Snapshot { std::map<int, Item> items; std::map<int, Anchor> anchors; std::vector<int> roots; Selection sel; QJsonObject weld; std::vector<RefPlane> refPlanes;
                   TSel tsel; QString tmode; std::shared_ptr<const void> chain; std::shared_ptr<const void> patchUsed; QString label; bool weak = true; qint64 at = 0; };   // (label: what the step was — weak: guessed from the tool)   // (tsel, tmode: the Topology room's selection — undone too; chain: the Smooth EdgeChain curve as it was)
  bool tselPending_ = false;
  void tselNote();                                       // a selection change: one undo step once the gesture is over
  void tselPush();
  bool pickWeldSpot(const QPointF& m);                   // RMB on a pin: its menu (Ignore / Weld again)
  std::vector<WeldProblem> spots_;
  int weldHot_ = -1;
  // the brush (ViewportBrush.cpp)
  struct BrushCap { int a; double w; Vec3 start; std::optional<Vec3> hit0; Vec3 cen; };
  struct BrushDrag { std::vector<BrushCap> cap; std::vector<std::tuple<int, int, int>> mir; Vec3 n;   // (mir: Symmetry — cap i's mirror k lands on cap j: j moved by the mirrored stroke too)
    QPointF last, start; double dist = 0, r0 = 0, h0 = 0; int sign = 1; bool moved = false; };
  BrushDrag bd_;
  QPointF mouse_;
  int hoverGroup_ = -1;
  mutable std::map<int, CapMesh> capCache_;
  mutable std::map<int, std::vector<Vec3>> capWorld_;      // (a placed capsule's points in the world — while the collision uses them)
  bool inViewport_ = false;
  std::vector<int> brushAnchors() const;
  std::optional<Vec3> brushCenter3D(const QPointF& m, const std::vector<int>& list) const;
  std::vector<std::pair<int, double>> brushCapture(const QPointF& m) const;
  double brushWeight(double d) const;
  void startBrush(const QPointF& m, bool smooth);
  void startInflate(const QPointF& m, int sign);
  void smoothStep(const QPointF& m);
  void brushMove(const QPointF& m);
  // the gizmo (ViewportGizmo.cpp)
  Gizmo gz_;
  std::array<Camera, 4> panes_; int act_ = 0, soloPane_ = 0; bool split_ = false; QString layout_ = "4";
  void saveSplit() const;
  void loadSplit();
  struct SnapOrbit { bool on = false; Vec3 pos; Quat quat; } snapOrbit_;
  int refPlane_ = -1, refRev_ = 0; bool refPivEdit_ = false;
  struct RpDrag { int id = -1; Vec3 n, ref, start; std::optional<Vec3> startHit; bool moved = false; M4 m0; Vec3 piv0; } rpd_;
  bool rpDragPress(const QPointF& m);
  void rpDragMove(const QPointF& m);
  QTimer* navAnim_ = nullptr;
  bool gzLocal_ = false;
  struct GzCustom { Quat q; Vec3 pivot; bool hasPivot = false; QByteArray key; } gzCustom_;   // (Custom: the gizmo put by Alt + drag)
  bool gdCustom_ = false;                                // (an Alt drag: the gizmo itself moved / turned, not the model)
  QByteArray gzKey() const;                              // (what the custom pivot belongs to: the selection now)
  Vec3 tSelNormal(int m) const;                          // the selection's average normal (world)
  Quat gzFrame() const;                                  // the gizmo's axes by the orientation chosen
  Quat gzFrameLocal() const;
  bool gizmoPressCustom(const QPointF& m);
  struct GzDrag { std::vector<int> list; std::vector<Vec3> starts; std::vector<int> meshes; std::vector<std::vector<Vec3>> meshStarts; std::vector<int> imeshes; std::vector<Affine> imStarts; std::vector<int> groups; std::vector<Vec3> gpStarts;
                  Affine lastM; int snap = -1; bool moved = false; };
  GzDrag gd_;
  struct GzTargets { std::vector<int> anchors, meshes; Vec3 pivot; std::vector<int> imeshes; int refPlane = -1; int topo = -1; std::vector<int> verts; std::vector<int> groups; };   // (groups: those with a point of their own — it moves with them)
  std::optional<GzTargets> gizmoTargets() const;
  Quat localFrame() const;
  void rotateOwnAxes(const Quat& D);
  bool gizmoPress(const QPointF& m, bool ctrl = false, const QString& forced = {});
  void gizmoMove(const QPointF& m, Qt::KeyboardModifiers mods);
  void gizmoRelease(const QPointF& m);
  QString tool_ = "cursor";
  int drawing_ = -1;                                     // the curve being drawn (S.drawing)
  std::optional<Vec3> ghost_;                            // the ghost line's end (the next point)
  std::optional<Vec3> placePoint(const QPointF& m) const;   // on the head (or a mesh with Collision), else a plane at the last point
  void addDrawPoint(const QPointF& m);
  void afterEdit(bool topology);                         // pending curves rebuilt, the step committed
  std::optional<AutoFixResult> wfix_;
  bool wfixBusy_ = false;
  void showWeldSpots(const LockWeldResult& lw);         // the places, the fixes under review among them
  void afterSceneSwap();                                 // the scene replaced from outside (a fix shown): drawn anew
  enum class Drag { None, Orbit, Pan, Dolly, Anchor, Lasso, PaintSel, AnchorLine, Gizmo, Brush, BrushSmooth, Inflate, BrushAdjust, Mask, GuideStroke, CutStroke, Smooth, Slide, CrownPlace, EdgeBox, RefTop, RefMove, RefPlane, Topo, Uv, LockEx };
  // the Lock Extrude tool (ViewportLockEx.cpp): the flat locks picked, extruded into volumes — editable while the tool and
  // the selection stay
  struct LockExState { bool on = false; std::vector<lockex::Built> built; lockex::Params P; std::vector<int> selKey; } lx_;
  QPointF lxStart_; lockex::Params lxP0_; bool lxRmb_ = false, lxMoved_ = false;
  bool lockExPress(QMouseEvent* e);
  void lockExMove(const QPointF& m);
  void lockExRelease();
  bool lockExBuild();                                    // (the copies made for the selected locks)
  void lockExUpdate();
  // the Topology room (ViewportTopo.cpp)
  TSel tsel_; int topoRev_ = 1;
  // the snap mask's proxy of a high-poly mesh (ViewportMaskProxy.cpp — the file's space): its triangles (faceGroup each),
  // their hierarchy, each polygon of the mesh → its proxy triangle
  struct MaskProxy { std::shared_ptr<PolyMesh> mesh; std::shared_ptr<topo::Bvh> bvh; std::vector<int> f2p, pSrc; };   // (pSrc: the polygon each triangle was made from)
  struct MaskProxyJob { std::shared_ptr<PolyMesh> src; std::vector<std::pair<std::shared_ptr<PolyMesh>, Affine>> others; std::shared_ptr<MaskProxy> out; std::atomic<bool> done{false}, cancel{false}; };
  struct MaskProxySlot { std::weak_ptr<PolyMesh> src; std::shared_ptr<const MaskProxy> px; std::shared_ptr<MaskProxyJob> job; qint64 seen = 0; bool failed = false; };
  std::map<int, MaskProxySlot> mproxy_; mutable std::map<int, std::pair<double, std::vector<char>>> pxMask_;
  static void maskProxyBuild(MaskProxyJob& J);
  bool maskProxyTick();                                  // (→ one still being made)
  std::shared_ptr<const MaskProxy> maskProxy(int id) const;   // (ready and still of its geometry)
  static std::vector<char> pxFromMask(const MaskProxy& X, const std::vector<char>& a);
  static std::vector<char> pxApply(const MaskProxy& X, const std::vector<char>& before, const std::vector<char>& after, std::vector<char> a, const std::vector<char>& locked);
  std::vector<char> faceLocked(int m) const;            // (the hidden polygons — a hidden polygroup's, those hidden with H; empty: none)
  struct PxVis { QByteArray key; std::vector<char> hid; std::shared_ptr<const topo::Bvh> bvh; };   // (the proxy's hidden triangles; the hierarchy of the shown ones)
  mutable std::map<int, PxVis> pxVis_;
  const PxVis& pxVis(int m, const MaskProxy& X) const;
  using Occluders = std::vector<std::pair<int, std::shared_ptr<const topo::Bvh>>>;   // (a mesh: its hierarchy — null: a high-poly one's own, through heavyRay)
  Occluders meshOccluders(int m) const;                  // (the other shown meshes that may hide m)
  bool occludedBy(const Occluders& o, const Vec3& eye, const Vec3& p, double tol) const;   // (one of them between eye and p, more than tol before p)
  const std::vector<char>& pxMaskOf(int m, const MaskProxy& X) const;   // (the mesh's mask on its proxy — made once per mask)
  struct TDrag { std::shared_ptr<const MaskProxy> px; std::vector<char> parr, parr0; std::vector<std::pair<int, std::shared_ptr<const topo::Bvh>>> occ; int type = 0; QString mode; double x0 = 0, y0 = 0; bool moved = false, empty = false, erase = false; int m = -1;
                 std::vector<int> list; std::vector<Affine> starts; Vec3 ref, n; std::optional<Vec3> hit0; Vec3 w;
                 std::vector<int> verts; std::vector<Vec3> base; std::vector<int> pairs; Affine inv; std::vector<char> arr; QPointF last; std::vector<std::array<float, 2>> uv0;
                 int handle = -1; Vec3 handleW;                  // (the element snap: the vertex taken hold of, where it was)
                 struct OnEl { int m = -1, el = -1, f = -1, e = -1; bool dbl = false; }; std::optional<OnEl> onEl; std::vector<std::pair<double, double>> pts; bool clearAll = false; } td_;
  struct TClick { int id = -1; char key = 0; int el = -1; qint64 t = 0; std::vector<int> prev; int lastBefore = -1; } tclick_;
  // the modelling tools' state: what is under the pointer, the PolyPen's chain and anchors, a press / drag, the Loop's
  // preview, the Knife's curves, the edge chain's curve, the Plane Cut's line, the last edit (made again on a setting)
  struct TTState {
    int m = -1; TTPt hov; int hf = -1; bool hovOn = false; QPointF mouse; bool shiftHeld = false;
    std::vector<TTPt> chain; std::vector<Vec3> anchors;
    int drag = 0; QPointF press; bool moved = false, ctrl = false, shift = false; TTPt pressPt; int pressF = -1;
    std::vector<int> verts; std::vector<Vec3> base; Vec3 ref, n; std::optional<Vec3> hit0; Vec3 delta; std::shared_ptr<PolyMesh> before;
    qint64 lastClick = 0; QPointF lastClickPos;
    int le = -1; double lt = 0.5; bool lshift = false, lctrl = false; std::vector<std::vector<Vec3>> loopPrev; bool loopClosed = false;
    int lpVert = -1; std::pair<int, int> lpEdge{-1, -1}; bool lpCtrl = false;   // (Loop, parallel: the side it keeps to — set when Ctrl goes down, kept till it goes down again)
    bool bevPull = false;                                      // (Shift + drag: the polygons only moved out — no new ones)
    bool bevEdges = false;                                     // (Bevel on edges: a chamfer — new loops either side)
    bool symNew = false;                                       // (the edit made polygons — with Symmetry on, their mirror made too)
    std::vector<int> symPairs;                                 // (PolyPen's drag with Symmetry on: each vertex's mirror pair)
    struct KCurve { std::vector<Vec3> a; bool line = false, closed = false; }; std::vector<KCurve> kc; int kcur = -1, kdc = -1, kda = -1;
    std::vector<std::pair<std::vector<KCurve>, int>> kUndo, kRedo;   // (the knife's curves: their own undo / redo while drawn)
    int wsrc = -1; std::vector<int> wsrcs; int wtgt = -1;            // (Weld: a drag from vertex wsrc (with the picked ones wsrcs) onto wtgt)
    struct ECurve { int m = -1; std::vector<int> verts; bool closed = false; std::vector<Vec3> a, a0; std::vector<double> u; std::vector<Vec3> base; } ec; int eca = -1;
    QPointF p0, p1;
    QString lastOp; int lastM = -1; std::shared_ptr<PolyMesh> lastAfter; QJsonObject lastArgs; TSel lastSel;   // (lastSel: what the edit worked on)
    int rev = 0;
    const PolyMesh* fillB = nullptr; std::vector<std::vector<int>> fillAll, fillLoops; int fillOff = 0; std::vector<std::vector<Vec3>> fillPrev;   // (Fill Hole: the holes, those aimed at, the variant, the preview)
    QString fillKey; std::set<int> fillSelAfter;                // (the preview's key; the patch selected after a fill)
    double adj0 = 0, adjDone = 0; QString arm;            // (arm: a menu button's one-shot action in the viewport — optpick, nturn, ncopy)                                // (Fill Hole: Shift / Ctrl + RMB drag — the value at the press, the one last shown)
    std::vector<Vec3> tgt; std::vector<std::vector<int>> nbs; int lead = -1, shapeOp = 0; std::vector<std::array<float, 3>> n0;   // (Edge Shaper / Slide / Normals: targets, neighbours, the lead vertex, the op, the normals at the press)
  };
  void fillPreview(const QPointF& mp, const Vec3* aim = nullptr);   // (aim: the hole nearest that point — tests)
  void fillAdjustShow();                                             // (a changed Edge Flow / Subdivision: the patch made again, else the preview)
  bool shapePress(const QPointF& mp, bool rmb, bool shift, bool ctrl);   // (Edge Shaper, Slide, Normals, Optimize: a drag in empty space; → handled)
  void shapeMove(const QPointF& mp);
  void shapeRelease();
  void fillSnapPts(int m, std::vector<Vec3>& pts) const;             // (Snap on: the new points onto the other meshes' surface)
  std::vector<int> fillBuild(int m, tedit::MeshEd& E, QJsonArray* loops);
public:
  void fillAimAt(const Vec3& w) { fillPreview(QPointF(), &w); }
  QString testBevel() { tt_.before.reset(); return bevelApply(topoMesh(), ttools.bevOffset, false); }   // (tests: Bevel / Extrude with the panel's offset)
  QString testLoop(int e, double t) { tt_.before.reset(); return loopApply(topoMesh(), e, t, false, 1, false); }   // (tests: a Loop cut through edge e)
  QString fillHolesInfo();
private:
  mutable TTState tt_;                                   // (mutable: the pointer's polygon noted while picking)
  qint64 smoothShow_ = 0;                                // (PolyPen's smoothing circle shown a moment after the wheel)
  qint64 fillShow_ = 0;                                // (the fill radius shown a moment after [ / ])
  void ttTouch() { tt_.rev++; topoRev_++; overlayRev_++; requestUpdate(); }
  void topoToolDraw(TopoDraw& D);
  // PatchTool's events and drawing (ViewportPatch.cpp)
  bool patchPress(QMouseEvent* e); bool patchRmb(QMouseEvent* e); bool patchMove(const QPointF& mp); bool patchRelease(const QPointF& mp); bool patchKey(int key); bool patchWheel(int steps);
  void patchDraw(TopoDraw& D); void patchOverlay(std::vector<Overlay>& out) const;
  std::optional<Vec3> patchSnap(const QPointF& mp, int skipCurve) const;   // (where a click puts an anchor: another curve's anchor, the built mesh's rim vertex, a curve, the surface)
  Vec3 patchOnRefs(const Vec3& p) const;                 // (onto the references' surface — the nearest point)
  std::vector<Vec3> patchSample(const std::vector<Vec3>& anchors, const std::vector<Vec3>& prevA = {}, const std::vector<Vec3>& prevL = {}) const;   // (a curve laid on what the view sees under it; the spans whose anchors are as they were (prevA, laid prevL) kept as laid then)
  int patchTarget() const;                               // (the mesh being built: picked, not a reference)
  void patchJoinAsk(int c);                              // (an end of curve c on another's end: one curve of them, or kept apart)
  // the Lobe Brush's events and drawing (ViewportLobes.cpp)
  bool lobePress(QMouseEvent* e); bool lobeRmb(QMouseEvent* e); bool lobeMove(const QPointF& mp); bool lobeRelease(const QPointF& mp); bool lobeKey(int key);
  void lobePaint(const QPointF& mp); void lobeDraw(TopoDraw& D); void lobeGhostDraw(TopoDraw& D); void lobeWrite(int m, const std::vector<int>& lab, bool commitIt);
  TTPt ttPickPt(const QPointF& mp, int m, bool anchors, bool back = false) const;   // (back: its polygon seen from behind taken too — Ctrl+click deletes it)
  std::vector<Vec3> ttWorld(int m) const;
  Vec3 ttFreePoint(const QPointF& mp, int m) const;
  mutable bool fpRefused_ = false; SideGuard sideGuard(int m) const;               // (the built mesh m: the side of the references it lies on — its back none to snap onto)
  bool ttSeen(const Vec3& w, int m) const;   // (a world point in sight: nothing of mesh m nor of a shown reference in front of it)
  Vec3 ttSnapW(const Vec3& w, int m) const; Vec3 ttSnapMoved(const Vec3& w, int m) const;   // (ttSnapMoved: for a vertex already near the surface — the view's point unless the surface right by it is much nearer)             // snap on: a world point onto the other meshes where it is seen (along the view), else their nearest
  bool ttNearPx(const Vec3& a, const Vec3& b, double px) const;   // (on screen within px)
  std::vector<TTPt> penFillPoints(const QPointF& mp) const;  // (what Shift+RMB fills: a pick with the user's anchors, else the ghost's polygon, else the rim's)
  std::vector<TTPt> penFillPick(const QPointF& mp) const;
  std::vector<TTPt> penFillHole(const QPointF& mp) const;   // (nothing to pick round the pointer: the hole of the mesh it is in — up to 24 corners)
  struct PenAsk { int m = -1; std::vector<std::pair<Vec3, Vec3>> pairs; QWidget* w = nullptr; } penAsk_;   // (PolyPen: a weld in doubt — asked; its vertices by place, world)
  void penWeldAsk(const QPointF& at); void penAskClose(); void penAskWeld();
  QString penFillHoleMake(const QPointF& mp);           // (that hole filled: quads round its middle — the anchor there, else a point made)   // Shift+RMB: the anchors / rim vertices round the pointer that hold it (3 or 4) — none: empty
  Vec3 ttPtW(const TTPt& p) const;                     // (a pen point in the world)
  QString ttDone(int m, const tedit::MeshEd& E, const PolyMesh& src, const QString& msg, char selKey = 0, const std::vector<int>& sel = {}, const std::vector<std::pair<int, int>>& selE = {}, bool commitIt = true);
  QString penClick(const TTPt& p, bool dbl);
  QString penNewMesh();                                 // (no mesh picked: the closed polygon → a new RetopoMesh, selected)
  QString penFill(const QPointF& mp);
  QString penDelete(const TTPt& p, int f);
  QString loopApply(int m, int e, double t, bool selectNew, int cuts, bool parallel);
  QString planeApply(int m, const Vec3& o, const Vec3& n, const Vec3& along);
  QString bevelApply(int m, double offset, bool inset);
  QString bevelEdgesApply(int m, double offset);          // Bevel on edges: each picked edge split into two, a loop of polygons between (a chamfer)
  QString bridgeUneven(int m, tedit::MeshEd& E, const PolyMesh& B, std::vector<int> c1, std::vector<int> c2, bool closed);   // (Bridge: chains of unlike counts — quads where they keep step, triangles where one runs ahead)
  struct THover { int m = -1, f = -1, e = -1, v = -1; std::vector<int> region, rim, pts; int ver = -1; bool inv = false; char key = 0; } thov_;   // (region / rim / pts: Fill / Outline / Loop / Ring's preview)   // (region / rim: Fill / Outline's preview)
  struct TgHover { int id = -1, gi = -1; } tgHover_;
  struct TgIso { int id = -1; QJsonValue hid; std::optional<Scene::Isolation> iso; }; std::optional<TgIso> tgIso_;
  struct TcIso { int id = -1; QJsonValue hid; std::optional<Scene::Isolation> iso; }; std::optional<TcIso> tcIso_;   // (Ctrl+Shift+H: what to bring back)
  struct TopoPick { int m = -1, f = -1, e = -1, v = -1; Vec3 point; };
  struct PickCache { std::weak_ptr<PolyMesh> poly, base; QByteArray hid; std::vector<uint32_t> tris; std::vector<int> tri2f; Box3 box;
    std::shared_ptr<topo::Bvh> bvh; double bvhSum = 0; };   // (bvh: what hides what — made when a selection asks, again when the vertices moved)
  mutable std::map<int, PickCache> pickCache_;
  const PickCache* pickCacheOf(int m) const;
  const topo::Bvh* pickBvhOf(int m) const;                // the shown polygons of the mesh, for "is it hidden by the mesh itself"
  std::optional<TopoPick> tPick(const QPointF& mp, const std::vector<int>& list) const;
  int loopEdgeAt(int m, int f, const QPointF& mp) const;  // the edge a polygon loop runs across (from where in the polygon the pointer is)
  std::vector<int> tLoopOf(int m, const TopoPick& r, const QPointF& mp, bool ring) const;   // Loop / Ring: the elements (the mode's)
  std::vector<int> tSymList(int m, const std::vector<int>& list, char key) const;          // the elements' mirrors across X = 0
  struct SymC { const PolyMesh* B = nullptr; std::vector<int> pairs; std::unordered_multimap<quint64, int> faces; }; mutable SymC symC_;   // (faces: by their sorted corners' hash)
  std::shared_ptr<const topo::Bvh> bvhOf(int m, bool masked) const;
  struct BvhC { std::weak_ptr<PolyMesh> poly; QString tag; std::shared_ptr<topo::Bvh> b; };
  mutable std::map<int, BvhC> bvhCache_;
  mutable QByteArray ghostKey_; mutable std::vector<Ghost> ghosts_;   // (Topology Ghost: what it was made from, the ghosts)
  QByteArray ghostMidKey_; std::vector<std::vector<Vec3>> ghostMid_; qint64 revealShown_ = 0;   // (the Reveal Brush: the ghost polygons' middles, its stroke last shown)
  mutable std::vector<std::pair<QByteArray, std::vector<Ghost>>> ghostRecent_; mutable QByteArray ghostDrawKey_; mutable TopoDraw ghostDrawn_;   // (the last few made — back to one at once; its lines made once)
  std::map<int, std::pair<QByteArray, TopoDraw::Tris>> maskDrawCache_; int maskPaintRev_ = 0; qint64 maskPaintShown_ = 0;   // (the snap masks' green, made once per mask; a stroke's steps)
  mutable std::shared_ptr<struct GhostJob> ghostJob_; mutable QTimer* ghostPoll_ = nullptr; mutable std::shared_ptr<ui::ThinkScope> ghostThink_;   // (the ghost being made, in a thread of its own)
  void ghostDraw(TopoDraw& D) const;
  std::set<std::pair<int, int>> revealFaces_;                // (the Reveal Brush's stroke: the ghost polygons gathered — ghost, polygon)
  void revealGather(const QPointF& mp);
  QString revealApply();
  std::map<int, int> geomStamp_;                         // (a mesh's points changed in place — its cached BVH made anew)
  std::vector<Vec3> tWorld(int m) const;                 // its vertices as shown (world)
  bool topoPress(QMouseEvent* e);
  bool topoMove(const QPointF& m, Qt::KeyboardModifiers mods);
  bool topoRelease(const QPointF& m);
  bool topoDblClick(QMouseEvent* e);
  void topoHover(const QPointF& m, Qt::KeyboardModifiers mods);
  // double click: alone — the piece (polygons) / the loop (edges, vertices); after a click on another element a — the loop
  // through both, every so many as far apart as they are (up to the selection or a fork)
  void topoDouble(int m, const TopoPick& r, char key, int a = -1, const std::vector<int>& prev = {});
  std::vector<int> tPatternWalk(int m, char key, int a, int b, const std::set<int>& stop) const;
  std::vector<int> tBoxPick(int m, double x0, double y0, double x1, double y1, char key) const;
  // the elements of the active mesh whose point on the screen is inside (the box first; facing the camera unless through)
  std::vector<int> tAreaPick(int m, double x0, double y0, double x1, double y1, char key, const std::function<bool(double, double)>& in) const;
  void tPaintAt(const QPointF& mp, const QString& mode);  // (the Select Cursor's drag: what is under the brush picked / taken off)
  void topoAfterMove(int m, std::vector<Vec3>& P, const std::vector<int>& verts, const std::vector<Vec3>& base, const std::vector<int>* pairs);
  void topoLive(int m, const std::vector<Vec3>& pos, const std::vector<std::array<float, 2>>* uv = nullptr);   // (a drag: the positions shown, no undo step yet)
  void topoWrite(int m, std::shared_ptr<PolyMesh> B);   // (a new source: the geometry changed)
  void liveApply(int m, const std::vector<Vec3>* pos, const std::vector<std::array<float, 2>>* uv);
  void smPaint(const QPointF& m);
  void smLasso();
  void topoGizmoApply(const Affine& M);
  struct GzComp { std::vector<int> idx; Vec3 c; Quat q; };   // (a component moved on its own: its vertices (indices into verts), centre, axes)
  struct TopoGz { int m = -1; std::vector<int> verts; std::vector<Vec3> w0, base; std::vector<int> pairs; bool on = false; std::vector<std::array<float, 2>> uv0;
    std::vector<GzComp> comps; Quat q0; Vec3 p0; std::vector<std::vector<int>> nbs;   // (Components; the gizmo at the press; Slide: each vertex's neighbours off the selection)
    std::shared_ptr<PolyMesh> preExtrude; TSel preSel; } tgz_;
  bool tExtrudeSel(int m);                               // Ctrl + a gizmo handle: the selected polygons / edges extruded in place (selected, ready to move)
  bool gizmoPressFree(const QPointF& m);                 // a drag in free space: as the gizmo's centre / view ring
  std::vector<int> uvParked_;
  struct HeavyJob { std::shared_ptr<PolyMesh> src; std::shared_ptr<PolyMesh> lod; std::shared_ptr<topo::Bvh> bvh; std::vector<int> t2f; std::atomic<bool> done{false}, cancel{false}, bvhDone{false}; bool wantLod = true; };
  struct HeavySlot { std::weak_ptr<PolyMesh> src; std::shared_ptr<PolyMesh> lod; std::shared_ptr<topo::Bvh> bvh; std::vector<int> t2f; std::shared_ptr<HeavyJob> job; qint64 seen = 0; bool wantLod = false;
                     mutable QByteArray hidSig = "?"; mutable std::vector<char> hidTri; mutable std::vector<int> pcOf, pc2f; };   // (hidTri: its hidden triangles; pcOf: each triangle's number in the pick cache)
  std::map<int, HeavySlot> heavy_; QTimer* heavyPoll_ = nullptr; qint64 wheelAt_ = 0, wheelSlowUntil_ = 0;
  static void heavyBuild(HeavyJob& J);
  void heavyTick();
  void heavyStale(int m);                                // (its geometry changed in place: made again once it rests)
  std::optional<std::optional<RayHit>> heavyRay(int id, const Ray& r, bool exactTri = true) const;   // (exactTri: the hit's triangle as the pick cache numbers them)                            // (the UV Editor windows while the Hair room is shown)
  int heavyFace(int id, int bvhTri) const;               // (the polygon of a triangle a heavyRay hit, -1)
  bool pxSeen(int m, const MaskProxy& X, const PxVis& V, int t, const Vec3& eyeW, const Affine& M, bool ortho, const Vec3& dirW) const;   // (a proxy triangle not hidden by the real surface in front of it)
  TopoDraw topoDraw_; QByteArray topoSig_;
  // the UV (ViewportUv.cpp)
  int uvRev_ = 1, uvStamp_ = 0;
  UvDraw uvDraw_, uvHovDraw_; QByteArray uvSig_, uvHovSig_; int uvHovRev_ = 0;
  struct UvCache { std::weak_ptr<PolyMesh> base; int stamp = -1; std::shared_ptr<const topo::TTopo> T; uv::Points P; uv::Shells S; std::vector<char> cont, seam; std::vector<int> mc; bool mcMade = false; };   // (mc: the mirror corners — made when the symmetry tint asks)
  mutable UvCache uvc_;
  const UvCache* uvCache() const;                        // (the active mesh's points and shells, kept while its UV stays)
  std::set<int> uvPts_; int uvPtsVer_ = -1, uvLastPt_ = -1;   // (UV points picked in the editor; −1: from the vertices picked; uvLastPt_: the last clicked — Ctrl+Shift's path from it)
  std::set<int> uvPoints();                              // the picked UV points (Vertex mode)
  struct UvHover { int f = -1, e = -1, ek = -1, p = -1; } uvHov_;
  struct UvDragS { int type = 0; QPointF p0, last; bool moved = false; QString mode; int m = -1; char op = 'm'; int axis = 0; std::vector<int> corners; std::vector<std::array<float, 2>> uv0; std::vector<std::array<double, 2>> piv;   // (axis: the gizmo's handle — 0 free, 1 U, 2 V; piv: per corner its pivot — each shell its own)
                   double cu = 0, cv = 0; bool dbl = false; } uvd_;
  // the brushes (ViewportUvBrush.cpp): 'r' Relax, 'p' Pin — the radius in screen pixels, the Relax's step
  QPointF uvBrushAt_{-1, -1};
  struct UvBrushS { std::vector<std::vector<int>> pf; std::vector<char> border, fixed, vis, showF; std::vector<std::array<float, 2>> U; std::shared_ptr<PolyMesh> W; uv::PackOpts po; QPointF last{-1e9, -1e9}; int touched = 0; } uvBr_;   // (W: a copy the Relax works on; vis: a point of a shown polygon)
  void uvBrushOverlay(std::vector<Overlay>& out) const;
  bool uvBrushPress(const QPointF& mp, bool ctrl);
  void uvBrushStroke(const QPointF& mp, Qt::KeyboardModifiers mods);
  void uvBrushRelease(const UvDragS& d);
  struct UvClick { int el = -1; char key = 0; qint64 t = 0; } uvClick_;
  struct UvTf { bool on = false; char kind = 0; int m = -1; std::vector<int> ks; std::vector<std::array<float, 2>> uv0; const PolyMesh* base = nullptr;
    std::array<double, 4> box{0, 0, 1, 1}; int nx = 3, ny = 3; std::vector<std::array<double, 2>> ctl, ctl0;   // (the warp's anchors: now, at the drag's start)
    double cu = 0, cv = 0, hw = 1, hh = 1, ang = 0, cu0 = 0, cv0 = 0, hw0 = 1, hh0 = 1, ang0 = 0;     // (the frame: its middle, half sizes, turn)
    std::vector<int> pof; std::vector<std::array<double, 2>> prest, pcur, pcur0; std::vector<std::array<int, 3>> ptri; std::vector<int> pins; std::vector<std::array<double, 2>> pin0;   // (Puppet: each corner's point, the points at rest / now / at the drag's start, the triangles, the pins)
    std::vector<int> chPins, chOrd, chPos; size_t chN = 0; int chBand = 0; std::vector<double> chL;   // (Puppet: the factor of its system — for these pins)
    int drag = -1; QPointF p0; } uvTf_;
  int uvTfRev_ = 0;
  bool uvTfPress(const QPointF& m);
  void uvTfResync();                                     // (the UV changed under the Frame / Warp / Puppet — an undo: made again from it, the Puppet's pins kept)
  void uvTfMove(const QPointF& m, Qt::KeyboardModifiers mods);
  void uvTfRelease();
  void uvTfDraw(UvDraw& D);
  std::vector<std::array<float, 2>> uvTfResult() const;
  bool uvSplitByMode_ = false; bool uvWasSplit_ = false; QString uvWasLayout_; int uvWasPane_ = -1; QString uvWasView_;
  QPointF uvToPx(double u, double v) const;
  std::array<double, 2> pxToUv(const QPointF& p) const;
  void uvPress(QMouseEvent* e);
  void uvMove(QMouseEvent* e);
  void uvRelease(QMouseEvent* e);
  void uvWheel(QWheelEvent* e);
  void uvFrame();
  struct UvPick { int f = -1, e = -1, ek = -1, p = -1; };   // (ek: the corner the edge starts from — its side)
  UvPick uvPickAt(const QPointF& m) const;
  std::vector<int> uvBox(double x0, double y0, double x1, double y1, char key) const;
  std::vector<int> uvMoving() const;                     // the corners the selection moves
  void uvLive(int m, const std::vector<std::array<float, 2>>& uv);
  QString uvEdit(const std::function<QString(PolyMesh&, const topo::TTopo&)>& fn);   // (on a copy of the active mesh: written, one undo step)
  std::vector<std::array<float, 2>> uvKeep(int m, const std::vector<Vec3>& P0, const std::vector<std::array<float, 2>>& uv0, const std::vector<Vec3>& P) const;
  struct RoomState { bool valid = false; std::map<int, Item> items; std::map<int, Anchor> anchors; std::vector<int> roots; std::vector<Snapshot> hist; int idx = -1;
                     Selection sel; QString tool = "cursor"; QJsonValue counters; };
  std::map<QString, RoomState> rooms_;
  std::set<int> sharedIds(const std::map<int, Item>& items) const;
  void loadTopoPrefs();
  std::vector<double> smoothStart_;

  Projector projector() const;
  Ray ray(const QPointF& m) const;
  std::optional<RayHit> pickSurface(const QPointF& m) const;
  int pickAnchor(const QPointF& m) const;
  struct PickedCurve { int id; int seg; int sample; double t; double d; };
  std::optional<PickedCurve> pickCurve(const QPointF& m, bool hairOnly, bool noService) const;
  bool curveVisible(const Item& c) const;
  void onSelChange();
  void paintSel(const QPointF& m);
  void markCurves(const std::set<int>& ids);
  void startAnchorDrag(const std::vector<int>& list, const QPointF& m, const Vec3& ref);
  bool slideToCurve(const QPointF& m);
  void insertAt(const PickedCurve& ch, const QPointF& m);
  void anchorLine(const QPointF& a, const QPointF& b);
  void checkWeld(int a, const QPointF& m);
  int findWeldCandidate(int a) const;

  void reloadHair();                                     // the geometry built anew (the curves came / went)
  void commit(bool topology);                            // the network kept up, a snapshot for undo
  Snapshot snapshot() const;
  void restore(const Snapshot& s);

  Scene* scene_;
  Camera cam_;
  HairState hair_;
  Selection sel_;
  DisplayState ds_;
  std::vector<ShownAnchor> anchors_;
  std::set<int> hoverCurves_;
  bool solo_ = false;
  bool shellPress(QMouseEvent* e);                       // the mask brushes, the guide strokes (→ handled)
  void shellMove(const QPointF& m);
  void shellRelease(const QPointF& m);
  bool paintCocoonMask(const QPointF& m, const QString& op, double amt, const QString& type, double strength, const QString& field = "cmask");
  void createCocoonGuide(const QPointF& A, const QPointF& B);
  void placeDisc(const QPointF& m);
  int makeDisc(const Vec3& C, const Vec3& N, double rOut, double rIn, int parent);
  int discOnCocoon(int k, double f);
  int sceneDiscAt(std::optional<double> y);
  int guidesGroupForNew();
  struct MaskDrag { QString op; QPointF last; bool moved = false; std::set<int> touched; bool rib = false; } md_;
  struct RefDrag { Vec3 n, p0, start; std::optional<Vec3> hit0; double h0 = 0; } rd_;
  bool refPress(const QPointF& m);
  void refMove(const QPointF& m);
  std::vector<walltools::Kink> kinks_;
  std::map<int, QByteArray> meshSig_;
  bool meshWeightsShown(int mesh) const;
  std::vector<int> modMeshes() const;
  int zoneG_ = -1; qint64 zoneUntil_ = 0;
  QString kinkKey_;
  void syncKinks();
  std::map<int, RibDraw> ribs_;
  std::map<int, rib::Shape> ribShapes_;
  std::map<int, QByteArray> ribSig_;
  int ribsRev_ = 0, ribsSeen_ = -1;
  void syncRibs();
  void ribFollow();                                      // (the root of a lock started from anchors follows them)
  bool ribPress(QMouseEvent* e);
  bool paintRibNow(const QPointF& m, const QString& op, double amt);
  Vec3 camDir() const;
  std::vector<Vec3> collideBetween(const Vec3& a, const Vec3& b, int depth = 0) const;
  struct EdgePick { int mesh, e; };
  std::optional<EdgePick> pickEdge(const QPointF& m, bool hover) const;
  std::optional<EdgePick> hoverEdge_;
  struct { int mesh = -1, e = -1; qint64 t = 0; } edgeClick_;
  void edgeClick(const QPointF& m, bool ctrl, bool shift);
  void edgeBoxDeselect(double x0, double y0, double x1, double y1);
  void setEdgeSel(int mesh, const std::vector<int>& list, const QString& mode);
  std::map<int, crown::CrownMesh> crowns_;
  std::map<int, QByteArray> crownSig_;
  int crownsRev_ = 0, crownsSeen_ = -1;
  std::map<int, std::map<int, Vec3>> crownSymLast_;
  std::map<int, Vec3> attLast_;
  struct SlideDrag { int a = -1; std::vector<Vec3> poly; bool moved = false; } sl_;
  void syncCrowns();
  void crownFrame();                                     // (beforeFrame: symmetric crowns, attached anchors)
  bool crownPress(QMouseEvent* e, int a);                // Ctrl + drag along a connector, the crown tool's click → handled
  void slideMove(const QPointF& m);
  void checkAttach(int a, const QPointF& m);             // an anchor end dropped near a connector: attach it?
  QString autoAttachEnds(int curve);                     // a drawn curve's ends placed on a crown connector: attached to it (on its anchor — that anchor shared)
  crown::HeadHit headHitFn() const;
  std::map<int, TubeDraw> tubes_;
  std::map<int, QByteArray> tubeSig_;
  int tubesRev_ = 0, tubesSeen_ = -1;
  void syncTubes();                                      // (in beforeFrame: the modifiers whose curve / settings changed)
  bool modPress(QMouseEvent* e);                         // the weight brush (→ handled)
  bool paintWeights(const QPointF& m, const QString& op, double amt);
  bool guideAdd_ = false;
  QWidget* pieHost_ = nullptr;
  std::optional<SurfCutState> scut_;
  int scutDrag_ = -1;
  QString applyEdgeCut();                                // (the cut begun on the wall: from its top edge to its bottom)
  QString applyEdgeToCurveCut();                         // (begun on a curve, its last anchor on the top / bottom edge: from the edge to that curve)
  QString applyFoldCut(const void* fold);                // (a "V" placed: a "Y" — two curves sharing the stem, the triangle between the legs taken out)
  std::vector<Overlay> scutViz_;
  std::vector<std::vector<Vec3>> fillViz_;               // (the fill outline shown)
  std::vector<std::vector<Vec3>> pieVizA_, pieVizB_;
  std::map<int, Vec3> linkLast_;                         // (⇶ linked anchors: their positions after the last sync)
  void drawSurfCut();
  bool surfCutDown(QMouseEvent* e);
  void surfCutMove(const QPointF& m);
  std::optional<SurfCutEnd> edgePick(const QPointF& m, double maxPx) const;
  std::optional<RayHit> pickWall(const QPointF& m, int sf) const;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  bool cutPress(QMouseEvent* e);
  void cutMove(const QPointF& m);
  void cutRelease(QMouseEvent* e);
  struct SplitPlan;
  std::optional<SplitPlan> splitPlan(const PickedCurve& ch, const QPointF& m, QString* err) const;
  void splitEnds(const PickedCurve& ch, const QPointF& m);
  std::vector<std::pair<double, double>> splitViz_;     // (the preview: pairs of screen points)
  bool soloOff();                                        // (Ctrl+H … while Solo is on: Solo off, nothing else)
  bool soloFollow();                                     // → the isolation changed
  int staticRev_ = 1, overlayRev_ = 1, weldCand_ = -1;
  bool allWalls_ = true, ringsDirty_ = true;
  std::set<int> dirtyWalls_, pendingCurves_;
  std::map<std::string, std::string> netSig_;            // the network names' signatures per group (NET_SIG)

  Drag drag_ = Drag::None;
  QPointF last_, start_;
  AnchorDrag ad_;
  std::vector<std::pair<double, double>> lasso_;
  bool lassoAdd_ = false;
  struct { QString mode; std::optional<QPointF> last; bool touched = false; } ps_;
  std::vector<Snapshot> history_;
  int histIdx_ = -1, changes_ = 0;
  bool bySlider_ = false;                                // (the last move back in the history made by the History slider, not Ctrl+Z)
  QString stepLabel_;                                    // (the name for the next step)
  QString guessStepLabel() const;
};

}  // namespace hm
