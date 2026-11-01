// The UV solvers (no mesh, no UI): one shell as local vertices (X, 3D) and triangles, its UV per local vertex.
// The unknowns (Dofs): each vertex's u and v — free, held at a value, or tied to another (x = s·y + o: mirrored partners,
// points that must meet, a line kept straight), all solved exactly. LSCM (Lévy et al. 2002) by a sparse Cholesky
// factorisation; SLIM (Rabinovich et al. 2017: the symmetric Dirichlet energy, its local / global steps and a line search
// that never lets a triangle turn over); Tutte's embedding (its holes closed for it: never turned over — SLIM's start
// when LSCM's isn't clean)
#pragma once
#include "scene/Scene.h"
#include <array>
#include <vector>

namespace hm::uv::solve {

using P2 = std::array<double, 2>;
using Tri = std::array<int, 3>;

// the unknowns of n vertices: dof 2i — u of vertex i, 2i + 1 — its v
class Dofs {
public:
  explicit Dofs(int verts);
  int size() const { return int(par_.size()); }
  void hold(int d, double value);                            // x_d = value
  void holdPoint(int i, const P2& q) { hold(2 * i, q[0]); hold(2 * i + 1, q[1]); }
  void tie(int d, int to, double s = 1, double o = 0);       // x_d = s · x_to + o
  bool anyHeld() const;
  int heldPoints() const;                                    // (vertices with both u and v held)
  // resolved: per dof its unknown (−1: held) and x = s · z + o (z: the unknown, or the held value)
  struct Map { std::vector<int> var; std::vector<double> s, o, val; int unknowns = 0; };
  Map resolve() const;
private:
  struct Root { int r; double s, o; };
  Root find(int d) const;
  mutable std::vector<int> par_; mutable std::vector<double> s_, o_;
  std::vector<char> held_; std::vector<double> val_;
};

// a polygon (its corners in 3D, in order) cut into triangles lying the right way in its plane (ear clipping; a quad: the
// diagonal that keeps both halves the right way round) — indices into the ring
std::vector<Tri> triangulate(const std::vector<Vec3>& ring);

// LSCM: needs two points held (or ties fixing the turn, the size and the place); start — the values where the solve fails
std::vector<P2> lscm(const std::vector<Vec3>& X, const std::vector<Tri>& tris, const Dofs& D, const std::vector<P2>& start);
// Tutte: the longest border on a circle (by length), the holes closed for the solve, every inner point the mean of its
// neighbours; empty — a border that branches. mirror (each vertex's mirror; a symmetric shell): the circle turned so its
// mirror line is U = 0 — the disc symmetric too
std::vector<P2> tutte(const std::vector<Vec3>& X, const std::vector<Tri>& tris, const std::vector<int>* mirror = nullptr, const std::vector<std::array<int, 2>>* weld = nullptr);   // (weld: vertex pairs made one first — a hole zipped)
// the triangles turned over (against the shell's own way round)
int flips(const std::vector<P2>& U, const std::vector<Vec3>& X, const std::vector<Tri>& tris);
// the symmetric Dirichlet energy, area-weighted mean (4 — no stretch at all; the UV taken at the 3D size: scale-free)
double distortion(const std::vector<P2>& U, const std::vector<Vec3>& X, const std::vector<Tri>& tris);

// SLIM on U (no triangle turned over to start with — else left as it is): iters at most, stops when the energy settles;
// the held / tied dofs kept. U at the 3D size of X (X may be scaled first)
struct SlimResult { bool ok = false; int iters = 0; double e0 = 0, e1 = 0; };
SlimResult slim(std::vector<P2>& U, const std::vector<Vec3>& X, const std::vector<Tri>& tris, const Dofs& D, int iters, const std::vector<std::array<int, 2>>* soft = nullptr, double wSoft = 1e4);   // (soft: dof pairs pulled to one value — a strong pull, not a tie: the start needn't keep them)

}  // namespace hm::uv::solve
