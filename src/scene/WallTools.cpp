#include "scene/WallTools.h"
#include "scene/Edit.h"
#include <QJsonObject>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <optional>

namespace hm::walltools {

namespace {
const double PI = 3.14159265358979323846, KINK_TURN = 35, KINK_EX = 20, KINK_TOWARD = 0.5;
double angleTo(const Vec3& a, const Vec3& b) { double d = std::sqrt(a.lengthSq() * b.lengthSq()); return d == 0 ? PI / 2 : std::acos(clamp(a.dot(b) / d, -1, 1)); }
template <class T> bool has(const std::vector<T>& v, const T& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
}  // namespace

// ✂ Width segments → curves: column ends meeting a driver curve's end (a pointed tip, a shared root) are welded to that
// anchor; a second surface becomes a wall of its own
QString segmentsToCurves(Scene& sc, const CurveCache& cc, const std::map<int, WallMesh>& walls, Selection& sel, const std::vector<int>& list0) {
  std::vector<int> list; for (int w : list0) if (const Item* s = sc.item(w); s && s->type == ItemType::Surface && s->raw.value("p").toObject().value("segs").toInt(1) >= 2) list.push_back(w);
  if (list.empty()) return QObject::tr("!Select a NURBS surface with width segments (Width segments ≥ 1)");
  double scale = 0; { Box3 b = sc.bounds(); Vec3 s = b.size(); scale = std::max({s.x, s.y, s.z}); }
  std::vector<int> madeC, madeS; Selection del;
  for (int wid : list) {
    auto wm = walls.find(wid); if (wm == walls.end() || wm->second.grids.empty()) continue;
    const Item w = *sc.item(wid); const Item *A = sc.item(w.a), *B = sc.item(w.b); if (!A || !B) continue;
    QJsonObject p = w.raw.value("p").toObject(); const int segs = p.value("segs").toInt(1), M = segs + 1;
    const auto& G = wm->second.grids[0]; if (G.M != M) continue;
    const int R = G.R, K = std::max({int(A->anchors.size()), int(B->anchors.size()), 3});
    const std::vector<int> ends{A->anchors.front(), B->anchors.front(), A->anchors.back(), B->anchors.back()}; const double tol = 1e-4 * scale;
    std::vector<int> chain{A->id};
    for (int j = 1; j < segs; j++) {
      std::vector<Vec3> col; for (int k = 0; k < R; k++) col.push_back(wm->second.pos[size_t(G.start + k * M + j)]);
      std::vector<double> L{0}; for (int k = 1; k < R; k++) L.push_back(L.back() + col[size_t(k)].distanceTo(col[size_t(k - 1)]));
      auto at = [&](double s) { int k = 0; while (k < R - 2 && L[size_t(k + 1)] < s) k++; double g = L[size_t(k + 1)] - L[size_t(k)]; return col[size_t(k)].lerp(col[size_t(k + 1)], g > 0 ? clamp((s - L[size_t(k)]) / g, 0, 1) : 0); };
      std::vector<int> an;
      for (int q = 0; q < K; q++) {
        Vec3 pt = at(L.back() * q / (K - 1)); int e = -1;
        if (q == 0 || q == K - 1) for (int x : ends) if (sc.anchor(x)->pos.distanceTo(pt) < tol) { e = x; break; }
        an.push_back(e >= 0 ? e : sc.addAnchor(pt).id);
      }
      Item& c = edit::makeUserCurve(sc, w.parent); c.anchors = an;
      if (A->fair || B->fair) { c.fair = std::max(A->fair, B->fair); c.fairSet = true; }
      madeC.push_back(c.id); chain.push_back(c.id);
    }
    chain.push_back(B->id);
    auto addWall = [&](int a, int b, QJsonObject pp) {
      int n = edit::nextNumber(sc, ItemType::Surface);
      Item& s = sc.addItem(ItemType::Surface, w.parent, QString("Surface %1").arg(n)); s.a = a; s.b = b; s.raw.insert("p", pp); madeS.push_back(s.id);
    };
    for (size_t i = 0; i + 1 < chain.size(); i++) {
      QJsonObject pp{{"segs", 1}, {"bulge", 0}, {"flip", p.value("flip").toBool()}}; if (p.contains("bfade") && !p.value("bfade").isNull()) pp.insert("bfade", p.value("bfade"));
      addWall(chain[i], chain[i + 1], pp);
    }
    if (p.value("b2on").toBool() && p.value("bulge").toDouble() != 0) {         // (the twin: a wall of its own)
      QJsonObject tp{{"segs", std::max(1, p.value("b2segs").toInt(1))}, {"bulge", p.value("b2bulge").isDouble() ? p.value("b2bulge") : p.value("bulge")},
                     {"flip", p.value("b2flip").isBool() ? p.value("b2flip").toBool() : !p.value("flip").toBool()}, {"bfade", p.value("bfade")}};
      addWall(w.b, w.a, tp);
    }
    del.items.push_back(wid);
  }
  sc.reindex();
  if (!del.items.empty()) edit::deleteItems(sc, cc, del);
  sel.anchors.clear(); sel.items = madeC; sc.reindex();
  return QObject::tr("%1 curve%2 and %3 wall%4 made from the segments").arg(madeC.size()).arg(madeC.size() == 1 ? "" : "s").arg(madeS.size()).arg(madeS.size() == 1 ? "" : "s");
}

namespace {
// how far the side turns at its anchor a, on the curves themselves (degrees); nullopt — a stretch on no built curve
std::optional<double> kinkLocal(const Scene& sc, const CurveCache& cc, int a, int pa, int na) {
  struct Leg { const CurveSampling* S; int j, s; double len; };
  auto leg = [&](int nb) -> std::optional<Leg> {
    for (const Item* c : sc.curves()) {
      if (c->junc || c->ring >= 0) continue;
      auto it = cc.find(c->id); if (it == cc.end() || it->second.p.size() < 3 || it->second.arc.size() != it->second.p.size()) continue;
      auto ia = std::find(c->anchors.begin(), c->anchors.end(), a), ib = std::find(c->anchors.begin(), c->anchors.end(), nb);
      if (ia == c->anchors.end() || ib == c->anchors.end() || std::abs(int(ia - ib)) != 1) continue;
      const auto& S = it->second;
      auto near = [&](const Vec3& q) { int j = 0; double b = 1e300; for (size_t m = 0; m < S.p.size(); m++) { double d = (S.p[m] - q).lengthSq(); if (d < b) { b = d; j = int(m); } } return j; };
      int j = near(sc.anchor(a)->pos), jn = near(sc.anchor(nb)->pos); if (j == jn) continue;
      return Leg{&S, j, jn > j ? 1 : -1, std::abs(S.arc[size_t(jn)] - S.arc[size_t(j)])};
    }
    return std::nullopt;
  };
  auto L0 = leg(pa), L1 = leg(na); if (!L0 || !L1) return std::nullopt;
  double h = 0.25 * std::min(L0->len, L1->len); if (!(h > 0)) return std::nullopt;
  auto at = [&](const Leg& L) { const auto& A = L.S->arc; double t = A[size_t(L.j)] + L.s * h; size_t i = 1; while (i < A.size() - 1 && A[i] < t) i++;
    double g = A[i] - A[i - 1]; return L.S->p[i - 1].lerp(L.S->p[i], clamp((t - A[i - 1]) / (g ? g : 1), 0, 1)); };
  Vec3 ap = sc.anchor(a)->pos, d0 = ap - at(*L0), d1 = at(*L1) - ap;
  if (d0.lengthSq() < 1e-18 || d1.lengthSq() < 1e-18) return std::nullopt;
  return angleTo(d0, d1) * 180 / PI;
}
}  // namespace

std::vector<Kink> wallKinks(const Scene& sc, const CurveCache& cc, int wid) {
  std::vector<Kink> out; const Item* w = sc.item(wid); if (!w) return out;
  const Item *A = sc.item(w->a), *B = sc.item(w->b); if (!A || !B || A->type != ItemType::Curve || B->type != ItemType::Curve) return out;
  auto ptsOf = [&](const Item* c) {
    auto it = cc.find(c->id); if (it != cc.end() && it->second.p.size() > 1) return it->second.p;
    std::vector<Vec3> o; for (size_t i = 0; i < c->anchors.size(); i++) { const Vec3& p = sc.anchor(c->anchors[i])->pos; if (!i) o.push_back(p); else for (int k = 1; k <= 4; k++) o.push_back(sc.anchor(c->anchors[i - 1])->pos.lerp(p, k / 4.0)); }
    return o;
  };
  for (auto [Sd, O] : {std::pair<const Item*, const Item*>{A, B}, {B, A}}) {
    const auto& An = Sd->anchors; auto OP = ptsOf(O); if (An.size() < 3 || OP.size() < 2) continue;
    const int n = int(An.size()); auto P = [&](int i) { return sc.anchor(An[size_t(i)])->pos; };
    std::vector<double> turn(size_t(n), 0.0); for (int i = 1; i < n - 1; i++) turn[size_t(i)] = 180 - angleTo(P(i - 1) - P(i), P(i + 1) - P(i)) * 180 / PI;
    std::map<int, std::optional<double>> LC;
    auto lcAt = [&](int j) -> std::optional<double> { if (j <= 0 || j >= n - 1) return std::nullopt; auto f = LC.find(j); if (f != LC.end()) return f->second; auto v = kinkLocal(sc, cc, An[size_t(j)], An[size_t(j - 1)], An[size_t(j + 1)]); LC[j] = v; return v; };
    for (int i = 1; i < n - 1; i++) {
      int a = An[size_t(i)]; double ex = turn[size_t(i)] - (turn[size_t(i - 1)] + turn[size_t(i + 1)]) / 2;
      if (has(O->anchors, a) || turn[size_t(i)] < KINK_TURN || ex < KINK_EX) continue;
      Vec3 p = P(i - 1), d = P(i + 1) - p; double L2 = d.lengthSq(); if (L2 < 1e-9) continue;
      Vec3 foot = p + d * clamp((P(i) - p).dot(d) / L2, 0, 1), v = P(i) - foot; if (v.lengthSq() < 1e-12) continue;
      double bd = 1e300; Vec3 bp; for (auto& o : OP) { double dd = (o - foot).lengthSq(); if (dd < bd) { bd = dd; bp = o; } }
      Vec3 toO = bp - foot; if (toO.lengthSq() < 1e-12) continue;
      double tw = v.normalized().dot(toO.normalized()); if (tw < KINK_TOWARD) continue;
      const Item* own = Sd; for (const Item* c : sc.curves()) if (!c->junc && c->ring < 0 && has(c->anchors, a)) { own = c; break; }
      auto lc = lcAt(i); double lex = ex;
      if (lc) { std::vector<double> nb; for (int j : {i - 1, i + 1}) if (auto x = lcAt(j)) nb.push_back(*x);
        lex = *lc - (nb.empty() ? 0 : (nb[0] + (nb.size() > 1 ? nb[1] : nb[0])) / 2); if (*lc < KINK_TURN || lex < KINK_EX) continue; }
      int oi = int(std::find(own->anchors.begin(), own->anchors.end(), a) - own->anchors.begin());
      out.push_back({a, wid, own->id, oi, lc ? *lc : turn[size_t(i)], lex, lex * tw});
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const Kink& x, const Kink& y) { return x.score > y.score; });
  return out;
}

}  // namespace hm::walltools
