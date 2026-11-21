// The outer shell of a high-poly reference with a double wall (a sculpted garment with a thickness, a shell extracted from a
// body, a lining): which polygons are its outside. The snap's nearest-point queries look only at those.
#pragma once
#include "scene/Topo.h"
#include <atomic>
#include <string>
#include <vector>
namespace hm::shell {
struct Report { std::string text; int buried = 0, lining = 0; };
// bvh: the mesh's triangles (world or file space), tris / t2f: the same triangles in order and their polygons; → per polygon 1 — outer;
// cancel (when set to 2 mid-way) stops it
std::vector<char> outerFaces(const topo::Bvh& bvh, const std::vector<uint32_t>& tris, const std::vector<int>& t2f, int nf, const std::atomic<int>* cancel = nullptr, Report* report = nullptr);
}
