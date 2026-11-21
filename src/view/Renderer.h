// Vulkan rendering of the viewport. Buffers are host-visible; a replaced buffer is destroyed only after every frame in
// flight that may still read it has finished (deferred deletion). What changed is rebuilt per frame, piece by piece:
// the meshes (on a load / visibility change), single walls (an edit), the cocoons' rings and shells, the overlays
// (curves, anchors, the lasso).
#pragma once
#include "scene/Looks.h"
#include "view/SceneGeometry.h"
#include <array>
#include <QVulkanWindow>
#include <QImage>
#include <map>
#include <set>
#include <vector>

namespace hm {

class Viewport;
struct WallMesh;

class Renderer : public QVulkanWindowRenderer {
public:
  explicit Renderer(Viewport* w);
  void initResources() override;
  void initSwapChainResources() override;
  void releaseSwapChainResources() override;
  void releaseResources() override;
  void startNextFrame() override;

private:
  struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize size = 0; };
  struct GpuMesh { int itemId = 0; Buf vb, ib, wb; uint32_t triCount = 0, wireCount = 0; float color[4]; bool transparent = false, hl = false, depthWrite = false, textured = false, paint = false, cached = false, wireOn = true, unlit = false; float mat = 0; };   // (mat: the material's code, mesh.frag)   // (textured: drawn by its textured copy — but as the Topology room's two-tone active mesh)
  // 👁 Display (settings.view): what is shown, the opacities, X-ray
  struct ViewOpts { double xray = 0.2, surfOp = 0.85, modOp = 0.45, anchorOp = 1, curveOp = 1, guideOp = 1, gridOp = 1; bool showGrid = false, showLoops = false, showMods = true, showMeshWire = false; };
  ViewOpts viewOpts() const;
  struct GpuLines { Buf vb; uint32_t count = 0; float width = 1, bias = 0; bool noPdo = false; float xray = 0; };   // (xray: drawn over everything too, this faint)
  // textures (an imported mesh's look): an image with its mip levels; a part — the triangles of one texture set
  struct GpuTex { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
  struct TexPart { uint32_t first = 0, count = 0; VkDescriptorSet ds = VK_NULL_HANDLE; float color[4]; int flags = 0; bool transparent = false; int other = -1; };   // (other: the Paint room — a UV set not shown now, its number in paintOtherNames_: drawn with that set's picture of this frame)
  struct GpuTexMesh { int itemId = 0; Buf vb, ib; std::vector<TexPart> parts; bool flat = false, cached = false, unlit = false; float mat = 0; };
  static float matCode(const Item& it, bool topoRoom);   // the Topology room's material settings packed for the shaders (0 — none)
  // an imported mesh's GPU copies kept while its geometry (the same PolyMesh) and how it is drawn stay: a mode switch, a
  // selection, a look of another mesh don't rebuild a heavy model
  // (an editable mesh keeps its CPU copy too: a drag patches the vertices it moved — Viewport::LiveMove)
  struct Adj { std::vector<uint32_t> vOff, vCorner; std::vector<int> faceOf; };   // vertex → its corners; corner → its polygon
  struct PlainC { std::weak_ptr<PolyMesh> poly; QByteArray key; GpuMesh g; bool used = false;
                  const PolyMesh* raw = nullptr; bool keep = false, flat = true; std::vector<MeshVertex> verts; std::vector<uint32_t> wire; std::vector<LineVertex> lv;
                  Adj adj; std::vector<uint32_t> wOff, wSlot; };
  struct TexC { std::weak_ptr<PolyMesh> poly; QByteArray key; GpuTexMesh g; std::set<QString> tex, sets; bool used = false;
                const PolyMesh* raw = nullptr; bool keep = false, flat = true; float uvScale = 1; std::vector<TexVertex> verts; Adj adj; };
  std::vector<TexVertex>* keepTexVerts_ = nullptr;
  struct VertPts { int m = -1; const PolyMesh* raw = nullptr; int selVer = -1; size_t n = 0; QByteArray hid; std::vector<PointVertex> pts; Buf buf; } vpts_;   // (TopoDraw::pointsOf)
  void syncVertPts();      // (syncTextured / syncChecker: their vertices kept here when set)
  bool patchLive(int m, const std::shared_ptr<PolyMesh>& poly, const PolyMesh* prev, const std::vector<int>& verts, const std::vector<int>& corners);
  void patchBuffer(const Buf& dst, const void* data, size_t stride, std::vector<uint32_t> idx);
  static void adjOf(Adj& a, const PolyMesh& P);
  std::map<int, PlainC> plainCache_; std::map<int, TexC> texMeshCache_;
  void dropMeshCaches();

  Buf makeBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
  void retire(Buf& b);
  void retire(GpuMesh& m) { retire(m.vb); retire(m.ib); retire(m.wb); }
  void collectGarbage(bool all);
  VkShaderModule loadShader(const QString& name);
  VkPipeline makePipeline(VkPipelineLayout layout, const QString& vs, const QString& fs, VkPrimitiveTopology topo,
                          const VkVertexInputBindingDescription& bind, const VkVertexInputAttributeDescription* attrs, int nAttrs,
                          bool depthTest, bool depthWrite, bool blend, bool depthBias, bool lineWidthDynamic, bool colorWrite = true);
  GpuMesh uploadMesh(int itemId, const MeshDraw& d, const float color[4], const std::vector<LineVertex>* wire);
  void uploadLines(GpuLines& g, const LineBatch& b);
  void syncStatic();
  void syncWall(int id);
  void syncRings();
  void syncOverlays();
  void syncTubes();
  void syncCrowns();
  void syncRibs();
  void syncTopo();                                       // (the Topology room's overlays over the active mesh)
  GpuTex uploadImage(const QImage& img, bool srgb);
  void updateRegion(GpuTex& t, const QImage& src, std::vector<QRect> rs, bool srgb);   // (a part of a texture made by uploadImage sent again, its mip levels there made anew)
  qint64 paintRevDone_ = -1; int paintGen_ = 0; QSize paintTexSize_;
  std::map<QString, qint64> paintOtherRevDone_; std::map<QString, QSize> paintOtherSize_;   // (the other sets' textures kept: a change sends its tiles only)
  void destroyTex(GpuTex& t);
  QString textureFor(const looks::MapSrc& s, bool srgb);   // its key in texCache_ ("" — none / unreadable)
  VkDescriptorSet descriptorFor(const QString& mapKey, const QString& alphaKey);
  void syncTextured(const Item& it, std::set<QString>& usedTex, std::set<QString>& usedSets, const PolyMesh* P = nullptr, std::vector<GpuTexMesh>* out = nullptr);
  // heavy meshes' LOD (Viewport::lodOf, the file's space — placed by the mesh's transform here): drawn instead while the
  // camera moves; the lists the frame draws then
  struct LodGpu { std::shared_ptr<const PolyMesh> lod; QByteArray key; GpuMesh plain; std::vector<GpuTexMesh> tex; std::set<QString> usedTex, usedSets; bool used = false; };
  std::map<int, LodGpu> lodGpu_;
  std::vector<GpuMesh> staticNav_; std::vector<GpuTexMesh> texNav_;
  void syncLods(std::set<QString>& usedTex, std::set<QString>& usedSets);
  // the UV mode: the checker on a mesh by its UV; the UV Editor's window (its grid, the UV's lines, polygons, points, the
  // texture under them — in UV units, drawn with the window's own matrix)
  void syncChecker(const Item& it, std::set<QString>& usedTex, std::set<QString>& usedSets);
  QByteArray paintOthersKey() const;
  void syncPaintView(const Item& it, std::set<QString>& usedTex, std::set<QString>& usedSets);   // (the Paint room: the map painted, on the polygons of its UV set)
  static constexpr int PAINT_VIEW = -0x7ffffff0;          // (sketchTexture's id of the Paint room's map)
  // (the Paint room's other UV sets: each one's picture uploaded when its revision changes — a stroke passing over it — and its
  // descriptor looked up at the draw; the meshes are not made anew for that)
  std::vector<QString> paintOtherNames_; std::map<QString, QString> paintOtherKey_; std::map<QString, VkDescriptorSet> paintOtherDs_;
  VkDescriptorSet paintDs_ = VK_NULL_HANDLE;             // (its texture this frame: the parts flagged 32 are drawn with it)
  QString checkerTex();
  void syncUv();
  void syncUvHover();
  std::vector<GpuLines> uvHovLines_; std::vector<GpuMesh> uvHovTris_; Buf uvHovPts_; uint32_t uvHovPtCount_ = 0; QByteArray uvHovSeen_;
  std::vector<GpuLines> uvGrid_, uvLines_; std::vector<GpuMesh> uvTris_; Buf uvPts_, uvQuad_; uint32_t uvPtCount_ = 0, uvQuadN_ = 6;
  VkDescriptorSet uvTexDs_ = VK_NULL_HANDLE; QByteArray uvSeen_, checkerSig_; float uvOp_ = 1; bool uvChk_ = false;   // (the UV Editor: what is under the UV — its opacity, the checker or a texture)

  Viewport* w_;
  QVulkanDeviceFunctions* df_ = nullptr;
  bool wideLines_ = false;
  float maxLineWidth_ = 1;
  VkPipelineLayout meshLayout_ = VK_NULL_HANDLE, lineLayout_ = VK_NULL_HANDLE, pointLayout_ = VK_NULL_HANDLE;
  VkPipeline meshBlendWritePipe_ = VK_NULL_HANDLE, pointDepthPipe_ = VK_NULL_HANDLE;   // (see-through walls writing depth; anchors depth-tested)
  Buf pins_; uint32_t pinCount_ = 0;
  VkPipeline meshPipe_ = VK_NULL_HANDLE, meshBlendPipe_ = VK_NULL_HANDLE, linePipe_ = VK_NULL_HANDLE, lineTopPipe_ = VK_NULL_HANDLE,
             pointPipe_ = VK_NULL_HANDLE, meshDepthPipe_ = VK_NULL_HANDLE;   // (meshDepthPipe_: the depth only — a see-through mesh's nearest surface first)
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout texSetLayout_ = VK_NULL_HANDLE; VkDescriptorPool texPool_ = VK_NULL_HANDLE; VkSampler sampler_ = VK_NULL_HANDLE;
  VkPipelineLayout texLayout_ = VK_NULL_HANDLE; VkPipeline texPipe_ = VK_NULL_HANDLE, texBlendPipe_ = VK_NULL_HANDLE;
  VkPipeline rpBehindPipe_ = VK_NULL_HANDLE, rpOverPipe_ = VK_NULL_HANDLE;   // (reference planes: behind the model / over it)
  Buf rpVb_;
  looks::MapSrc refImage(const RefPlane& rp) const;
  QString sketchTexture(int planeId);                    // (a plane's ✏ drawing: uploaded again when its rev changes, the old one freed)
  void dropTex(const QString& key);                      // (a texture and its set freed after the frames in flight)
  std::map<int, QString> sketchKey_;                     // (plane id → its drawing's texture key, "sk:<id>:<rev>#c")
  Buf markVb_; uint32_t markCount_ = 0; std::map<int, GpuLines> paneMarks_;   // (the reference markers: rings in every pane; guides per pane)
  std::map<QString, GpuTex> texCache_; std::map<QString, VkDescriptorSet> texSets_; GpuTex white_;
  std::map<QString, qint64> texSeen_;                    // (a map read from a file: when a mesh last asked for it — kept three minutes past that, a room left and come back to reads nothing anew)
  std::vector<GpuTexMesh> texMeshes_;
  struct DeadTex { GpuTex t; VkDescriptorSet ds; int frames; }; std::vector<DeadTex> deadTex_;
  int texRev_ = -1;

  std::vector<GpuMesh> static_, shells_, tubes_;
  std::set<int> wireDisp_;                                     // (the meshes the views' ⬡ / ▦ show as wire — never Head Collision's objects)
  GpuLines tubeGuides_, tubeLoops_, tubeWire_, edgeSel_, edgeHov_;   // (tubeWire_: the braids' mesh, over their surfaces)
  int tubesRev_ = -1, crownsRev_ = -1, ribsRev_ = -1;
  std::vector<GpuMesh> ribs_, ref_;
  GpuLines ribLines_, paneFrames_, paneActive_, zone_;
  std::vector<GpuMesh> crowns_;
  std::vector<GpuMesh> topoTris_; std::vector<float> topoTrisLift_; std::vector<GpuLines> topoLines_; Buf topoPts_; uint32_t topoPtCount_ = 0; Buf topoPtsX_; uint32_t topoPtXCount_ = 0; Buf topoPtsN_; uint32_t topoPtNCount_ = 0;   // (N: points with no PDO — the Topology Ghost)
  std::array<long long, 3> topoKey_{-1, -1, -1}; float topoOvPdo_ = 0;
  std::map<int, GpuMesh> walls_;
  GpuLines floorP_[4], axesP_[4];   // (each pane's own floor and axes)
  GpuLines curves_, floor_, axes_, rings_, ringsBack_, lasso_, joints_, gizmo3_, gizmo2_, guides_, masks_;
  std::map<int, GpuLines> paneGrid_;                      // (each orthographic pane's view grid: made every frame over what its camera sees — no edge)
  Buf points_; uint32_t pointCount_ = 0;
  int staticRev_ = -1, overlayRev_ = -1, crownStatic_ = -2;
  // the view window's picture: its windows drawn into an image of their own (a render pass like the viewport's — the same pipelines),
  // read back and handed to the window (Viewport::detachedFrame)
  struct Off { VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE, msaa = VK_NULL_HANDLE; VkDeviceMemory colorM = VK_NULL_HANDLE, depthM = VK_NULL_HANDLE, msaaM = VK_NULL_HANDLE;
               VkImageView colorV = VK_NULL_HANDLE, depthV = VK_NULL_HANDLE, msaaV = VK_NULL_HANDLE; VkFramebuffer fb = VK_NULL_HANDLE; Buf read; int w = 0, h = 0; } off_;
  VkRenderPass offPass_ = VK_NULL_HANDLE;
  bool ensureOff(int w, int h);
  void destroyOff();
  struct Dead { Buf b; int frames; };
  std::vector<Dead> dead_;
};

}  // namespace hm
