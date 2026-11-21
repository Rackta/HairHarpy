// Baking a high-poly onto a low-poly's UVs, on the CPU: every texel of the low mesh casts a ray from the cage (its surface pushed
// out along its normals) inward; where the ray meets the high mesh, the high mesh's normal (in the low mesh's tangent space),
// its height over the low surface, its position and its ambient occlusion are written. Curvature is derived from the normal map.
// Padding (dilation over the UV islands' borders) at the end. No GPU, nothing of the viewport — see tools/painttest.cpp.
#pragma once
#include <QImage>
#include <QString>
#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace hm { struct PolyMesh; }

namespace hm::paint {

struct V3 { float x = 0, y = 0, z = 0; };
struct V2 { float x = 0, y = 0; };

struct TriMesh {
  std::vector<V3> pos;
  std::vector<std::array<int, 3>> tri;
  std::vector<std::array<V3, 3>> nrm;      // per triangle corner (smooth, or the file's own)
  std::vector<std::array<V2, 3>> uv;       // per triangle corner (empty — none: a high-poly needs none)
  std::vector<V3> col;                     // per vertex: its colour 0…1 (ZBrush polypaint, vertex colours) — empty: none
  bool flipV = false;                      // the picture's rows run from v = 1 down (PolyMesh::flipY: as the renderer and every exported texture read it)
  bool empty() const { return tri.empty(); }
};

// polygons triangulated (a fan), normals: the file's own per corner, else the area-weighted ones of the vertices
// (hardEdges are honoured: the corners on a hard edge get the face's normal)
TriMesh triMeshFrom(const hm::PolyMesh& m);

struct BakeSettings {
  int size = 2048;
  int padding = 8;                 // px
  float cage = 0.02f;              // how far the rays start from the low surface, of the high mesh's bounding diagonal
  float back = 0.02f;              // how far behind it they still hit, the same measure
  int samples = 1;                 // 1, or 2 (a 2×2 grid per texel)
  int aoRays = 16;
  float aoDist = 0.08f;            // of the diagonal
  bool normal = true, ao = true, height = true, position = false, curvature = false;
  bool colour = false;             // the high mesh's vertex colours (polypaint) — an ID map: a mask by a colour picks a part
  bool flipGreen = false;          // DirectX's normal maps (Y down)
  // a partial bake: only the texels where this mask is not black are cast (Grayscale8, any size — read by UV). The user paints it
  // over the folds of the cloth; the rest of the model costs nothing and keeps its own normals. Null — the whole low-poly
  QImage mask;
};

struct BakeResult {
  QImage normal;       // RGB888, tangent space
  QImage ao;           // Grayscale8, 255 = open
  QImage height;       // Grayscale16, 32768 = the low surface, above — the high one rises over it
  QImage position;     // RGB888, the high mesh's position in its bounding box
  QImage curvature;    // Grayscale8, 128 = flat, brighter — convex, darker — concave
  QImage colour;       // RGB888: the high mesh's vertex colour where the ray met it (null — the high mesh has none)
  int texels = 0, hit = 0;
  QString log;
};

// progress: 0…1, return false to stop
BakeResult bake(const TriMesh& low, const TriMesh& high, const BakeSettings& s, const std::function<bool(float)>& progress = {});

// A shadow other objects cast onto a UV set (19.11, recording Cloth_2, 57:28: the shadow of the hair on the shoulders painted with the
// hair hidden, the hair shown, the extra wiped — three times in a minute; the author: "живая тень объекта", its own light each).
// P, N: w × h × 3 floats — the surface under every texel (N = 0: none); L: toward the light; softDeg: the light's half-angle (a soft
// edge widening with the distance to the caster, as a real one); rays per sample. -> Grayscale8, 255 — the light fully blocked.
// Worked out on a coarser grid on big maps (step 2 from 2048, 4 from 4096) and spread back over the surface, not across seams
// the surface of a UV set made small enough to hand to a worker thread: a sample per grid cell (its place and normal), for every
// texel which of the four grid samples round it lie next to it on the model (bits 0..3; 0xff - no surface), the texels with none
// beside them kept whole (a narrow strip casts its own rays). Depends on the geometry only - kept while the light changes
struct ShadowFrame {
  int w = 0, h = 0, step = 1, gw = 0, gh = 0; float tex = 0;
  std::vector<V3> gp, gn; std::vector<unsigned char> gok;   // (per grid cell; gok 0 - no surface in it)
  std::vector<unsigned char> near4;                          // (per texel)
  std::vector<int> lone; std::vector<V3> lp, ln;            // (texels with no grid sample beside them: index, place, normal)
};
std::shared_ptr<ShadowFrame> shadowFrame(const float* P, const float* N, int w, int h);
QImage castShadow(const ShadowFrame& F, const TriMesh& casters, const float L[3], float softDeg, int rays,
                  const std::function<bool(float)>& progress = {});

// a bump (height) map as a tangent-space normal map: strength scales the slope, a wrap at the borders
QImage heightToNormal(const QImage& height16, float strength, bool flipGreen = false);

// the normal map the Normal mode gives: the baked normals where the bake mask lets them through (white — the bake, black — flat; a
// null mask — the whole bake), with the painted bump's slope added over them. Any of the three may be null; all null — a null image.
// size: the result's (the inputs are read by UV, whatever their own size)
QImage finalNormal(const QImage& bakedNormal, const QImage& bakeMask, const QImage& height16, float strength, bool flipGreen, const QSize& size);

}  // namespace hm::paint
