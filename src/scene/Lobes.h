// Lobes (the Topology room's Lobe Brush): a high-poly mesh's polygons painted into layers — a lobe of a braid each (the
// mesh's lobes: the layers' names, lobeF: each polygon's layer, 0 — none). The fill stops at the crevices between them
// (the seams, the surface's concavity); each lobe laid onto a square — the retopology the user makes on one lobe shown
// (and made) on the others through it.
#pragma once
#include "scene/Scene.h"
#include "scene/Topo.h"
#include <QString>
#include <array>
#include <memory>
#include <array>
#include <memory>
#include <vector>

namespace hm::lobes {

QString enc(const std::vector<int>& lab);                    // (each polygon's layer, packed)
std::vector<int> dec(const QString& s, int n);              // (empty / of another size — all 0)
// per polygon: how hollow the surface is there (> 0 — a crevice, < 0 — a bulge), smoothed, against the mean edge
std::vector<float> concavity(const topo::TTopo& T, const std::vector<Vec3>& P);
float quantile(std::vector<float> v, double q);
// the crevices: the polygons with an edge folded sharper than foldDeg (the seams where strands meet), or hollow at
// concThr and over
std::vector<char> crevices(const topo::TTopo& T, const std::vector<Vec3>& P, const std::vector<float>& conc, double foldDeg, float concThr);
// the fill from polygon f0: over the polygons free to take (label 0 or `layer`) while out of the crevices (widened by
// `close` rings — their gaps shut), then across the widening and on up a hollow's slope to its bottom; more than
// maxFaces out of them — leaked: returned as far as it went
std::vector<int> fill(const topo::TTopo& T, const std::vector<float>& conc, std::vector<char> cr, const std::vector<int>& lab, int f0, int layer, bool overwrite, int close, size_t maxFaces);

// a lobe laid onto a square: its two sides along it (along its long axis) at v = 0 / 1, its ends at
// u = 0 (the higher one — the braid's root) / 1; each inner vertex the mean of its neighbours (Tutte: nothing folds).
// The retopology of one lobe goes onto another through it
struct LobeMap {
  int id = 0; bool ok = false;
  std::vector<Vec3> P; std::vector<std::array<int, 3>> tri; std::vector<double> U, V;   // (its surface — world; each vertex on the square)
  std::shared_ptr<topo::Bvh> bvh; int GB = 40; std::vector<std::vector<int>> bins;      // (its triangles in 3D; on the square by cells)
};
std::vector<LobeMap> maps(const topo::TTopo& T, const std::vector<Vec3>& P, const std::vector<int>& lab);
bool toUV(const LobeMap& M, const Vec3& p, double& u, double& v, double* dist = nullptr);   // (a point near it → its place on the square)
Vec3 fromUV(const LobeMap& M, double u, double v);                                          // (a place on the square → the lobe)
// one lobe laid over another: mirrored (across its middle plane — the normal n through rc) or not, then turned, sized
// and moved — p → R·(p − c0)·s + c1; err: how far its points stay from the other's surface (root mean square)
struct Fit { bool mirror = false; Vec3 n, rc, c0, c1; double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; double s = 1, err = 1e300; Vec3 apply(const Vec3& p) const; };
std::vector<Fit> fits(const LobeMap& A, const LobeMap& B);   // (the poses found — every way round, the best first)

}  // namespace hm::lobes
