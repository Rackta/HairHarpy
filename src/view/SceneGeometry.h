// CPU-side geometry of what the viewport draws, built from the scene (no Vulkan here): triangles + normals of meshes,
// curve polylines, anchor points, the floor grid and world axes. Colours are linear (the swapchain is sRGB).
#pragma once
#include "io/MeshImport.h"
#include "scene/Scene.h"
#include <QColor>
#include <cstdint>
#include <set>
#include <vector>

namespace hm {

struct MeshVertex { float p[3]; float n[3]; float c[3] = {1, 1, 1}; float g[3] = {0, 0, 0}; };   // c: a tint (linear) times the mesh colour; g: the polygon's own normal where n is a normal of the file's (the PDO goes by it), else zero
struct LineVertex { float p[3]; float c[4]; };
struct PointVertex { float p[3]; float c[4]; float shape; };   // c.a — size in px

struct MeshDraw {
  int itemId = 0;
  std::vector<MeshVertex> verts;
  std::vector<uint32_t> tris, wire;     // indices: triangles, polygon edges (lines)
  float color[4] = {0.5f, 0.5f, 0.5f, 1};
  bool showWire = false;
};

struct LineBatch { std::vector<LineVertex> verts; float width = 1; float bias = 0; bool depthTest = true; };

QColor linearColor(const QColor& srgb);
void toLinear(const QColor& srgb, float out[4], float alpha = 1);
// baseColor: the model colour, or half way to the colour of the topmost coloured group above it (mixed in linear, as THREE.Color)
void itemBaseColor(const Scene& sc, const Item& it, float out[4]);

MeshDraw buildMeshDraw(const PolyMesh& m, bool withWire, bool flat = false, const std::vector<std::array<float, 3>>* tint = nullptr);   // (tint: a linear colour per vertex — the polypaint — instead of m.col)
// (flat — per polygon, else smooth vertex normals)
// each corner's normal, smooth but broken at the hard edges (m.hardEdges): the polygons round a vertex averaged only
// within the fans the hard edges leave
std::vector<Vec3> hardCornerNormals(const PolyMesh& m);
// an imported mesh's textured copy: a vertex per polygon corner (its UV — in the image's orientation: the file's flipY
// applied), the triangles by texture set (sets: texSets — a polygon of another material goes to the first)
struct TexVertex { float p[3]; float n[3]; float uv[2]; };
struct TexDraw { std::vector<TexVertex> verts; std::vector<std::vector<uint32_t>> bySet; };
TexDraw buildTexDraw(const PolyMesh& m, const std::vector<TexSet>& sets, bool flat);
LineBatch buildFloor(double scale, const Vec3& center, double floorY, double opacity = 1);
// the snapping grid of a view (its own step): the floor for the perspective, orthographic and top views (at floorY), a
// wall through the origin for the side ones — front / back: X–Y, left / right: Z–Y
LineBatch buildViewGrid(const QString& view, double scale, const Vec3& center, double floorY, double step);
LineBatch buildAxes(double scale, double opacity = 0.6);

}  // namespace hm
