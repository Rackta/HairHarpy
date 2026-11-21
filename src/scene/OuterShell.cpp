#include "scene/OuterShell.h"
#include <algorithm>
#include <cmath>
#include <thread>
namespace hm::shell {
// Each polygon looks out over its front (its normal's side), 16 rays spread over the hemisphere:
//   buried — most rays that hit something leave through the BACK of another polygon at once: its front is inside the solid of another
//     layer (two skins laid over each other, the sculpted one and the base under it, each coming out on top here and there);
//   lining — hardly any ray gets away, and right behind it (within 1% of the model) lies a polygon's back that does see the outside:
//     the inner wall of a shell with a thickness, its hollow side.
// Everything else is outside — a deep fold or the inside of a mouth too: those have no better wall right behind them.
std::vector<char> outerFaces(const topo::Bvh& bvh, const std::vector<uint32_t>& tris, const std::vector<int>& t2f, int nf, const std::atomic<int>* cancel, Report* report) {
  std::vector<char> outer(size_t(nf), 1); const auto& P = bvh.P; if (P.empty() || t2f.empty()) return outer;
  std::vector<int> first(size_t(nf), -1); for (size_t t = t2f.size(); t-- > 0;) if (t2f[t] >= 0 && t2f[t] < nf) first[size_t(t2f[t])] = int(t);
  Box3 box; for (const Vec3& p : P) box.expand(p); const double diag = box.size().length(), eps = std::max(1e-9, diag * 2e-6), T = diag * 0.01;
  const int NR = 16; std::vector<float> esc(size_t(nf), 1), back(size_t(nf), 0); std::vector<int> behind(size_t(nf), -1);
  auto triN = [&](int t) { const Vec3 &A = P[tris[size_t(t) * 3]], &B = P[tris[size_t(t) * 3 + 1]], &C = P[tris[size_t(t) * 3 + 2]]; return (B - A).cross(C - A); };
  auto run = [&](auto fn) { std::atomic<int> next{0}; auto work = [&] { for (;;) { const int a = next.fetch_add(2048); if (a >= nf || (cancel && cancel->load() == 2)) return; for (int f = a; f < std::min(nf, a + 2048); f++) fn(f); } };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency()); std::vector<std::thread> th; for (unsigned i = 1; i < hw; i++) th.emplace_back(work); work(); for (auto& x : th) x.join(); };
  run([&](int f) { const int t = first[size_t(f)]; if (t < 0) return; Vec3 n = triN(t); const double L = n.length(); if (L < 1e-30) return; n = n * (1 / L);
    const Vec3 c = (P[tris[size_t(t) * 3]] + P[tris[size_t(t) * 3 + 1]] + P[tris[size_t(t) * 3 + 2]]) * (1.0 / 3), u = (std::abs(n.x) < 0.8 ? Vec3(1, 0, 0) : Vec3(0, 1, 0)).cross(n).normalized(), v = n.cross(u);
    int e = 0, b = 0;
    for (int k = 0; k < NR; k++) { const double z = 1 - (k + 0.5) / NR, r = std::sqrt(std::max(0.0, 1 - z * z)), az = k * 2.39996323; const Vec3 d = n * z + (u * std::cos(az) + v * std::sin(az)) * r;
      const auto h = bvh.ray(c + n * eps, d, 1e300); if (!h) { e++; continue; } if (triN(h->second).dot(d) > 0) b++; }
    esc[size_t(f)] = float(e) / NR; back[size_t(f)] = float(b) / NR;
    if (e <= NR / 8) if (const auto h = bvh.ray(c - n * eps, n * -1.0, T); h && triN(h->second).dot(n) < 0 && h->second >= 0 && size_t(h->second) < t2f.size()) behind[size_t(f)] = t2f[size_t(h->second)]; });
  int buried = 0, lining = 0;
  for (int f = 0; f < nf; f++) { if (back[size_t(f)] >= 0.5) { outer[size_t(f)] = 0; buried++; continue; }
    const int g = behind[size_t(f)]; if (g >= 0 && g != f && esc[size_t(g)] > esc[size_t(f)] + 0.25f) { outer[size_t(f)] = 0; lining++; } }
  if (report) { report->buried = buried; report->lining = lining; }
  if (report) report->text = "buried " + std::to_string(buried) + ", lining " + std::to_string(lining) + ", outer " + std::to_string(nf - buried - lining) + " of " + std::to_string(nf);
  return outer;
}
}
