#include "view/Viewport.h"
#include <QDragEnterEvent>
#include <QMimeData>
#include <deque>
#include "scene/Tubes.h"
#include <QTimer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include "scene/AnchorOps.h"
#include <QSettings>
#include "scene/Network.h"
#include "scene/Convert.h"
#include "scene/Edit.h"
#include "ui/Keymap.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Renderer.h"
#include <QJsonArray>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QVulkanFunctions>
#include <QWheelEvent>
#include <unordered_set>

namespace hm {

Viewport::Viewport(Scene* scene) : scene_(scene) {
  solo_ = QSettings().value("solo").toBool();
  uvKeys = QSettings().value("uvKeys").toBool();
  soloTopo = QSettings().value("soloTopo", "mesh").toString();
  loadTopoPrefs(); loadModelPrefs(); loadGhostPrefs(); loadInspect(); loadHairInspect();
  loadSplit();
  setPreferredColorFormats({VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB});
  // wide lines (curves 2 px) and large points (anchors) when the GPU has them
  setEnabledFeaturesModifier([this](VkPhysicalDeviceFeatures& f) {
    VkPhysicalDeviceFeatures have{};
    vulkanInstance()->functions()->vkGetPhysicalDeviceFeatures(physicalDevice(), &have);
    f.wideLines = have.wideLines; f.largePoints = have.largePoints; f.fillModeNonSolid = have.fillModeNonSolid;
  });
}

QVulkanWindowRenderer* Viewport::createRenderer() {
  // MSAA ×4 (the HTML version had antialias: true) when supported
  if (supportedSampleCounts().contains(4)) setSampleCount(4);
  return new Renderer(this);
}

// ---------- scene / selection state ----------

void Viewport::sceneChanged(bool reframe) {
  if (reframe) {
    loadRooms();                                         // (Hair shown; the other rooms wait as their records)
    meshHierarchy(*scene_);
    hair_.load(*scene_);
    sel_ = {}; pendingCurves_.clear(); ad_ = {}; spots_.clear(); weldHot_ = -1;
    patchLoad();                                         // (PatchTool's curves as the project kept them)
    if (scene_->iso && scene_->iso->solo) scene_->iso.reset();   // (Solo starts over in the new project)
    // as a project opens: the network cut / merged / joined and named (the signatures of the names start anew)
    Network net(*scene_, hair_.curves, sel_.items, sel_.anchors);
    auto u = net.loadUpkeep(netSig_);
    if (u.changed()) hair_.load(*scene_);
    history_ = {snapshot()}; histIdx_ = 0; history_[0].label = tr("Project opened"); history_[0].weak = false; emit historyChanged();
    // every window frames the new model (each keeps its view); the project's camera — the active one
    for (int i = 0; i < 4; i++) if (i != act_) { Camera& c = panes_[size_t(i)]; c.fitToBox(scene_->bounds()); }
    cam_.fitToBox(scene_->bounds());
    const CameraState& c = scene_->camera;
    if (c.valid) {                                       // the project's own camera
      cam_.view = c.view; cam_.pos = c.pos; cam_.quat = c.quat; cam_.zoom = c.zoom; cam_.pivot = c.pivot;
      cam_.up = cam_.view == "top" ? cam_.quat.rotate({0, 1, 0}) : Vec3(0, 1, 0);
    }
    emit viewChanged(cam_.view);
    if (u.changed() || u.renamed) emit sceneEdited();
  }
  staticRev_++; allWalls_ = true; ringsDirty_ = true;
  tubesRev_++; ribsRev_++; crownsRev_++;                    // (the Display settings: their opacities, X-ray)
  { ProfScope p3("commit.onSelChange"); onSelChange(); }
}

void Viewport::fitToBox(const Box3& box) {
  for (int i = 0; i < 4; i++) if (i != act_) panes_[size_t(i)].fitToBox(box);
  cam_.fitToBox(box); staticRev_++; ringsDirty_ = true;
  emit viewChanged(cam_.view); requestUpdate();
}

void Viewport::onSelChange() {
  if (lx_.on && drag_ != Drag::LockEx && sel_.items != lx_.selKey) lockExReset();   // (another object picked: the extrusion stays as it is)
  if (!sel_.empty() && refPlane_ >= 0) { refPlane_ = -1; refPivEdit_ = false; refRev_++; }   // (selecting anything in the scene lets the plane go)
  if (soloFollow()) { staticRev_++; allWalls_ = true; }   // (Solo: the shown group follows the selection)
  { ProfScope a("sel.gizmo"); updateGizmo(); }
  { ProfScope b("sel.ds"); ds_ = displayState(*scene_, sel_); }
  { ProfScope c("sel.anchors"); anchors_ = anchorList(*scene_, ds_, sel_, weldCand_); }
  overlayRev_++; ringsDirty_ = true;                     // (the curves shown / their colours; a selected cocoon's shell)
  { ProfScope d("sel.signal"); emit selectionChanged(sel_.items); }
  requestUpdate();
}

void Viewport::setSelectedItems(const std::vector<int>& ids) {
  if (ids == sel_.items && sel_.anchors.empty()) return;
  sel_.items = ids; sel_.anchors.clear();
  { ProfScope p3("commit.onSelChange"); onSelChange(); }
}

// selectItem: (not added) the selection replaced; added — on / off (an item selected again goes last: the active one)
void Viewport::selectItem(int id, bool add) {
  // ⧉ Lock select: a curve / wall / profile modifier picks every curve of its lock (Shift — the whole lock on / off)
  const Item* it0 = scene_->item(id);
  if (lockSel && it0 && ((it0->type == ItemType::Curve && it0->ring < 0) || it0->type == ItemType::Surface || it0->type == ItemType::Tube)) {
    std::vector<int> cs; for (int c : aops::lockCurvesFrom(*scene_, {id}, {})) if (!scene_->item(c)->junc) cs.push_back(c);
    if (!cs.empty()) {
      sel_.anchors.clear(); if (!add) sel_.items.clear();
      bool all = true; for (int c : cs) if (!sel_.hasItem(c)) all = false;
      if (add && all) { for (int c : cs) sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), c), sel_.items.end()); }
      else for (int c : cs) { sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), c), sel_.items.end()); sel_.items.push_back(c); }
      onSelChange(); return;
    }
  }
  sel_.anchors.clear();
  if (!add) sel_.items.clear();
  auto it = std::find(sel_.items.begin(), sel_.items.end(), id);
  if (add && it != sel_.items.end()) sel_.items.erase(it);
  else { if (it != sel_.items.end()) sel_.items.erase(it); sel_.items.push_back(id); }
  { ProfScope p3("commit.onSelChange"); onSelChange(); }
}

bool Viewport::curveVisible(const Item& c) const {
  return curveShown(*scene_, ds_, sel_, c) || c.id == ad_.snap || (showAllCurves && c.ring < 0 && scene_->isVisible(c));
}

void Viewport::liveAnchors(const std::vector<int>& anchors) { markCurves(curvesTouching(*scene_, anchors)); }
void Viewport::liveWalls(const std::set<int>& walls) {
  HairChanges ch = hair_.rebuildWalls(*scene_, walls);
  dirtyWalls_.insert(ch.walls.begin(), ch.walls.end()); if (!ch.cocoons.empty()) ringsDirty_ = true;
  overlayRev_++; requestUpdate();
}
void Viewport::liveCocoons(const std::set<int>& ids) {
  if (CocoonSystem* cs = hair_.cocoonSystem()) cs->rebuild(ids);
  ringsDirty_ = true; staticRev_++; requestUpdate();
}
void Viewport::commitLive(bool topology) { beforeFrame(); runEdit([](Scene&, Selection&, const CurveCache&) { return QString(); }, topology); }

void Viewport::setHoverCurves(const std::set<int>& ids) { if (ids == hoverCurves_) return; hoverCurves_ = ids; overlayRev_++; requestUpdate(); }

// a selected collision capsule's rings and lengthwise lines (in the world — drawn with the camera, orange): shown only
// while it is being placed — selected
std::vector<std::vector<Vec3>> Viewport::capsuleRings() const {
  std::vector<std::vector<Vec3>> out; if (room() != "hair") return out;
  for (int sid : sel_.items) if (const Item* ci = scene_->item(sid); ci && ci->raw.contains("collCap")) {
    const auto CP = capsuleWorld(sid); if (CP.size() < 3) continue; const int S = 32, R = int((CP.size() - 2) / S);
    for (int k = 0; k < R; k += 2) { std::vector<Vec3> L; for (int s = 0; s <= S; s++) L.push_back(CP[size_t(1 + k * S + s % S)]); out.push_back(std::move(L)); }
    for (int s = 0; s < S; s += 4) { std::vector<Vec3> L{CP[0]}; for (int k = 0; k < R; k++) L.push_back(CP[size_t(1 + k * S + s)]); L.push_back(CP.back()); out.push_back(std::move(L)); }
  }
  return out;
}

std::vector<Viewport::Overlay> Viewport::overlays() const {
  std::vector<Overlay> out;
  if (!hoverCurves_.empty()) {
    Projector P = projector();
    for (int id : hoverCurves_) { auto it = hair_.curves.find(id); if (it == hair_.curves.end()) continue;
      std::vector<std::pair<double, double>> pts; for (const Vec3& p : it->second.p) { ScreenPt s = P.toScreen(p); if (s.z < 1) pts.push_back({s.x, s.y}); }
      if (pts.size() >= 2) out.push_back({pts, QColor(0xff, 0xe6, 0xc8), false}); }
  }
  if (!lasso_.empty()) out.push_back({lasso_, QColor(0x99, 0xcc, 0xff), true});
  esnapOverlay(out); topoToolOverlay(out);                                      // (the modelling tools' circles, lines, marks)
  uvPinOverlay(out);                                         // (the UV Puppet's pins)
  uvGizmoOverlay(out);                                       // (the UV Editor's Move / Rotate / Scale gizmo)
  uvBrushOverlay(out);                                       // (the UV brush's circle)
  if (drag_ == Drag::AnchorLine) out.push_back({{{start_.x(), start_.y()}, {last_.x(), last_.y()}}, QColor(0xff, 0xcf, 0x4d), false});
  // the brush's circle and its hardness (dashed — here thinner)
  if (drag_ == Drag::GuideStroke) out.push_back({{{start_.x(), start_.y()}, {mouse_.x(), mouse_.y()}}, QColor(0x5f, 0xe3, 0x9a), false});
  if (drag_ == Drag::CutStroke) out.push_back({{{start_.x(), start_.y()}, {mouse_.x(), mouse_.y()}}, QColor(0xff, 0x6b, 0x6b), false});
  for (size_t i = 0; i + 1 < splitViz_.size(); i += 2) out.push_back({{splitViz_[i], splitViz_[i + 1]}, QColor(0xff, 0x8a, 0x3d), false});
  if (scut_) out.insert(out.end(), scutViz_.begin(), scutViz_.end());
  for (auto [L0, col] : {std::pair{&pieVizA_, QColor(0x5f, 0xe0, 0xff)}, std::pair{&pieVizB_, QColor(0xff, 0xb0, 0x40)}}) if (!L0->empty()) { Projector P = projector();
    for (auto& L : *L0) { std::vector<std::pair<double, double>> o; for (auto& p : L) { ScreenPt s = P.toScreen(p); if (s.z < 1) o.push_back({s.x, s.y}); } if (o.size() >= 2) out.push_back({o, col, false}); } }
  if (!fillViz_.empty()) { Projector P = projector();
    for (auto& L : fillViz_) { std::vector<std::pair<double, double>> o; for (auto& p : L) { ScreenPt s = P.toScreen(p); if (s.z < 1) o.push_back({s.x, s.y}); } if (o.size() >= 2) out.push_back({o, QColor(0x5f, 0xe0, 0xff), false}); } }
  if (mergePick && (inViewport_ || drag_ == Drag::BrushAdjust)) {   // (◎ Pick: the circle whose diameter becomes the distance)
    QPointF c = drag_ == Drag::BrushAdjust ? bd_.start : mouse_; std::vector<std::pair<double, double>> P;
    for (int i = 0; i < 64; i++) { double t = 2 * M_PI * i / 64; P.push_back({c.x() + pickR * std::cos(t), c.y() + pickR * std::sin(t)}); }
    out.push_back({P, QColor(0x5f, 0xe0, 0xff), true});
  }
  if (ribDrawOn() && (inViewport_ || drag_ == Drag::BrushAdjust)) {   // (a flat lock: its half width at the cursor)
    QPointF c = drag_ == Drag::BrushAdjust ? bd_.start : mouse_; std::vector<std::pair<double, double>> P;
    for (int i = 0; i < 64; i++) { double t = 2 * M_PI * i / 64; P.push_back({c.x() + ribR * std::cos(t), c.y() + ribR * std::sin(t)}); }
    out.push_back({P, QColor(0xc0, 0x80, 0xe0), true});
  }
  if (tool_ == "cut" && cutMode == "ends" && (inViewport_ || drag_ == Drag::BrushAdjust)) {   // (Split ends: the spread circle)
    QPointF c = drag_ == Drag::BrushAdjust ? bd_.start : mouse_; std::vector<std::pair<double, double>> P;
    for (int i = 0; i < 64; i++) { double t = 2 * M_PI * i / 64; P.push_back({c.x() + splitR * std::cos(t), c.y() + splitR * std::sin(t)}); }
    out.push_back({P, QColor(0xff, 0xbe, 0x5a), true});
  }
  if ((tool_ == "brush" || tool_ == "tmask" || tool_ == "tlobe" || (tool_ == "tbrush" && room() == "topo") || shellBrushActive() || weightBrushOn() || ribBrushOn()) && (inViewport_ || drag_ == Drag::BrushAdjust) && !(tool_ == "tmask" && drag_ == Drag::Topo && td_.type == 5)) {
    QPointF c = drag_ == Drag::BrushAdjust ? bd_.start : mouse_;
    for (double r : {brushR, std::max(2.0, brushR * brushHard)}) {
      std::vector<std::pair<double, double>> P; for (int i = 0; i < 64; i++) { double t = 2 * M_PI * i / 64; P.push_back({c.x() + r * std::cos(t), c.y() + r * std::sin(t)}); }
      out.push_back({P, r == brushR ? QColor(0xff, 0xbe, 0x5a) : QColor(0xb0, 0x88, 0x48), true});
    }
  }
  // the Curve tool: from the last point to where the next one goes
  if (ghost_ && drawing_ >= 0) if (const Item* c = scene_->item(drawing_); c && !c->anchors.empty()) {
    Projector P = projector(); ScreenPt a = P.toScreen(scene_->anchor(c->anchors.back())->pos), b = P.toScreen(*ghost_);
    if (a.z < 1 && b.z < 1) out.push_back({{{a.x, a.y}, {b.x, b.y}}, QColor(0xff, 0xaa, 0x33), false});
  }
  return out;
}

// ---------- picking ----------

Projector Viewport::projector() const { return Projector(cam_, paneW(), paneH()); }
Ray Viewport::ray(const QPointF& m) const { return rayAt(cam_, m.x(), m.y(), paneW(), paneH()); }

// pickables(): the shown walls, meshes (the head, imported, converted) and a selected cocoon's shell
std::optional<RayHit> Viewport::pickSurface(const QPointF& m) const {
  std::vector<PickMesh> list; std::deque<std::vector<Vec3>> own;   // (own: positions made here — a shell's; a deque: they stay put)
  // (a mesh's triangles and box: made once per geometry — the same PolyMesh — not on every click / wheel step)
  static std::map<const PolyMesh*, std::pair<std::weak_ptr<PolyMesh>, std::pair<std::vector<uint32_t>, Box3>>> triCache;
  for (auto c = triCache.begin(); c != triCache.end();) { if (c->second.first.expired()) c = triCache.erase(c); else ++c; }
  auto polyTris = [&](const std::shared_ptr<PolyMesh>& sp) -> const std::pair<std::vector<uint32_t>, Box3>& {
    auto f = triCache.find(sp.get()); if (f != triCache.end() && f->second.first.lock() == sp && f->second.second.second.empty() == sp->pos.empty()) return f->second.second;
    const PolyMesh& p = *sp; std::vector<uint32_t> t;
    for (int fi = 0; fi < p.faceCount(); fi++) { int s = p.faceStart[fi], e = p.faceStart[fi + 1]; for (int k = s + 1; k < e - 1; k++) { t.push_back(p.faceVerts[s]); t.push_back(p.faceVerts[k]); t.push_back(p.faceVerts[k + 1]); } }
    auto& slot = triCache[sp.get()]; slot = {sp, {std::move(t), p.bounds()}}; return slot.second;
  };
  const Ray R = ray(m); std::optional<RayHit> heavyHit;
  for (auto& [id, it] : scene_->items) {
    if (!it.poly || !scene_->isVisible(it)) continue;
    if (it.raw.contains("collCap")) continue;                   // (a collision capsule: never picked in the viewport — it wraps the head)
    if (auto hh = heavyRay(id, R, false)) { if (*hh && (!heavyHit || (*hh)->t < heavyHit->t)) heavyHit = *hh; continue; }   // (a heavy mesh: its hierarchy)
    // (a converted mesh moved by the gizmo changes its points in place: its box made anew then)
    const auto& c = polyTris(it.poly);
    PickMesh pm; pm.itemId = id; pm.pos = &it.poly->pos; pm.trisRef = &c.first; pm.box = it.type == ItemType::Mesh ? it.poly->bounds() : c.second; list.push_back(std::move(pm));
  }
  if (ds_.showSurfaces) {
    for (auto& [id, w] : hair_.walls) {
      const Item* sf = scene_->item(id); if (!sf || !scene_->isVisible(*sf)) continue;
      PickMesh pm; pm.itemId = id; pm.pos = &w.pos;
      for (auto& q : w.quads) for (int k : {0, 1, 2, 0, 2, 3}) pm.tris.push_back(uint32_t(q[k]));
      for (auto& p : w.pos) pm.box.expand(p);
      list.push_back(std::move(pm));
    }
    for (auto& [id, k] : crowns_) {                      // (the crowns' meshes)
      const Item* it = scene_->item(id); if (!it || !scene_->isVisible(*it)) continue;
      PickMesh pm; pm.itemId = id; pm.pos = &k.pos;
      for (auto& f : k.faces) for (size_t q = 1; q + 1 < f.size(); q++) { pm.tris.push_back(uint32_t(f[0])); pm.tris.push_back(uint32_t(f[q])); pm.tris.push_back(uint32_t(f[q + 1])); }
      for (auto& p : k.pos) pm.box.expand(p);
      list.push_back(std::move(pm));
    }
    for (auto& [id, r] : ribs_) {                        // (the flat locks' ribbons: the curve)
      PickMesh pm; pm.itemId = id; auto& P = own.emplace_back(); const size_t M = r.rings.empty() ? 0 : r.rings[0].size();
      for (auto& R : r.rings) P.insert(P.end(), R.begin(), R.end()); pm.pos = &P;
      for (size_t i = 0; i + 1 < r.rings.size(); i++) for (size_t j = 0; j < (r.tube ? M : M - 1); j++) { size_t j1 = (j + 1) % M, a = i * M + j, b = i * M + j1, d = (i + 1) * M + j1, e2 = (i + 1) * M + j; for (size_t v : {a, b, d, a, d, e2}) pm.tris.push_back(uint32_t(v)); }
      for (auto& p : P) pm.box.expand(p);
      list.push_back(std::move(pm));
    }
    for (auto& [id, t] : tubes_) {                       // (the curve modifiers' surfaces)
      PickMesh pm; pm.itemId = t.item >= 0 ? t.item : id; pm.pos = t.mesh ? &t.mesh->pos : &t.pos;
      if (t.mesh) { const PolyMesh& M = *t.mesh; for (int f = 0; f < M.faceCount(); f++) { const int s = M.faceStart[size_t(f)], e = M.faceStart[size_t(f) + 1]; for (int k = s + 1; k + 1 < e; k++) for (int v : {s, k, k + 1}) pm.tris.push_back(uint32_t(M.faceVerts[size_t(v)])); }
        for (auto& p : M.pos) pm.box.expand(p); list.push_back(std::move(pm)); continue; }
      for (int i = 0; i + 1 < t.NR; i++) for (int j = 0; j < t.n; j++) { int j1 = (j + 1) % t.n, a = i * t.n + j, b = i * t.n + j1, d = (i + 1) * t.n + j1, e = (i + 1) * t.n + j; for (int v : {a, b, d, a, d, e}) pm.tris.push_back(uint32_t(v)); }
      for (auto& p : t.pos) pm.box.expand(p);
      list.push_back(std::move(pm));
    }
    for (auto& [id, k] : hair_.cocoons()) {
      const Item* it = scene_->item(id); if (!it || !sel_.hasItem(id) || !scene_->isVisible(*it)) continue;
      const CocoonGeo& G = k.geo; const int M = 24;
      own.emplace_back(); auto& P = own.back();
      for (int i = 0; i < G.N; i++) for (int j = 0; j < M; j++) { double th = j * 2 * 3.14159265358979323846 / M; P.push_back(G.c[i] + G.u[i] * (std::cos(th) * G.a[i]) + G.v[i] * (std::sin(th) * G.b[i])); }
      PickMesh pm; pm.itemId = id; pm.pos = &P;
      for (int i = 0; i < G.N - 1; i++) for (int j = 0; j < M; j++) { int j1 = (j + 1) % M, a = i * M + j, b = i * M + j1, d = (i + 1) * M + j1, e = (i + 1) * M + j; for (int v : {a, b, d, a, d, e}) pm.tris.push_back(uint32_t(v)); }
      for (auto& p : P) pm.box.expand(p);
      list.push_back(std::move(pm));
    }
  }
  auto h = raycast(R, list); if (heavyHit && (!h || heavyHit->t < h->t)) h = heavyHit;
  return h;
}

// pickAnchor: the shown anchor nearest the cursor within 10 px
#ifdef Q_OS_WIN
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int);
#endif
// B held right now (asked of the system: the key's press may have gone to a text field — B + click on an anchor still works)
static bool bHeld() {
#ifdef Q_OS_WIN
  return (GetAsyncKeyState(0x42) & 0x8000) != 0 && !(QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier));
#else
  return false;
#endif
}

int Viewport::pickAnchor(const QPointF& m) const {
  Projector P = projector(); int best = -1; double bd = 10;
  for (auto& a : anchors_) {
    const Anchor* an = scene_->anchor(a.id); if (!an) continue;
    ScreenPt s = P.toScreen(an->pos); if (s.z > 1) continue;
    double d = std::hypot(s.x - m.x(), s.y - m.y()); if (d < bd) { bd = d; best = a.id; }
  }
  return best;
}

// pickCurve: the shown curve nearest the cursor within 7 px (hairOnly — no disc rings; noService — no HC / JC / EC)
std::optional<Viewport::PickedCurve> Viewport::pickCurve(const QPointF& m, bool hairOnly, bool noService) const {
  if (!ds_.showCurves) return std::nullopt;
  Projector P = projector(); std::optional<PickedCurve> best; double bd = 7;
  for (const Item* c : scene_->curves()) {
    if ((hairOnly && c->ring >= 0) || (noService && (c->junc || c->hasPath())) || !curveVisible(*c)) continue;
    auto it = hair_.curves.find(c->id); if (it == hair_.curves.end()) continue;
    auto h = nearestOnScreen(P, it->second.p, m.x(), m.y());
    if (h && h->d < bd) { bd = h->d; best = PickedCurve{c->id, it->second.seg[h->i], h->i, h->t, h->d}; }
  }
  return best;
}

// paintSel: Ctrl + a stroke over curves — the first one touched decides: select (it wasn't) or deselect; curves welded
// along the touched segment (a Y trunk) go together
void Viewport::paintSel(const QPointF& m) {
  QPointF from = ps_.last ? *ps_.last : m;
  int n = std::max(1, int(std::ceil(std::hypot(m.x() - from.x(), m.y() - from.y()) / 4)));
  bool changed = false;
  for (int i = 1; i <= n; i++) {
    auto ch = pickCurve(from + (m - from) * (double(i) / n), true, true); if (!ch) continue;
    ps_.touched = true;
    if (ps_.mode.isEmpty()) ps_.mode = sel_.hasItem(ch->id) ? "remove" : "add";
    const Item* c = scene_->item(ch->id); std::vector<int> ids{ch->id};
    if (ch->seg + 1 < int(c->anchors.size())) {              // curvesOnSegment
      int p = c->anchors[ch->seg], q = c->anchors[ch->seg + 1];
      for (const Item* h : scene_->curves()) {
        if (h == c || h->ring >= 0 || !scene_->isVisible(*h) || h->junc || h->hasPath()) continue;
        auto pi = std::find(h->anchors.begin(), h->anchors.end(), p), qi = std::find(h->anchors.begin(), h->anchors.end(), q);
        if (pi != h->anchors.end() && qi != h->anchors.end() && std::abs(int(pi - qi)) == 1) ids.push_back(h->id);
      }
    }
    for (int id : ids) {
      bool has = sel_.hasItem(id);
      if (ps_.mode == "add" && !has) { sel_.items.push_back(id); changed = true; }
      if (ps_.mode == "remove" && has) { sel_.items.erase(std::find(sel_.items.begin(), sel_.items.end(), id)); changed = true; }
    }
  }
  ps_.last = m;
  if (changed) onSelChange();
}

// ---------- edits ----------

void Viewport::markCurves(const std::set<int>& ids) { pendingCurves_.insert(ids.begin(), ids.end()); requestUpdate(); }

// the meshes the Extra tab works on: selected imported meshes and the meshes of selected mesh modifiers
std::vector<int> Viewport::modMeshes() const {
  std::vector<int> out;
  for (int id : sel_.items) { const Item* it = scene_->item(id); if (!it) continue; int m = it->type == ItemType::IMesh ? id : it->type == ItemType::XMod && it->raw.value("mesh").isDouble() ? it->raw.value("mesh").toInt() : -1;
    if (m >= 0 && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m); }
  return out;
}
// the weights are coloured only while worked on: the modifier selected, or its brush on with this mesh among those it paints
bool Viewport::meshWeightsShown(int mesh) const {
  std::vector<int> xs; for (int x : meshmods::modsOf(*scene_, mesh)) { QString k = scene_->item(x)->raw.value("kind").toString(); if (k == "thick" || k == "mask") xs.push_back(x); }
  if (xs.empty()) return false;
  for (int x : xs) if (sel_.hasItem(x)) return true;
  if (!(tool_ == "modifier" && modTab == "extra" && maskBrush.on && (maskBrush.kind == "mask" || maskBrush.kind == "thick"))) return false;
  auto selM = modMeshes(); return !selM.empty() ? std::find(selM.begin(), selM.end(), mesh) != selM.end() : tube::modTubes(*scene_, sel_).empty();
}

void Viewport::beforeFrame() {
  ProfScope prof_("beforeFrame");
  if (!meshmods::refresh(*scene_, meshSig_, [this](int m) { return meshWeightsShown(m); }).empty()) staticRev_++;   // (imported meshes: their modifiers)
  heavyTick();                                             // (heavy meshes: their LOD / hierarchy made in the background)
  // ⇶ anchors glued by Snap to path: one moved — both (every frame, as the HTML version's frameBody)
  if (auto moved = aops::linkSync(*scene_, linkLast_); !moved.empty()) { auto t = curvesTouching(*scene_, moved); pendingCurves_.insert(t.begin(), t.end()); }
  crownFrame();                                            // (symmetric crowns, anchors attached to connectors)
  ribFollow();                                             // (flat locks started from anchors: their roots follow them)
  if (pendingCurves_.empty()) { syncTubes(); syncCrowns(); syncRibs(); syncKinks(); return; }
  bool draft = drag_ == Drag::Anchor || drag_ == Drag::Gizmo || drag_ == Drag::Brush || drag_ == Drag::BrushSmooth || drag_ == Drag::Inflate;
  for (int id : pendingCurves_) if (const Item* c = scene_->item(id); c && c->ring >= 0) { ringsDirty_ = true; break; }   // (a disc's rings: its surface)
  HairChanges ch = hair_.apply(*scene_, pendingCurves_, draft);
  pendingCurves_.clear();
  dirtyWalls_.insert(ch.walls.begin(), ch.walls.end());
  if (!ch.cocoons.empty()) ringsDirty_ = true;
  overlayRev_++;
  syncTubes(); syncCrowns(); syncRibs(); syncKinks();
}

// the selected walls' dents, looked for again once they were rebuilt or the selection changed (not while dragging)
void Viewport::syncKinks() {
  if (drag_ == Drag::Anchor || drag_ == Drag::Gizmo || drag_ == Drag::Brush || drag_ == Drag::BrushSmooth || drag_ == Drag::Inflate) return;
  QString key = QString::number(scene_->revision); std::vector<int> ws;
  for (int id : sel_.items) if (const Item* w = scene_->item(id); w && w->type == ItemType::Surface) { ws.push_back(id); key += "," + QString::number(id); }
  if (key == kinkKey_) return;
  kinkKey_ = key; size_t was = kinks_.size(); kinks_.clear();
  for (int w : ws) { auto k = walltools::wallKinks(*scene_, hair_.curves, w); kinks_.insert(kinks_.end(), k.begin(), k.end()); }
  overlayRev_++;
  if (!kinks_.empty() || was) QTimer::singleShot(0, this, [this] { emit brushChanged(); });   // (their list in Settings)
}

void Viewport::reloadHair() {
  pendingCurves_.clear(); dirtyWalls_.clear();
  hair_.load(*scene_);
  allWalls_ = true; ringsDirty_ = true; overlayRev_++;
  staticRev_++;                                          // (meshes made / gone — Convert, its undo — and groups hidden)
  requestUpdate();
}

// Convert to geometry: the selected cocoons' locks → meshes in the Meshes group, their groups hidden (Ctrl+Z — all back)
void Viewport::convertSelected() {
  if (beforeHeavy) beforeHeavy();                            // (a recovery copy first: heavy)
  ui::BusyScope bz(tr("Converting"));
  ConvertResult r = convertCocoons(*scene_, hair_, sel_.items, cam_.sceneCenter);
  if (r.meshes.empty()) { emit statusText(r.message); return; }
  scene_->revision++;
  commit(true);
  emit statusText(r.message);
  if (r.lockWeld) {                                      // (the joints to redo — red, after the commit)
    showWeldSpots(*r.lockWeld);
    if (!r.lockWeld->problems.empty()) emit weldIssues(r.lockWeld->problems, true);
  }
}

// the places drawn: the problems (not where a fix is under review), the places marked correct, the fixes under review
void Viewport::showWeldSpots(const LockWeldResult& lw) {
  std::vector<WeldProblem> fx;
  if (wfix_) for (size_t i = 0; i < wfix_->fixes.size(); i++) {
    const WeldFix& f = wfix_->fixes[i]; if (f.done) continue;
    WeldProblem p; p.c = f.c; for (int id : f.ks) if (scene_->item(id)) p.locks.push_back(id); p.fix = int(i);
    if (!p.locks.empty()) fx.push_back(p);
  }
  spots_.clear();
  for (auto& p : lw.problems) {
    bool nearFix = false; for (auto& x : fx) if (x.c.distanceTo(p.c) < 1.5 * hair_.cocoonSystem()->loopStep(x.locks[0])) nearFix = true;
    if (!nearFix) spots_.push_back(p);
  }
  spots_.insert(spots_.end(), lw.accepted.begin(), lw.accepted.end());
  spots_.insert(spots_.end(), fx.begin(), fx.end());
  weldHot_ = -1; overlayRev_++; requestUpdate();
}

std::optional<LockWeldResult> Viewport::checkConnections(bool report) {
  auto lw = hm::checkConnections(*scene_, hair_, sel_.items);
  if (!lw) { emit statusText(tr("Nothing to check: no lock meshes")); return lw; }
  showWeldSpots(*lw);
  emit weldIssues(lw->problems, report);
  return lw;
}

void Viewport::afterSceneSwap() {
  pendingCurves_.clear(); dirtyWalls_.clear();
  std::vector<int> keep; for (int id : sel_.items) if (scene_->item(id)) keep.push_back(id); sel_.items = keep;
  std::vector<int> ka; for (int id : sel_.anchors) if (scene_->anchor(id)) ka.push_back(id); sel_.anchors = ka;
  allWalls_ = true; ringsDirty_ = true; overlayRev_++; staticRev_++;
  onSelChange(); emit sceneEdited(); requestUpdate();
}

int Viewport::autoFix(const std::vector<WeldProblem>& problems, const std::function<void(double)>& onStep) {
  wfix_.reset();
  AutoFixResult r = weldAutoFix(*scene_, hair_, problems, onStep);
  int n = int(r.fixes.size());
  if (n) wfix_ = std::move(r);
  afterSceneSwap();
  checkConnections(false);
  return n;
}

void Viewport::fixShow(int i, bool on) {
  if (!wfix_ || i < 0 || i >= int(wfix_->fixes.size())) return;
  wfix_->fixes[size_t(i)].on = on;
  weldFixApply(*scene_, hair_, *wfix_); afterSceneSwap();
  checkConnections(false);
  emit statusText(on ? tr("After: %1").arg(wfix_->fixes[size_t(i)].op.txt) : tr("Before the fix"));
}

void Viewport::fixDecide(int i, bool accept) {
  if (!wfix_ || i < 0 || i >= int(wfix_->fixes.size())) return;
  WeldFix& f = wfix_->fixes[size_t(i)]; QString txt = f.op.txt;
  f.done = true; if (f.on != accept) { f.on = accept; weldFixApply(*scene_, hair_, *wfix_); afterSceneSwap(); }
  wfixBusy_ = true; commit(true); wfixBusy_ = false;
  int left = 0; for (auto& x : wfix_->fixes) if (!x.done) left++;
  if (!left) wfix_.reset();
  checkConnections(false);
  emit statusText((accept ? tr("Accepted: %1").arg(txt) : tr("Rejected — the place is as it was")) + (left ? tr(" · %1 more to review (blue)").arg(left) : QString()));
}

void Viewport::fixAll(bool accept) {
  if (!wfix_) return;
  int n = 0; for (auto& f : wfix_->fixes) if (!f.done) { f.done = true; f.on = accept; n++; }
  weldFixApply(*scene_, hair_, *wfix_); afterSceneSwap();
  wfixBusy_ = true; commit(true); wfixBusy_ = false; wfix_.reset();
  checkConnections(false);
  emit statusText(tr("%1: %2 fix%3").arg(accept ? tr("Accepted") : tr("Rejected")).arg(n).arg(n == 1 ? "" : "es"));
}

void Viewport::endFixReview() {
  if (!wfix_) return;
  wfixBusy_ = true; commit(true); wfixBusy_ = false; wfix_.reset();
  emit statusText(tr("The fixes stay as they are shown"));
}

void Viewport::setTool(const QString& t) {
  if (t == tool_) return;
  if (drawing_ >= 0 && t != "draw") finishDrawing();
  if (t != "cut") { if (scut_) cancelSurfCut(); splitViz_.clear(); }
  if (t != "lockex") lockExReset();                       // (Lock Extrude: another tool — its extrusion stays as it is)
  tool_ = t;
  // (a modelling tool from the Object mode: the mode it works in — the PolyPen vertices, Bevel / Inset polygons, the
  // rest edges)
  if (isModelTool() && t != "tpatch" && t != "tlobe" && topoMode == "object" && topoMesh() >= 0 && !topoHeavy(topoMesh()))   // (PatchTool: works in any mode)
    setTopoMode(t == "tpen" ? lastEditMode_ : t == "tweld" || t == "tslide" ? "vert" : t == "treveal" || t == "tbevel" || t == "tinset" || t == "tnormals" || t == "tmirror" ? "poly" : "edge", true);
  if (!isModelTool()) { tt_.chain.clear(); tt_.loopPrev.clear(); tt_.le = -1; }
  topoRev_++; overlayRev_++;                              // (the tool's own drawing — PatchTool's curves — made anew: else only after the next edit)
  updateGizmo();
  syncCursor();
  emit toolChanged(t);
  requestUpdate();
}

// the pointer: crosshair for the drawing tools, the arrow with the brush beside it while a painting brush is on (the snap
// mask, the shell masks, the curve modifiers' weights, the flat locks' brush)
void Viewport::syncCursor() {
  const QString& t = tool_;
  static const char* SEL[] = {"cursor", "rect", "lasso", "fill", "outline", "loop", "ring"};   // (the Topology room's Select Cursor: its kind beside the arrow)
  int kind = (t == "tmask" || t == "tbrush" || t == "tlobe" || shellBrushActive() || weightBrushOn() || ribBrushOn()) ? 2 : (t == "draw" || t == "guide" || t == "crown" || t == "edge" || t == "cut" || t == "tpen" || t == "tknife" || t == "tplane" || t == "tloop") ? 1 : 0;
  if (kind == 0 && t == "cursor" && room() == "topo") for (int i = 0; i < 7; i++) if (tset.selTool == SEL[i]) kind = 10 + i;
  if (kind == cursorKind_) return; cursorKind_ = kind;
  if (kind >= 10) setCursor(ui::badgeCursor(QString("cur_tsel_") + SEL[kind - 10]));
  else if (kind == 2) setCursor(ui::badgeCursor("brush_paint")); else setCursor(kind == 1 ? Qt::CrossCursor : Qt::ArrowCursor);
}

// the lock groups of the selected cocoons (selCocoonGroups)
std::vector<int> Viewport::selectedCocoonGroups() const {
  std::vector<int> out; const CocoonSystem* cs = hair_.cocoonSystem(); if (!cs) return out;
  for (int id : sel_.items) { const Item* k = scene_->item(id); if (!k || k->type != ItemType::Cocoon) continue;
    int g = const_cast<CocoonSystem*>(cs)->lockGroup(id); if (g >= 0 && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g); }
  return out;
}

namespace { const char* CONV_KEYS[] = {"step", "dev"}; }

// (groupConv: a group's settings, the project's where it has none; the step — the project's loop step)
QJsonValue Viewport::convValue(const QString& key) const {
  QJsonObject S = scene_->fileRaw.value("settings").toObject();
  auto cg = selectedCocoonGroups();
  if (!cg.empty()) {
    QJsonObject c = scene_->item(cg[0])->raw.value("conv").toObject();
    if (key == "step") { double st = c.value("step").toDouble(0); if (st > 0) return st; double dn = c.value("density").toDouble(0); return dn > 0 ? QJsonValue(scene_->bounds().size().y / dn) : QJsonValue(projectStep(*scene_)); }   // (an older group: a density)
    if (c.contains(key)) return c.value(key);
  }
  if (key == "step") return projectStep(*scene_);
  return S.value(key);
}

void Viewport::setConv(const QString& key, const QJsonValue& v, bool commitNow) {
  QJsonObject S = scene_->fileRaw.value("settings").toObject();
  auto cg = selectedCocoonGroups();
  if (!cg.empty()) for (int gid : cg) {
    Item* g = scene_->item(gid); QJsonObject c = g->raw.value("conv").toObject();
    if (!c.contains("step") || c.value("step").isNull()) c.insert("step", projectStep(*scene_));
    for (const char* k : CONV_KEYS) if (!c.contains(k)) c.insert(k, S.value(k));
    c.insert(key, v); g->raw.insert("conv", c);
  } else {
    S.insert(key, v);
    // (none selected — the project's step: the groups that went by it (a crown, its continuation — made with the project's
    // step) take the new one too; else the project's step stays whatever most walls' groups use, and nothing changes)
    if (key == "step") { const double was = projectStep(*scene_);
      for (auto& [id, g] : scene_->items) { if (g.type != ItemType::Group) continue; QJsonObject c = g.raw.value("conv").toObject(); const double st = c.value("step").toDouble(0);
        if (st > 0 && std::abs(st / was - 1) < 0.02) { c.insert("step", v); g.raw.insert("conv", c); } } }
  }
  if (key == "step") S.insert("step", v);                    // (the last step set is the project's loop step)
  scene_->fileRaw.insert("settings", S);
  scene_->revision++;
  reloadHair();
  if (commitNow) commit(false);
}

void Viewport::setWeldSetting(const QString& key, const QJsonValue& v) {
  scene_->fileRaw.insert(key, v);
  commit(false);
}

// RMB on a pin: Ignore (a place to fix — "correct as it is", nothing welded there; an 'off' place only no longer
// reported) / Weld again (a place marked correct) — then the places checked anew
bool Viewport::pickWeldSpot(const QPointF& m) {
  if (spots_.empty()) return false;
  Projector P = projector(); int best = -1; double bd = 22;
  for (size_t i = 0; i < spots_.size(); i++) {
    ScreenPt s = P.toScreen(spots_[i].c); if (s.z >= 1) continue;
    double d = std::min(std::hypot(m.x() - s.x, m.y() - (s.y - 22)), std::hypot(m.x() - s.x, m.y() - s.y) + 6);   // (the head, the tip)
    if (d < bd) { bd = d; best = int(i); }
  }
  if (best < 0) return false;
  const WeldProblem sp = spots_[size_t(best)];
  setWeldHot(best);
  if (sp.fix >= 0 && wfix_ && sp.fix < int(wfix_->fixes.size())) {        // (an auto-fix under review)
    const WeldFix f = wfix_->fixes[size_t(sp.fix)];
    QMenu menu; menu.setToolTipsVisible(true);
    QAction* show = menu.addAction(f.on ? tr("Show before") : tr("Show after")); show->setToolTip(tr("Switch this place between before and after the fix (%1)").arg(f.op.txt));
    QAction* acc = menu.addAction(tr("Accept")); acc->setToolTip(tr("Keep the fix: %1").arg(f.op.txt));
    QAction* rej = menu.addAction(tr("Reject")); rej->setToolTip(tr("Put this place back as it was"));
    QAction* got = menu.exec(toGlobal(m));
    if (got == show) fixShow(sp.fix, !f.on); else if (got == acc) fixDecide(sp.fix, true); else if (got == rej) fixDecide(sp.fix, false);
    return true;
  }
  QMenu menu; QString what = weldIssueText(*scene_, hair_, sp);
  QAction* a = menu.addAction(sp.ok ? tr("Weld again") : tr("Ignore"));
  a->setToolTip(sp.ok ? tr("Weld this place again (it was marked as correct)") : tr("%1 — a correct place: don't weld here, leave it as it is").arg(what));
  menu.setToolTipsVisible(true);
  if (menu.exec(toGlobal(m)) != a) return true;
  weldSpotSet(*scene_, hair_, sp, !sp.ok);
  commit(false);
  checkConnections(false);
  emit statusText(sp.ok ? tr("Welded again: %1").arg(what) : tr("Ignored: %1 — left as it is, no longer reported (green)").arg(what));
  return true;
}

// (the project's keys undone with the edits: the weld settings and marked places, the shell modifiers all off)
namespace { const char* WELD_KEYS[] = {"weld", "lockWeld", "lockWeldR", "weldOk", "weldSnap", "shellModsOff"}; }
Viewport::Snapshot Viewport::snapshot() const {
  QJsonObject w; for (const char* k : WELD_KEYS) if (scene_->fileRaw.contains(k)) w.insert(k, scene_->fileRaw.value(k));
  std::shared_ptr<const void> ch; if (tt_.ec.m >= 0) ch = std::make_shared<const decltype(tt_.ec)>(tt_.ec);   // (the EdgeChain curve: its shape back with the step)
  return {scene_->items, scene_->anchors, scene_->roots, sel_, w, scene_->refPlanes, tsel_, topoMode, ch};
}

// commit: the network kept up after the edit (merged, cut at the forks, joined, named — the HTML version's order),
// the geometry built anew when curves came / went, then a snapshot for undo
void Viewport::commit(bool topology) {
  ProfScope prof_("commit");
  // (an Auto-fix under review and something else edited: the review ends, the fixes stay as they are now)
  if (wfix_ && !wfixBusy_) {
    wfix_.reset();
    std::vector<WeldProblem> s; for (auto& p : spots_) if (p.fix < 0) s.push_back(p); spots_ = s; overlayRev_++;
    emit statusText(tr("Auto-fix review ended — the fixes stay as they are now"));
  }
  Network::Upkeep u;
  if (room() == "hair") { Network net(*scene_, hair_.curves, sel_.items, sel_.anchors); u = net.upkeep(netSig_); }   // (the hair logic of curves: only in its room)
  if (topology || u.changed()) reloadHair();
  const int dropped = int(history_.size()) - (histIdx_ + 1);   // (made back in the history: the later steps go)
  history_.resize(size_t(histIdx_ + 1));
  { ProfScope p2("commit.snapshot"); history_.push_back(snapshot()); }
  { Snapshot& s = history_.back(); s.weak = stepLabel_.isEmpty(); s.label = s.weak ? guessStepLabel() : stepLabel_; s.at = QDateTime::currentMSecsSinceEpoch(); stepLabel_.clear(); }
  if (history_.size() > 300) history_.erase(history_.begin());
  histIdx_ = int(history_.size()) - 1;
  if (dropped > 0) emit historyOverwritten(dropped, bySlider_);
  bySlider_ = false;
  emit historyChanged();
  changes_++; emit changed();
  { ProfScope p3("commit.onSelChange"); onSelChange(); }
  if (topology || u.changed() || u.renamed) emit sceneEdited();
}

void Viewport::restore(const Snapshot& s) {
  lockExReset();   // (Lock Extrude's live extrusion: its curves and walls may be gone with the step — the next drag makes it anew)
  ProfScope prof_("restore");
  if (s.chain) tt_.ec = *static_cast<const decltype(tt_.ec)*>(s.chain.get()); else if (tt_.ec.m >= 0) tt_.ec = {};   // (the EdgeChain curve as it was at that step)
  // (what differs from now: only anchors moved, curves reshaped, walls' settings — those rebuilt; anything else — items
  // come or gone, cocoons, groups, meshes — the whole hair built anew)
  bool whole = s.items.size() != scene_->items.size() || s.roots != scene_->roots;
  std::vector<int> movedA; std::set<int> curvesCh, wallsCh;
  if (!whole) for (auto& [id, it] : s.items) {
    auto f = scene_->items.find(id); if (f == scene_->items.end()) { whole = true; break; }
    const Item& o = f->second;
    if (it.type != o.type || it.parent != o.parent || it.children != o.children || it.visible != o.visible) { whole = true; break; }
    if (it.type == ItemType::Curve) { if (it.anchors != o.anchors || it.closed != o.closed || it.path != o.path || it.ring != o.ring || it.conn != o.conn || it.junc != o.junc || it.seg != o.seg) { whole = true; break; }
      bool same = it.ovr.size() == o.ovr.size() && it.round == o.round && it.fair == o.fair && it.raw == o.raw;
      if (same) for (auto i = it.ovr.begin(), j = o.ovr.begin(); i != it.ovr.end(); ++i, ++j) if (i->first != j->first || i->second.in.has_value() != j->second.in.has_value() || i->second.out.has_value() != j->second.out.has_value()
          || (i->second.in && (*i->second.in - *j->second.in).lengthSq() > 0) || (i->second.out && (*i->second.out - *j->second.out).lengthSq() > 0)) { same = false; break; }
      if (!same) curvesCh.insert(id); continue; }
    if (it.type == ItemType::Surface) { if (it.a != o.a || it.b != o.b) { whole = true; break; } if (it.raw != o.raw) wallsCh.insert(id); continue; }
    if (it.raw != o.raw || it.name != o.name || it.color != o.color || it.poly != o.poly || it.base != o.base) { if (it.type == ItemType::Group && it.raw == o.raw && it.poly == o.poly) continue; whole = true; break; }   // (a group renamed / recoloured: nothing to rebuild)
  }
  if (!whole) { if (s.anchors.size() != scene_->anchors.size()) whole = true;
    else for (auto& [id, a] : s.anchors) { auto f = scene_->anchors.find(id); if (f == scene_->anchors.end()) { whole = true; break; }
      if ((a.pos - f->second.pos).lengthSq() > 0 || a.smooth != f->second.smooth || a.pass != f->second.pass || a.raw != f->second.raw) movedA.push_back(id); } }
  // (only anchors / shapes / wall settings changed: written into the items in place — the hair's systems kept for the
  // partial rebuild below (the cocoons' context among them) hold pointers to them; a copied map would leave those dangling)
  if (whole) { scene_->items = s.items; scene_->anchors = s.anchors; }
  else { for (auto& [id, it] : s.items) scene_->items.at(id) = it; for (auto& [id, a] : s.anchors) scene_->anchors.at(id) = a; }
  scene_->roots = s.roots;
  scene_->refPlanes = s.refPlanes; refRev_++; if (!refPlane(refPlane_)) refPlane_ = -1;
  for (const char* k : WELD_KEYS) { if (s.weld.contains(k)) scene_->fileRaw.insert(k, s.weld.value(k)); else scene_->fileRaw.remove(k); }
  // (the Topology room in a component mode: the meshes worked on stay selected — only Object mode takes the selection back)
  if (room() == "topo" && topoMode != "object") { Selection keep; for (int id : sel_.items) if (scene_->item(id)) keep.items.push_back(id); sel_ = keep.items.empty() ? s.sel : keep; }
  else sel_ = s.sel;
  if (wfix_) { wfix_.reset(); std::vector<WeldProblem> sp; for (auto& p : spots_) if (p.fix < 0) sp.push_back(p); spots_ = sp; }   // (undo ends a review)
  if (room() == "topo") {                                    // (the components' selection and its mode back as they were)
    // (the mode stays: the selection back only when it was made in this mode — else none)
    const int ver = tsel_.ver + 1;
    if (s.tmode.isEmpty() || s.tmode == topoMode) tsel_ = s.tsel; else { const int id = tsel_.id; tsel_ = {}; tsel_.id = id; }
    tsel_.ver = ver; thov_ = {};
    staticRev_++; topoRev_++; updateGizmo(); emit topoState();
  }
  scene_->reindex(); scene_->revision++;
  if (whole || room() != "hair") reloadHair();
  else {                                                     // (only what changed rebuilt: the curves on the moved anchors, the reshaped ones, the walls set otherwise)
    std::set<int> cs = curvesTouching(*scene_, movedA); cs.insert(curvesCh.begin(), curvesCh.end());
    HairChanges ch; if (!cs.empty()) ch = hair_.apply(*scene_, cs, false);
    if (!wallsCh.empty()) { HairChanges w = hair_.rebuildWalls(*scene_, wallsCh); ch.walls.insert(w.walls.begin(), w.walls.end()); ch.cocoons.insert(w.cocoons.begin(), w.cocoons.end()); }
    pendingCurves_.clear(); dirtyWalls_.insert(ch.walls.begin(), ch.walls.end()); if (!ch.cocoons.empty()) ringsDirty_ = true;
    overlayRev_++; staticRev_++; requestUpdate();
  }
  changes_++; emit changed();
  { ProfScope p3("commit.onSelChange"); onSelChange(); }
  emit sceneEdited(); emit historyChanged();
}

QJsonObject Viewport::cameraJson() const {
  auto v3 = [](const Vec3& p) { return QJsonArray{p.x, p.y, p.z}; };
  return QJsonObject{{"view", cam_.view}, {"pos", v3(cam_.pos)}, {"quat", QJsonArray{cam_.quat.x, cam_.quat.y, cam_.quat.z, cam_.quat.w}},
                     {"zoom", cam_.zoom}, {"pivot", v3(cam_.pivot)}};
}

// the Topology room's selection changed: noted as an undo step when the gesture (a drag) is over — one per click / drag
void Viewport::tselNote() {
  if (tselPending_ || room() != "topo") return;
  tselPending_ = true;
  QTimer::singleShot(0, this, [this] { tselPending_ = false; if (drag_ == Drag::Topo) { tselPending_ = true; QTimer::singleShot(60, this, [this] { tselPending_ = false; tselNote(); }); return; } tselPush(); });
}
void Viewport::tselPush() {
  ProfScope prof_("tselPush");
  if (histIdx_ < 0 || size_t(histIdx_) >= history_.size()) return;
  const Snapshot& h = history_[size_t(histIdx_)];
  if (h.tsel.id == tsel_.id && h.tsel.f == tsel_.f && h.tsel.e == tsel_.e && h.tsel.v == tsel_.v) return;   // (nothing new — a mode switch alone is no step)
  history_.resize(size_t(histIdx_ + 1)); history_.push_back(snapshot());
  history_.back().label = tr("Selection"); history_.back().weak = false; history_.back().at = QDateTime::currentMSecsSinceEpoch();
  if (history_.size() > 300) history_.erase(history_.begin());
  histIdx_ = int(history_.size()) - 1;
}

// Snap (the Hair room): each anchor onto the nearest point of the shown reference meshes' surface (their snap mask's
// polygons, where one is on)
void Viewport::snapAnchors(const std::vector<int>& list) {
  std::vector<std::shared_ptr<const topo::Bvh>> tg;
  for (auto& [id, t] : scene_->items) if (t.type == ItemType::IMesh && t.poly && t.raw.value("tref").toBool() && scene_->isVisible(t)) if (auto b = bvhOf(id, true)) tg.push_back(b);
  if (tg.empty()) return;
  for (int a : list) { Anchor* an = scene_->anchor(a); if (!an) continue; double bd = 1e300; Vec3 best;
    for (auto& b : tg) { double d; auto q = b->closest(an->pos, nullptr, &d); if (q && d < bd) { bd = d; best = *q; } }
    if (bd < 1e300) an->pos = best; }
}

void Viewport::undo() {
  bySlider_ = false;
  if (room() == "topo" && tool_ == "tknife" && kUndoRedo(false)) return;   // (the knife's curves: their own steps first)
  if (room() == "topo" && tool_ == "tpatch" && patchUndoRedo(false)) return;   // (PatchTool's curves: theirs)
  if (histIdx_ <= 0) { emit statusText(tr("Nothing to undo")); return; }
  const auto used = history_[size_t(histIdx_)].patchUsed;   // (a patch undone: the curves it was made of come back)
  restore(history_[size_t(--histIdx_)]);
  if (used) { pt_.c = *static_cast<const std::vector<PatchCurve>*>(used.get()); pt_.undo.clear(); pt_.redo.clear(); pt_.draw = pt_.selC = pt_.selA = -1; patchChanged(); }
}
void Viewport::redo() {
  bySlider_ = false;
  if (room() == "topo" && tool_ == "tknife" && kUndoRedo(true)) return;
  if (room() == "topo" && tool_ == "tpatch" && patchUndoRedo(true)) return;
  if (histIdx_ + 1 >= int(history_.size())) { emit statusText(tr("Nothing to redo")); return; }
  restore(history_[size_t(++histIdx_)]);
  if (history_[size_t(histIdx_)].patchUsed) { pt_.c.clear(); pt_.undo.clear(); pt_.redo.clear(); pt_.draw = pt_.selC = pt_.selA = -1; patchChanged(); }   // (the patch made again: its curves used up)
}

// ---------- the history slider ----------
void Viewport::historyGoTo(int i) {
  if (i < 0 || i >= int(history_.size()) || i == histIdx_) return;
  histIdx_ = i; restore(history_[size_t(i)]); bySlider_ = true;   // (moved in time by the slider: an edit now — the Time Heist)
}

void Viewport::noteStep(const QString& s, bool done) {
  if (s.trimmed().isEmpty()) return;
  if (!done) { stepLabel_ = s.trimmed(); return; }
  // (a message right after a step: its name, unless the step has a real one already)
  if (histIdx_ >= 0 && size_t(histIdx_) < history_.size()) { Snapshot& h = history_[size_t(histIdx_)];
    if (h.weak && QDateTime::currentMSecsSinceEpoch() - h.at < 700) { h.label = s.trimmed(); h.weak = false; emit historyChanged(); } }
}

QString Viewport::guessStepLabel() const {
  static const std::map<QString, const char*> T = {{"move", "Move"}, {"rotate", "Rotate"}, {"gizmo", "Scale"}, {"draw", "Curve drawn"}, {"brush", "Brush"},
    {"cut", "Scissors"}, {"guide", "Shell modifiers"}, {"modifier", "Curve modifiers"}, {"crown", "Crown"}, {"braid", "Braid"}, {"edge", "Edges"}, {"cursor", "Edit"}};
  auto f = T.find(tool_); return f != T.end() ? tr(f->second) : room() == "topo" ? tr("Topology edit (%1)").arg(tool_) : tr("Edit (%1)").arg(tool_);
}

namespace {
bool sameOvr(const OvrMap& a, const OvrMap& b) {
  if (a.size() != b.size()) return false;
  auto eq = [](const std::optional<Vec3>& x, const std::optional<Vec3>& y) { return x.has_value() == y.has_value() && (!x || (x->x == y->x && x->y == y->y && x->z == y->z)); };
  for (auto i = a.begin(), j = b.begin(); i != a.end(); ++i, ++j) if (i->first != j->first || !eq(i->second.in, j->second.in) || !eq(i->second.out, j->second.out)) return false;
  return true;
}
bool sameItem(const Item& a, const Item& b) {
  return a.name == b.name && a.parent == b.parent && a.visible == b.visible && a.children == b.children && a.color == b.color && a.expanded == b.expanded
      && a.anchors == b.anchors && sameOvr(a.ovr, b.ovr) && a.closed == b.closed && a.junc == b.junc && a.para == b.para && a.ring == b.ring && a.conn == b.conn
      && a.round == b.round && a.fair == b.fair && a.path == b.path && a.hasRib == b.hasRib && a.seg == b.seg && a.a == b.a && a.b == b.b
      && a.poly == b.poly && a.base == b.base && a.headTag == b.headTag && a.raw == b.raw;
}
// an item a later step changed too: each field (each key of its record) the step changed, still as the step left it,
// set back to what it was before the step
void mergeBack(Item& s, const Item& a, const Item& b) {
  auto f = [](auto& sv, const auto& av, const auto& bv) { if (sv == bv && !(av == bv)) sv = av; };
  f(s.name, a.name, b.name); f(s.parent, a.parent, b.parent); f(s.visible, a.visible, b.visible); f(s.color, a.color, b.color);
  f(s.anchors, a.anchors, b.anchors); f(s.closed, a.closed, b.closed); f(s.ring, a.ring, b.ring); f(s.path, a.path, b.path);
  f(s.round, a.round, b.round); f(s.fair, a.fair, b.fair); f(s.a, a.a, b.a); f(s.b, a.b, b.b); f(s.poly, a.poly, b.poly); f(s.base, a.base, b.base); f(s.headTag, a.headTag, b.headTag);
  if (sameOvr(s.ovr, b.ovr) && !sameOvr(a.ovr, b.ovr)) s.ovr = a.ovr;
  QStringList keys = a.raw.keys(); for (const QString& k : b.raw.keys()) if (!keys.contains(k)) keys << k;
  for (const QString& k : keys) if (s.raw.value(k) == b.raw.value(k) && a.raw.value(k) != b.raw.value(k)) { if (a.raw.contains(k)) s.raw.insert(k, a.raw.value(k)); else s.raw.remove(k); }
  // (children: the ones the step brought in taken out, the ones it took out back in — the hierarchy is made whole after)
  for (int c : b.children) if (std::find(a.children.begin(), a.children.end(), c) == a.children.end()) s.children.erase(std::remove(s.children.begin(), s.children.end(), c), s.children.end());
  for (int c : a.children) if (std::find(b.children.begin(), b.children.end(), c) == b.children.end() && std::find(s.children.begin(), s.children.end(), c) == s.children.end()) s.children.push_back(c);
}
bool sameAnchor(const Anchor& a, const Anchor& b) { return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z && a.smooth == b.smooth && a.pass == b.pass && a.raw == b.raw; }
// what an item leans on (a wall its curves, an edge curve its route, a ring its guide, a modifier its source …)
std::vector<int> refsOf(const Item& it) {
  std::vector<int> r; if (it.a >= 0) r.push_back(it.a); if (it.b >= 0) r.push_back(it.b); if (it.ring >= 0) r.push_back(it.ring);
  for (auto& p : it.path) r.push_back(p[0]);
  for (const char* k : {"src", "mesh", "tube", "part", "low"}) if (it.raw.value(k).isDouble()) r.push_back(it.raw.value(k).toInt());
  for (const QJsonValue& v : it.raw.value("rings").toArray()) r.push_back(v.toInt());
  return r;
}
// the hierarchy made whole again: parents that are gone — roots; children lists only of those that say so, every child listed
void fixHierarchy(std::map<int, Item>& items, std::vector<int>& roots) {
  for (auto& [id, it] : items) if (it.parent >= 0 && !items.count(it.parent)) it.parent = -1;
  std::vector<const Item*> byOrder; for (auto& [id, it] : items) byOrder.push_back(&it);
  std::sort(byOrder.begin(), byOrder.end(), [](const Item* a, const Item* b) { return a->order < b->order; });
  std::map<int, std::vector<int>> kids; for (const Item* x : byOrder) kids[x->parent].push_back(x->id);
  auto fix = [&](std::vector<int>& list, int owner) {
    std::set<int> seen; std::vector<int> out;
    for (int c : list) { auto f = items.find(c); if (f != items.end() && f->second.parent == owner && seen.insert(c).second) out.push_back(c); }
    for (int c : kids[owner]) if (seen.insert(c).second) out.push_back(c);
    list = out; };
  for (auto& [id, it] : items) if (!it.children.empty() || kids.count(id)) fix(it.children, id);
  fix(roots, -1);
}
}  // namespace

// a step taken out: what it changed (items, anchors, reference planes, the weld settings) set back as it was before it —
// in every later step too, where that thing is still as the step left it (changed again later: the later change stays);
// what a later step leans on is never taken away. The first step (the project as opened) stays
bool Viewport::historyDelete(int k) {
  if (k <= 0 || k >= int(history_.size())) return false;
  const Snapshot A = history_[size_t(k - 1)], B = history_[size_t(k)];
  std::set<int> chI, chA, chR;
  for (auto& [id, it] : A.items) { auto f = B.items.find(id); if (f == B.items.end() || !sameItem(it, f->second)) chI.insert(id); }
  for (auto& [id, it] : B.items) if (!A.items.count(id)) chI.insert(id);
  for (auto& [id, an] : A.anchors) { auto f = B.anchors.find(id); if (f == B.anchors.end() || !sameAnchor(an, f->second)) chA.insert(id); }
  for (auto& [id, an] : B.anchors) if (!A.anchors.count(id)) chA.insert(id);
  auto rpJ = [](const std::vector<RefPlane>& v) { std::map<int, QJsonObject> m; for (auto& p : v) m[p.id] = refs::toJson(p); return m; };
  const auto rA = rpJ(A.refPlanes), rB = rpJ(B.refPlanes);
  for (auto& [id, j] : rA) { auto f = rB.find(id); if (f == rB.end() || f->second != j) chR.insert(id); }
  for (auto& [id, j] : rB) if (!rA.count(id)) chR.insert(id);
  auto revert = [&](Snapshot& S) {
    for (int id : chI) {
      auto s = S.items.find(id); auto b = B.items.find(id); auto a = A.items.find(id);
      const bool asB = (s == S.items.end() && b == B.items.end()) || (s != S.items.end() && b != B.items.end() && sameItem(s->second, b->second));
      if (!asB) {                                            // (changed again later: only what the step changed and nothing later did goes back)
        if (s != S.items.end() && a != A.items.end() && b != B.items.end()) mergeBack(s->second, a->second, b->second);
        continue;
      }
      if (a == A.items.end()) S.items.erase(id); else S.items[id] = a->second;
    }
    for (int id : chA) {
      auto s = S.anchors.find(id); auto b = B.anchors.find(id); auto a = A.anchors.find(id);
      const bool asB = (s == S.anchors.end() && b == B.anchors.end()) || (s != S.anchors.end() && b != B.anchors.end() && sameAnchor(s->second, b->second));
      if (!asB) continue;
      if (a == A.anchors.end()) S.anchors.erase(id); else S.anchors[id] = a->second;
    }
    // (what is still leaned on comes back: an item a kept one refers to, the anchors of the kept curves)
    for (bool grew = true; grew;) {
      grew = false; std::vector<int> need;
      for (auto& [id, it] : S.items) for (int r : refsOf(it)) if (r >= 0 && !S.items.count(r) && B.items.count(r)) need.push_back(r);
      for (int r : need) if (!S.items.count(r)) { S.items[r] = B.items.at(r); grew = true; }
    }
    for (auto& [id, it] : S.items) for (int an : it.anchors) if (!S.anchors.count(an)) { auto b = B.anchors.find(an); if (b != B.anchors.end()) S.anchors[an] = b->second; }
    if (S.roots == B.roots) S.roots = A.roots;
    if (S.weld == B.weld) S.weld = A.weld;
    if (!chR.empty()) {
      auto rS = rpJ(S.refPlanes); std::map<int, RefPlane> keep; for (auto& p : S.refPlanes) keep[p.id] = p;
      for (int id : chR) {
        const bool asB = (!rS.count(id) && !rB.count(id)) || (rS.count(id) && rB.count(id) && rS[id] == rB.at(id)); if (!asB) continue;
        keep.erase(id); for (auto& p : A.refPlanes) if (p.id == id) keep[id] = p;
      }
      std::vector<RefPlane> out; for (auto& [id, p] : keep) out.push_back(p); S.refPlanes = out;
    }
    fixHierarchy(S.items, S.roots);
  };
  for (size_t j = size_t(k) + 1; j < history_.size(); j++) revert(history_[j]);
  const QString name = B.label;
  history_.erase(history_.begin() + k);
  const bool here = histIdx_ >= k;
  if (here) { histIdx_--; restore(history_[size_t(histIdx_)]); } else emit historyChanged();
  ui::toast(tr("History step taken out: %1").arg(name));
  return true;
}

void Viewport::startAnchorDrag(const std::vector<int>& list, const QPointF& m, const Vec3& ref) {
  ad_ = {}; ad_.list = list;
  auto released = releaseFrozenAround(*scene_, list);
  if (!released.empty()) { std::vector<int> r; for (auto& x : released) r.push_back(x.anchor); markCurves(curvesTouching(*scene_, r)); }
  for (int id : list) ad_.starts.push_back(scene_->anchor(id)->pos);
  ad_.n = cam_.dir(); ad_.ref = ref; ad_.startHit = planeHit(ray(m), ref, ad_.n);
  drag_ = Drag::Anchor;
}

// Shift while dragging one anchor: it sits on the curve nearest the cursor (any group; hidden only by its eye; not its
// own curves) and slides along it — picked again on every move (another one taken when nearer by over 6 px; among
// curves almost equally near — within 4 px — the one nearer the camera; a point behind the head counts 25 px further)
bool Viewport::slideToCurve(const QPointF& m) {
  int a = ad_.list[0];
  Projector P = projector();
  struct Cand { int c; double d, z; Vec3 point; };
  std::vector<Cand> cands; const Cand* cur = nullptr;
  for (const Item* k : scene_->curves()) {
    if (k->ring >= 0 || std::find(k->anchors.begin(), k->anchors.end(), a) != k->anchors.end() || !scene_->isVisible(*k)) continue;
    auto it = hair_.curves.find(k->id); if (it == hair_.curves.end() || it->second.p.size() < 2) continue;
    auto h = nearestOnScreen(P, it->second.p, m.x(), m.y()); if (!h) continue;
    Vec3 pt = it->second.p[h->i].lerp(it->second.p[h->i + 1], h->t);
    cands.push_back({k->id, h->d, P.toScreen(pt).z, pt});
  }
  if (cands.empty()) return false;
  std::vector<PickMesh> head; double d0 = 1e300;
  for (auto& c : cands) d0 = std::min(d0, c.d);
  for (auto& [id, it] : scene_->items) if (it.headTag && it.poly && scene_->isVisible(it)) {
    PickMesh pm; pm.itemId = id; pm.pos = &it.poly->pos; pm.box = it.poly->bounds();
    for (int f = 0; f < it.poly->faceCount(); f++) { int s = it.poly->faceStart[f], e = it.poly->faceStart[f + 1]; for (int q = s + 1; q < e - 1; q++) { pm.tris.push_back(it.poly->faceVerts[s]); pm.tris.push_back(it.poly->faceVerts[q]); pm.tris.push_back(it.poly->faceVerts[q + 1]); } }
    head.push_back(std::move(pm));
  }
  if (!head.empty()) for (auto& r : cands) {
    if (r.d > d0 + 30) continue;
    ScreenPt s = P.toScreen(r.point); Ray ry = ray(QPointF(s.x, s.y));
    auto h = raycast(ry, head); double dp = ry.o.distanceTo(r.point);
    if (h && h->t < dp - 0.002 * cam_.sceneScale) r.d += 25;
  }
  double dmin = 1e300; for (auto& c : cands) dmin = std::min(dmin, c.d);
  const Cand* best = nullptr;
  for (auto& c : cands) if (c.d <= dmin + 4 && (!best || c.z < best->z)) best = &c;
  for (auto& c : cands) if (c.c == ad_.snap) cur = &c;
  if (cur && cur->d <= best->d + 6) best = cur;
  if (best->c != ad_.snap) {
    ad_.snap = best->c; overlayRev_++;
    emit statusText(tr("Sliding along %1 (move the cursor to another curve to switch · release Shift — free move)").arg(scene_->item(best->c)->name));
  }
  scene_->anchor(a)->pos = best->point;
  return true;
}

// RMB on a curve: a new anchor there (in every curve welded along that segment), dragged at once
void Viewport::insertAt(const PickedCurve& ch, const QPointF& m) {
  const auto& S = hair_.curves.at(ch.id).p;
  Vec3 point = S[ch.sample].lerp(S[ch.sample + 1], ch.t);
  int a = scene_->addAnchor(point).id;
  Network net(*scene_, hair_.curves, sel_.items, sel_.anchors);
  net.insertOnSegment(ch.id, ch.seg, a);
  reloadHair();
  sel_.anchors = {a}; sel_.items.clear(); onSelChange();
  startAnchorDrag({a}, m, point);
  ad_.inserted = true;
}

// an RMB stroke over empty space: an anchor wherever it crosses a curve (the selected curves only, if any), the shape
// kept — scissorHits: on each curve the crossing nearest the stroke's middle
void Viewport::anchorLine(const QPointF& p0, const QPointF& p1) {
  std::vector<const Item*> list;
  for (int id : sel_.items) if (const Item* c = scene_->item(id); c && c->type == ItemType::Curve) list.push_back(c);
  bool selected = !list.empty();
  if (!selected) for (const Item* c : scene_->curves()) if (c->ring < 0 && curveVisible(*c)) list.push_back(c);
  Projector P = projector();
  double x0 = p0.x(), y0 = p0.y(), x1 = p1.x(), y1 = p1.y(), mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
  struct Hit { int c, seg; double u; };
  std::vector<Hit> hits;
  for (const Item* c : list) {
    if (c->ring >= 0 || c->closed || !curveVisible(*c)) continue;
    auto it = hair_.curves.find(c->id); if (it == hair_.curves.end() || it->second.p.size() < 2) continue;
    const auto& S = it->second; std::vector<ScreenPt> sp; for (auto& p : S.p) sp.push_back(P.toScreen(p));
    std::optional<Hit> best; double bd = 1e300;
    for (size_t i = 0; i + 1 < sp.size(); i++) {
      const ScreenPt &a = sp[i], &b = sp[i + 1]; if (a.z > 1 || b.z > 1) continue;
      double rx = x1 - x0, ry = y1 - y0, sx = b.x - a.x, sy = b.y - a.y, den = rx * sy - ry * sx; if (std::abs(den) < 1e-9) continue;
      double t = ((a.x - x0) * sy - (a.y - y0) * sx) / den, v = ((a.x - x0) * ry - (a.y - y0) * rx) / den;
      if (t < 0 || t > 1 || v < 0 || v > 1) continue;
      double d = std::hypot(x0 + rx * t - mx, y0 + ry * t - my);
      if (d < bd) { bd = d; int seg = S.seg[i]; best = Hit{c->id, seg, clamp((double(i) - seg * 24 + v) / 24, 0, 1)}; }
    }
    if (best) hits.push_back(*best);
  }
  if (hits.empty()) { emit statusText(selected ? tr("The line doesn't cross the selected curves") : tr("The line doesn't cross any curve")); return; }
  Network net(*scene_, hair_.curves, sel_.items, sel_.anchors);
  std::vector<int> made; std::set<int> done;
  for (auto& h : hits) {
    if (done.count(h.c) || h.u < 0.03 || h.u > 0.97) continue;   // (welded along there — the anchor went in already / an anchor is there)
    int a = net.insertKeepShape(h.c, h.seg, h.u);
    for (const Item* c : scene_->curves()) if (std::find(c->anchors.begin(), c->anchors.end(), a) != c->anchors.end()) done.insert(c->id);
    made.push_back(a);
  }
  sel_.anchors = made;
  commit(true);
  emit statusText(tr("%n anchor(s) added", "", int(made.size())));
}

// findWeldCandidate: a shown anchor within 14 px of a on the screen, not on a curve with it (no disc / flat-lock points)
int Viewport::findWeldCandidate(int a) const {
  auto onRing = [&](int x) { for (const Item* c : scene_->curves()) if (c->ring >= 0 && std::find(c->anchors.begin(), c->anchors.end(), x) != c->anchors.end()) return true; return false; };
  auto onRib = [&](int x) { for (const Item* c : scene_->curves()) if (c->hasRib && std::find(c->anchors.begin(), c->anchors.end(), x) != c->anchors.end()) return true; return false; };
  auto shares = [&](int x, int y) { for (const Item* c : scene_->curves()) { bool hx = false, hy = false; for (int q : c->anchors) { hx |= q == x; hy |= q == y; } if (hx && hy) return true; } return false; };
  // (a curve with a profile modifier and a crown's connector are never welded together)
  auto onConn = [&](int x) { const Anchor* X = scene_->anchor(x); if (X && X->raw.value("at").isObject()) return true; for (const Item* c : scene_->curves()) if (!c->conn.isEmpty() && std::find(c->anchors.begin(), c->anchors.end(), x) != c->anchors.end()) return true; return false; };
  const bool aProf = tube::profiledAt(*scene_, a), aConn = onConn(a);
  if (onRing(a) || onRib(a)) return -1;
  Projector P = projector(); ScreenPt sa = P.toScreen(scene_->anchor(a)->pos);
  int best = -1; double bd = 14;
  for (auto& s : anchors_) {
    int b = s.id; if (b == a || shares(a, b) || onRing(b) || onRib(b)) continue;
    if ((aProf && onConn(b)) || (aConn && tube::profiledAt(*scene_, b))) continue;
    ScreenPt sb = P.toScreen(scene_->anchor(b)->pos); double d = std::hypot(sa.x - sb.x, sa.y - sb.y);
    if (d < bd) { bd = d; best = b; }
  }
  return best;
}

// checkWeld: a moved anchor released near another — the weld menu (the start of one curve onto the end of another:
// reverse one and weld, or join the two into one curve)
void Viewport::checkWeld(int a, const QPointF& m) {
  int b = findWeldCandidate(a); if (b < 0) { checkAttach(a, m); return; }   // (none near: a crown connector near?)
  weldCand_ = b; anchors_ = anchorList(*scene_, ds_, sel_, weldCand_); overlayRev_++; requestUpdate();
  Network probe(*scene_, hair_.curves, sel_.items, sel_.anchors);
  auto ea = probe.soleEnd(a), eb = probe.soleEnd(b);
  QMenu menu; QAction *weld = nullptr, *rev = nullptr, *join = nullptr;
  int st = -1, other = -1;
  if (ea && eb && ea->c != eb->c && ea->start != eb->start) {
    st = ea->start ? ea->c : eb->c; other = ea->start ? eb->c : ea->c;
    menu.setTitle(tr("Start meets end"));
    rev = menu.addAction(tr("⇄ Reverse %1, then weld").arg(scene_->item(st)->name));
    join = menu.addAction(tr("∿ Join into one curve"));
  } else {
    menu.setTitle(tr("Anchors are close"));
    weld = menu.addAction(tr("⛓ Weld anchors into one"));
  }
  menu.addSeparator(); QAction* none = menu.addAction(tr("Don't weld"));
  QAction* got = weldChoice.isEmpty() ? menu.exec(toGlobal(m))
               : weldChoice == "weld" ? weld : weldChoice == "reverse" ? rev : weldChoice == "join" ? join : none;
  weldCand_ = -1;
  if (!got || (got != weld && got != rev && got != join)) { onSelChange(); return; }
  Network net(*scene_, hair_.curves, sel_.items, sel_.anchors);
  if (got == weld) net.weld(a, b);
  else if (got == rev) { net.reverseCurves({st}); net.weld(a, b, [&] { net.recalcSilent({st, other}); }); }
  else net.joinCurves(a, b);
  commit(true);
  emit statusText(got == join ? tr("Joined into one curve") : tr("Anchors welded"));
}

// ---------- camera ----------

std::optional<Vec3> Viewport::pickPoint(const QPointF& wp) const { auto h = pickSurface(wp - paneOrigin()); if (h) return h->point; return std::nullopt; }

void Viewport::frameSelection() {
  if (cam_.view == "uv") { uvFrame(); return; }               // (the UV Editor: the selection's UV, else all of them)
  Box3 b; bool small = false;
  if (!sel_.anchors.empty()) { for (int a : sel_.anchors) if (auto* an = scene_->anchor(a)) b.expand(an->pos); small = true; }
  else for (int id : sel_.items) {
    const Item* it = scene_->item(id); if (!it) continue;
    if (it->poly) b.unite(it->poly->bounds());
    auto addCurve = [&](int cid) { if (const Item* c = scene_->item(cid)) for (int a : c->anchors) if (auto* an = scene_->anchor(a)) b.expand(an->pos); };
    if (it->type == ItemType::Curve) addCurve(id);
    if (it->type == ItemType::Surface) { addCurve(it->a); addCurve(it->b); }
    if (it->type == ItemType::Group) for (auto& [cid, c] : scene_->items) if (scene_->isDescendant(cid, id)) { if (c.type == ItemType::Curve) addCurve(cid); if (c.poly) b.unite(c.poly->bounds()); }
    if (it->type == ItemType::Cocoon) if (auto k = hair_.cocoons().find(id); k != hair_.cocoons().end()) for (auto& p : k->second.geo.c) b.expand(p);
    small = true;
  }
  if (b.empty()) { b = scene_->bounds(); small = false; }
  cam_.frame(b, small);
  requestUpdate();
}

void Viewport::setViewMode(const QString& v) { cam_.setView(v); emit viewChanged(v); requestUpdate(); }

// ---------- input ----------

namespace {
// (the event as the active pane sees it: its position relative to the pane)
QMouseEvent localEvent(QMouseEvent* e, const QPointF& origin) { return QMouseEvent(e->type(), e->position() - origin, e->globalPosition(), e->button(), e->buttons(), e->modifiers(), e->pointingDevice()); }
}  // namespace

void Viewport::mousePressEvent(QMouseEvent* ev) {
  if (drag_ == Drag::None) choosePane(ev->position());
  QMouseEvent le = localEvent(ev, paneOrigin()); QMouseEvent* e = &le;
  QPointF m = e->position(); last_ = start_ = m;
  if (cam_.view == "uv") { uvPress(e); requestUpdate(); return; }   // (the UV Editor: its own picking and navigation)
  bool alt = e->modifiers() & Qt::AltModifier, ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  const int navK = ui::Keymap::get().navFor(ui::Keymap::buttonOf(e->button()), ui::Keymap::modsOf(e->modifiers()));   // (the camera's mouse buttons — Hotkeys → Mouse & camera)
  const int mm = ui::Keymap::modsOf(e->modifiers()), mb = ui::Keymap::buttonOf(e->button());
  // ◎ the weld distance picker: a click sets the circle's diameter, Ctrl+RMB drag — its size (Alt / MMB still navigate)
  if (mergePick && !alt && e->button() != Qt::MiddleButton) {
    if (e->button() == Qt::RightButton && ctrl) { bd_ = {}; bd_.start = m; bd_.r0 = pickR; drag_ = Drag::BrushAdjust; return; }
    if (e->button() == Qt::LeftButton) {
      double d = pickWorld(m); setMergePick(false);
      QJsonObject w = scene_->fileRaw.value("weld").toObject(); if (!w.contains("on")) w.insert("on", true); w.insert("dist", d);
      setWeldSetting("weld", w); emit brushChanged(); ui::toast(tr("Weld distance = %1").arg(QString::number(d, 'g', 3)));
    }
    return;
  }
  // the camera orbits around the last clicked point (any click — Alt / MMB navigation too, as the HTML version)
  QElapsedTimer pickT; pickT.start();
  auto hit = pickSurface(m);
  if (hit) cam_.pivot = hit->point;
  else if (navK && pickT.elapsed() < 6) {   // (a heavy mesh without its hierarchy yet: not 60 more rays)           // (navigation from just off the model — a corner, a silhouette: the
    std::optional<RayHit> best;                                 //  surface nearest round the cursor, so a pan keeps that place under it)
    for (double r : {3.0, 6.0, 10.0, 16.0, 24.0}) {
      for (int k = 0; k < 12; k++) { double a = k * M_PI / 6; if (auto h = pickSurface(m + QPointF(std::cos(a) * r, std::sin(a) * r)); h && (!best || h->t < best->t)) best = h; }
      if (best) break;
    }
    if (best) cam_.pivot = best->point;
  }
  if (tool_ == "brush" && e->button() == Qt::LeftButton && ctrl && alt) { startInflate(m, -1); return; }   // (Ctrl+Alt: shrink — before the navigation)
  if ((alt || tset.gzFree) && !ctrl && e->button() == Qt::LeftButton && room() == "topo" && (tool_ == "move" || tool_ == "rotate") && gizmoPressCustom(m)) return;   // (Alt + a handle — or the free gizmo: the gizmo itself)
  if (tool_ == "guide" && alt && shellPress(e)) return;     // (the mask brushes' Ctrl+Alt — before the navigation)
  if (tool_ == "modifier" && modPress(e)) return;           // (the weight brush — its Ctrl+Alt too)
  if (ribPress(e)) return;                                   // (a flat lock's Extrude / Offset / Glue brush)
  if (e->button() == Qt::LeftButton && ctrl && alt && rpDragPress(m)) return;   // (Ctrl+Alt+drag: the active reference plane's picture)
  if (navK) {
    drag_ = navK == 1 ? Drag::Orbit : navK == 2 ? Drag::Pan : Drag::Dolly; cam_.trackball = ui::Keymap::get().nav().rotation == "trackball";
    return;
  }
  if (room() == "topo") {                                    // (the Topology room: its own Cursor — none of the hair tools)
    if (topoToolRmb(e)) return;                              // (a topology tool's own RMB menus)
    if (ui::Keymap::get().pieMouse("main", mb, mm) && tool_ != "treveal") { emit pieRequested(toGlobal(m)); return; }   // (the Reveal Brush: no pies — its RMB is its size)
    if (ui::Keymap::get().pieMouse("ops", mb, mm) && tool_ != "tbrush" && tool_ != "tmask" && tool_ != "treveal") { emit opsPieRequested(toGlobal(m)); return; }   // (Ctrl+RMB: the edits)
    topoPress(e); return;
  }
  if (tool_ == "lockex" && !ui::Keymap::get().pieMouse("main", mb, mm) && lockExPress(e)) return;   // (Lock Extrude: its picks and drags)
  if (ui::Keymap::get().pieMouse("main", mb, mm)) { emit pieRequested(toGlobal(m)); return; }   // (Shift+RMB: the pie — or as set in Hotkeys)
  if (tool_ == "edge") {                                     // (Edges: Ctrl — a click deselects an edge, a drag — every edge in the box)
    if (e->button() != Qt::LeftButton) return;
    if (ctrl && !shift) { drag_ = Drag::EdgeBox; lasso_ = {{m.x(), m.y()}}; return; }
    edgeClick(m, ctrl, shift); return;
  }
  // B + LMB: on an anchor — it picked (Shift: added) and its smoothness: a click toggles sharp ↔ smooth, a drag sideways
  // sets it; off the anchors — the selected anchors' smoothness
  const bool bDown = keyB || bHeld();   // (B held though its key press went elsewhere — a text field of a panel holding the focus)
  if (bDown && e->button() == Qt::LeftButton) if (const int a = pickAnchor(m); a >= 0 && std::find(sel_.anchors.begin(), sel_.anchors.end(), a) == sel_.anchors.end()) {
    if (!shift) sel_.anchors.clear(); sel_.anchors.push_back(a); onSelChange(); }
  if (bDown && e->button() == Qt::LeftButton && !sel_.anchors.empty()) {
    smoothStart_.clear(); for (int a : sel_.anchors) smoothStart_.push_back(scene_->anchor(a)->smooth); drag_ = Drag::Smooth; return;
  }
  if (ribDrawOn() && e->button() == Qt::RightButton && ctrl) { bd_ = {}; bd_.start = m; bd_.r0 = ribR; drag_ = Drag::BrushAdjust; return; }   // (a flat lock: Ctrl+RMB — its width)
  if (tool_ == "draw") { if (e->button() == Qt::LeftButton) addDrawPoint(m); return; }   // (the Curve tool: a click — a point)
  if (tool_ == "guide" && shellPress(e)) return;             // (Shell modifiers: the mask brushes, the guide strokes)
  if (tool_ == "cut" && cutMode == "surface" && (e->button() == Qt::LeftButton || e->button() == Qt::RightButton)) { surfCutDown(e); return; }
  if (tool_ == "cut" && cutPress(e)) return;                 // (the scissors)
  if ((tool_ == "brush" || shellBrushActive() || weightBrushOn() || ribBrushOn()) && e->button() == Qt::RightButton && ctrl) {   // (Ctrl+RMB drag: radius ←→, hardness ↑↓)
    bd_ = {}; bd_.start = m; bd_.r0 = brushR; bd_.h0 = brushHard; drag_ = Drag::BrushAdjust; return;
  }
  if (tool_ == "brush" && e->button() == Qt::LeftButton) { if (ctrl) startInflate(m, 1); else startBrush(m, shift); return; }
  if (e->button() == Qt::LeftButton && (tool_ == "move" || tool_ == "rotate" || tool_ == "gizmo") && gizmoPress(m)) return;
  if (e->button() == Qt::LeftButton && refPress(m)) return;   // (the reference cube: its top face — the height, its body — moved)
  if (e->button() == Qt::RightButton) {
    if (pickWeldSpot(m)) return;
    if (auto ch = pickCurve(m, true, false)) { insertAt(*ch, m); return; }
    drag_ = Drag::AnchorLine; return;                        // (an RMB stroke over empty space)
  }
  if (e->button() != Qt::LeftButton) return;
  if (tool_ == "cursor" && room() == "hair" && ctrl && shift) {   // (Ctrl+Shift + click: the group of what is under the pointer)
    if (const int g = groupUnder(m); g >= 0) { selectItem(g, false); hoverGroup_ = -1; overlayRev_++; allWalls_ = true; ui::hud({}); emit statusText(tr("Group selected: %1").arg(scene_->item(g)->name)); return; }
  }
  int a = pickAnchor(m);
  if (crownPress(e, a)) return;                              // (Ctrl + drag along a connector, the crown tool's click)
  // Ctrl + click on a welded anchor of a selected curve: that curve comes apart from the others there
  if (a >= 0 && ctrl) {
    std::vector<int> made; auto un = edit::unweldAnchor(*scene_, a, sel_.items, &made);
    if (!un.empty()) {
      std::vector<int> mark{a}; mark.insert(mark.end(), made.begin(), made.end()); markCurves(curvesTouching(*scene_, mark));
      afterEdit(true); emit sceneEdited();
      emit statusText(un.size() == 1 ? tr("Anchor unwelded from %1").arg(scene_->item(un[0])->name) : tr("Anchor unwelded from %1 curves").arg(un.size()));
      return;
    }
  }
  if (a >= 0) {
    auto& A = sel_.anchors; auto it = std::find(A.begin(), A.end(), a);
    if (ctrl) { if (it != A.end()) A.erase(it); else A.push_back(a); }
    else if (shift || it != A.end()) { if (it != A.end()) A.erase(it); A.push_back(a); }
    else A = {a};
    sel_.items.clear(); onSelChange();
    if ((tool_ == "cursor" || tool_ == "crown") && sel_.hasAnchor(a)) startAnchorDrag(sel_.anchors, m, scene_->anchor(a)->pos);
    return;
  }
  if (ctrl) { sel_.anchors.clear(); ps_ = {}; drag_ = Drag::PaintSel; paintSel(m); return; }
  if (auto ch = pickCurve(m, false, false)) {
    const Item* c = scene_->item(ch->id);
    if (c->ring >= 0) { selectItem(c->ring, shift); return; }
    std::vector<int> keep; for (int id : sel_.items) if (const Item* x = scene_->item(id); x && x->type == ItemType::Curve) keep.push_back(id);
    sel_.items = keep; selectItem(ch->id, true); return;
  }
  if (hit) if (const Item* hi = scene_->item(hit->itemId); !(hi && hi->type == ItemType::IMesh && hi->raw.value("tref").toBool())) { selectItem(hit->itemId, shift); return; }   // (a reference: not picked in the viewport — only in the outliner, as in the Topology room)
  drag_ = Drag::Lasso; lassoAdd_ = shift; lasso_ = {{m.x(), m.y()}};
}

void Viewport::mouseMoveEvent(QMouseEvent* ev) {
  if (drag_ == Drag::None && !ev->buttons()) { choosePane(ev->position()); syncCursor(); }
  QMouseEvent le = localEvent(ev, paneOrigin()); QMouseEvent* e = &le;
  QPointF m = e->position(), d = m - last_;
  double dpr = devicePixelRatio();
  if (drag_ == Drag::Uv || (drag_ == Drag::None && cam_.view == "uv")) { uvMove(e); last_ = m; return; }
  if (drag_ == Drag::None && tool_ == "draw" && drawing_ >= 0 && !ui::Pie::current()) {   // (the ghost line to the next point)
    ghost_ = placePoint(m); overlayRev_++; requestUpdate();
  }
  if (drag_ == Drag::None && gz_.visible()) {               // (the handle under the pointer lit)
    auto h = gz_.pick(cam_, paneW(), paneH(), m.x(), m.y());
    if (h.axis != gz_.hover.axis) { gz_.hover = h; requestUpdate(); }
  }
  if (drag_ == Drag::Gizmo) { gizmoMove(m, e->modifiers()); last_ = m; return; }
  mouse_ = m;
  if (room() == "hair" && drag_ == Drag::None) groupHover(m, e->modifiers());   // (Ctrl+Shift: the group under the pointer lit)
  if (room() == "topo" && drag_ != Drag::BrushAdjust && topoMove(m, e->modifiers())) { last_ = m; return; }
  if (room() == "topo" && (tool_ == "tmask" || tool_ == "tbrush")) { overlayRev_++; requestUpdate(); }   // (the brush's circle follows the pointer)
  if (drag_ == Drag::Brush || drag_ == Drag::BrushSmooth || drag_ == Drag::Inflate || drag_ == Drag::BrushAdjust) { brushMove(m); last_ = m; return; }
  if (drag_ == Drag::Mask || drag_ == Drag::GuideStroke) { shellMove(m); last_ = m; return; }
  if (tool_ == "cut" && cutMode == "surface" && drag_ == Drag::None) { surfCutMove(m); last_ = m; return; }
  if (mergePick && drag_ == Drag::None) { overlayRev_++; requestUpdate(); ui::hud(tr("Weld distance: %1 (circle diameter) — click to set · Ctrl + wheel / Ctrl+RMB drag — size · Esc — cancel").arg(QString::number(pickWorld(m), 'g', 3))); }
  if (tool_ == "edge" && drag_ == Drag::None && !ui::Pie::current()) {   // (the edge under the pointer lit)
    auto h = pickEdge(m, true);
    if (bool(h) != bool(hoverEdge_) || (h && (h->mesh != hoverEdge_->mesh || h->e != hoverEdge_->e))) { hoverEdge_ = h; overlayRev_++; requestUpdate(); }
  }
  if (drag_ == Drag::CutStroke || (tool_ == "cut" && drag_ == Drag::None)) { cutMove(m); if (drag_ == Drag::CutStroke) { last_ = m; return; } }
  if (tool_ == "brush" || shellBrushActive() || weightBrushOn() || ribBrushOn()) { overlayRev_++; requestUpdate(); }   // (the circle follows the pointer)
  switch (drag_) {
    case Drag::Orbit: {
      // Shift: the camera snaps to the nearest standard view; the orbit goes on invisibly underneath, so moving further
      // jumps to the next view
      bool fixed = cam_.view != "persp" && cam_.view != "ortho";
      if ((e->modifiers() & Qt::ShiftModifier) && !fixed) {
        if (!snapOrbit_.on) snapOrbit_ = {true, cam_.pos, cam_.quat};
        cam_.pos = snapOrbit_.pos; cam_.quat = snapOrbit_.quat; cam_.up = {0, 1, 0};
        cam_.orbit(d.x(), d.y()); snapOrbit_.pos = cam_.pos; snapOrbit_.quat = cam_.quat;
        snapToNearestView();
      } else { if (snapOrbit_.on) { snapOrbit_.on = false; ui::hud({}); } cam_.orbit(d.x(), d.y()); }
      break;
    }
    case Drag::Pan: cam_.pan(d.x() * dpr, d.y() * dpr); break;
    case Drag::Dolly: cam_.dolly(d.x() - d.y()); break;
    case Drag::Anchor: {
      bool shift = e->modifiers() & Qt::ShiftModifier;
      if (shift && ad_.list.size() == 1 && slideToCurve(m)) { ad_.moved = true; scene_->revision++; markCurves(curvesTouching(*scene_, ad_.list)); break; }
      if (ad_.snap >= 0) {                                   // Shift released: free again from here, no jump
        ad_.snap = -1; overlayRev_++; emit statusText(QString());
        ad_.starts.clear(); for (int id : ad_.list) ad_.starts.push_back(scene_->anchor(id)->pos);
        hsym_ = hairSymTwins(ad_.list);
        ad_.ref = scene_->anchor(ad_.list[0])->pos; ad_.startHit = planeHit(ray(m), ad_.ref, ad_.n);
      }
      auto h = planeHit(ray(m), ad_.ref, ad_.n); if (!h || !ad_.startHit) break;
      Vec3 off = *h - *ad_.startHit;
      for (size_t i = 0; i < ad_.list.size(); i++) scene_->anchor(ad_.list[i])->pos = ad_.starts[i] + off;
      if (!hsym_.start.empty()) hairSymApply(hsym_);   // (Symmetry: the twins mirrored)
      if (hairSnap && room() == "hair") snapAnchors(ad_.list);   // (Snap: onto the reference meshes)
      ad_.moved = true; scene_->revision++;
      { auto ids = ad_.list; const auto tw = hsym_.twinIds(); ids.insert(ids.end(), tw.begin(), tw.end()); markCurves(curvesTouching(*scene_, ids)); }
      break;
    }
    case Drag::Lasso: {
      auto [lx, ly] = lasso_.back();
      if (std::hypot(m.x() - lx, m.y() - ly) >= 3) { lasso_.push_back({m.x(), m.y()}); overlayRev_++; }
      break;
    }
    case Drag::AnchorLine: overlayRev_++; break;
    case Drag::Slide: slideMove(m); break;
    case Drag::LockEx: lockExMove(m); break;
    case Drag::RefPlane: rpDragMove(m); break;
    case Drag::RefTop: case Drag::RefMove: refMove(m); break;
    case Drag::EdgeBox: lasso_ = {{start_.x(), start_.y()}, {m.x(), start_.y()}, {m.x(), m.y()}, {start_.x(), m.y()}}; overlayRev_++; break;
    case Drag::PaintSel: paintSel(m); break;
    case Drag::Smooth: {
      double v = (m.x() - start_.x()) / 200;
      for (size_t i = 0; i < sel_.anchors.size() && i < smoothStart_.size(); i++) scene_->anchor(sel_.anchors[i])->smooth = clamp(smoothStart_[i] + v, 0, 1);
      markCurves(curvesTouching(*scene_, sel_.anchors));
      ui::hud(tr("Smoothness: %1%").arg(int(std::lround(scene_->anchor(sel_.anchors.back())->smooth * 100))));
      break;
    }
    default: return;
  }
  last_ = m;
  requestUpdate();
}

void Viewport::mouseReleaseEvent(QMouseEvent* ev) {
  QMouseEvent le = localEvent(ev, paneOrigin()); QMouseEvent* e = &le;
  QPointF m = e->position(); Drag d = drag_;
  if (d == Drag::Uv || (d == Drag::None && cam_.view == "uv")) { uvRelease(e); return; }
  if (d == Drag::Orbit && snapOrbit_.on) { snapOrbit_.on = false; ui::hud({}); }
  if (d == Drag::Gizmo) { gizmoRelease(m); requestUpdate(); return; }
  if (d == Drag::Topo) { topoRelease(m); requestUpdate(); return; }
  if (d == Drag::Mask || d == Drag::GuideStroke) { shellRelease(m); requestUpdate(); return; }
  if (d == Drag::CutStroke) { cutRelease(e); requestUpdate(); return; }
  if (scutDrag_ >= 0) { scutDrag_ = -1; requestUpdate(); return; }
  drag_ = Drag::None;
  if (d == Drag::Brush || d == Drag::BrushSmooth || d == Drag::Inflate || d == Drag::BrushAdjust) {
    ui::hud({});
    if (bd_.moved) {
      beforeFrame(); HairChanges ch = hair_.finishDraft(*scene_);
      dirtyWalls_.insert(ch.walls.begin(), ch.walls.end()); if (!ch.cocoons.empty()) ringsDirty_ = true;
      commit(false);
    }
    bd_ = {}; overlayRev_++; requestUpdate(); return;
  }
  if (d == Drag::Anchor) {
    if (ad_.snap >= 0) emit statusText(QString());
    if (ad_.moved) {
      beforeFrame();                                          // (the last move, then the draft walls built right)
      HairChanges ch = hair_.finishDraft(*scene_);
      dirtyWalls_.insert(ch.walls.begin(), ch.walls.end()); if (!ch.cocoons.empty()) ringsDirty_ = true;
      overlayRev_++;
    }
    std::vector<int> list = ad_.list; bool moved = ad_.moved;
    if (moved || ad_.inserted) commit(false);
    ad_ = {};
    if (moved && list.size() == 1) checkWeld(list[0], m);
  } else if (d == Drag::Lasso) {
    lasso_.push_back({m.x(), m.y()});
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (auto [x, y] : lasso_) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    bool big = x1 - x0 > 3 || y1 - y0 > 3;
    std::vector<int> inside;
    if (big && lasso_.size() >= 3) {
      Projector P = projector();
      for (auto& a : anchors_) { ScreenPt s = P.toScreen(scene_->anchor(a.id)->pos); if (s.z < 1 && insidePolygon(lasso_, s.x, s.y)) inside.push_back(a.id); }
    }
    if (!inside.empty()) {
      if (lassoAdd_) { for (int a : inside) if (!sel_.hasAnchor(a)) sel_.anchors.push_back(a); } else sel_.anchors = inside;
      sel_.items.clear();
    } else if (!lassoAdd_) {                                  // (a click on empty space: the selected curves stay)
      sel_.anchors.clear();
      std::vector<int> keep; for (int id : sel_.items) if (const Item* x = scene_->item(id); x && x->type == ItemType::Curve) keep.push_back(id);
      sel_.items = keep;
    }
    lasso_.clear(); onSelChange();
  } else if (d == Drag::PaintSel) {
    if (!ps_.touched && std::hypot(m.x() - start_.x(), m.y() - start_.y()) > 6 && !sel_.empty()) {
      sel_ = {}; onSelChange(); emit statusText(tr("Selection cleared (Ctrl + a stroke over empty space)"));
    }
  } else if (d == Drag::EdgeBox) {
    lasso_.clear(); overlayRev_++;
    if (std::hypot(m.x() - start_.x(), m.y() - start_.y()) <= 4) edgeClick(m, true, false);
    else edgeBoxDeselect(std::min(m.x(), start_.x()), std::min(m.y(), start_.y()), std::max(m.x(), start_.x()), std::max(m.y(), start_.y()));
  } else if (d == Drag::RefPlane) {
    ui::hud({}); if (rpd_.moved) commit(false); rpd_ = {};
  } else if (d == Drag::RefTop || d == Drag::RefMove) {
    ui::hud({});
  } else if (d == Drag::Slide) {
    if (sl_.moved) { beforeFrame(); commit(false); }
    sl_ = {};
  } else if (d == Drag::CrownPlace) {
    if (std::hypot(m.x() - start_.x(), m.y() - start_.y()) <= 5) createCrownAt(m);
  } else if (d == Drag::LockEx) {
    lockExRelease();
  } else if (d == Drag::Smooth) {
    if (std::hypot(m.x() - start_.x(), m.y() - start_.y()) < 4) {   // (a click: sharp ↔ smooth — all sharp after it if any was smooth)
      bool anySmooth = false; for (int a : sel_.anchors) if (scene_->anchor(a)->smooth > 0.5) anySmooth = true;
      for (int a : sel_.anchors) scene_->anchor(a)->smooth = anySmooth ? 0 : 1;
      markCurves(curvesTouching(*scene_, sel_.anchors)); ui::toast(anySmooth ? tr("Sharp anchor") : tr("Smooth anchor")); }
    ui::hud({}); beforeFrame(); commit(false); emit changed();
  } else if (d == Drag::AnchorLine) {
    overlayRev_++;
    if (std::hypot(m.x() - start_.x(), m.y() - start_.y()) > 5) anchorLine(start_, m);
  }
  requestUpdate();
}

void Viewport::wheelEvent(QWheelEvent* ev) {
  if (drag_ == Drag::None) choosePane(ev->position());
  QWheelEvent le(ev->position() - paneOrigin(), ev->globalPosition(), ev->pixelDelta(), ev->angleDelta(), ev->buttons(), ev->modifiers(), ev->phase(), ev->inverted()); QWheelEvent* e = &le;
  if (cam_.view == "uv") { uvWheel(e); return; }
  int steps = e->angleDelta().y() > 0 ? 1 : e->angleDelta().y() < 0 ? -1 : 0;
  if (!steps) return;
  // (the camera: by how far the wheel turned — 120 a notch; a smooth / high-resolution wheel sends parts of one)
  const double notches = e->angleDelta().y() / 120.0;
  if (mergePick && (e->modifiers() & Qt::ControlModifier)) { pickR = clamp(pickR * (steps > 0 ? 1.1 : 0.9), 2, 400); setMergePick(true); return; }   // (◎ Pick: Ctrl+wheel — its size)
  if (ribDrawOn() && (e->modifiers() & Qt::ControlModifier)) { ribR = clamp(ribR * (steps > 0 ? 1.1 : 0.9), 3, 400); overlayRev_++; requestUpdate(); panelsLater(); return; }   // (a flat lock: Ctrl+wheel — its width)
  if ((tool_ == "brush" || tool_ == "tmask" || tool_ == "tbrush" || tool_ == "treveal" || nBrushOn() || tool_ == "tlobe" || shellBrushActive() || weightBrushOn() || ribBrushOn()) && (e->modifiers() & Qt::ControlModifier)) { brushResize(steps > 0 ? 1.1 : 0.9); panelsLater(); return; }
  if (isModelTool() && topoToolWheel(steps, e->modifiers())) return;   // (Loop: the wheel — how many cuts)
  double dpr = devicePixelRatio();
  std::optional<double> hitDist;
  // (toward what is under the cursor; a slow look — a heavy mesh without its hierarchy yet: the steps go on at the rate
  // they had for a moment, not a ray per notch)
  if (!cam_.ortho() && QDateTime::currentMSecsSinceEpoch() >= wheelSlowUntil_) {
    QElapsedTimer pt; pt.start(); if (auto h = pickSurface(e->position())) hitDist = h->t;
    if (pt.elapsed() > 12) wheelSlowUntil_ = QDateTime::currentMSecsSinceEpoch() + 400;
  }
  cam_.wheel(e->position().x() * dpr, e->position().y() * dpr, notches, hitDist);
  wheelAt_ = QDateTime::currentMSecsSinceEpoch(); QTimer::singleShot(300, this, [this] { requestUpdate(); });   // (the LOD while the wheel turns: the full mesh again after)
  requestUpdate();
}

// (the hotkeys are the main window's — Keymap; here only what has no action: Esc, Enter)
void Viewport::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape && escape()) return;
  if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && drawing_ >= 0) { finishDrawing(); return; }
  QVulkanWindow::keyPressEvent(e);
}

void Viewport::resizeEvent(QResizeEvent* e) { QVulkanWindow::resizeEvent(e); requestUpdate(); }

bool Viewport::event(QEvent* e) {
  if (e->type() == QEvent::Enter || e->type() == QEvent::Leave) { inViewport_ = e->type() == QEvent::Enter; overlayRev_++; requestUpdate(); }
  if (e->type() == QEvent::DragEnter) {
    auto* d = static_cast<QDragEnterEvent*>(e);
    if (d->mimeData() && d->mimeData()->hasUrls() && onFileDrag) { d->acceptProposedAction(); onFileDrag(); return true; }
  }
  return QVulkanWindow::event(e);
}

}  // namespace hm

namespace hm {
void prof(const char* what, qint64 ms) {
  static const bool on = qEnvironmentVariableIsSet("HM_PROF"); if (!on) return;
  static QFile f(QDir::temp().filePath("hm_prof.txt")); if (!f.isOpen()) f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
  f.write(QByteArray(what) + " " + QByteArray::number(ms) + "\n"); f.flush();
}
ProfScope::ProfScope(const char* what) : w(what), t0(QDateTime::currentMSecsSinceEpoch()) {}
ProfScope::~ProfScope() { const qint64 d = QDateTime::currentMSecsSinceEpoch() - t0; if (d >= 2) prof(w, d); }
}  // namespace hm
