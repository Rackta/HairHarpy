#include "scene/AutoFix.h"
#include "scene/Network.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hm {

namespace {

const char* WELD_KEYS[] = {"weld", "lockWeld", "weldOk", "weldSnap"};
const double INF = std::numeric_limits<double>::infinity();

double nearestF(const std::vector<Vec3>& P, const Vec3& p) {
  double best = 0, bd = INF;
  for (size_t i = 0; i + 1 < P.size(); i++) {
    const Vec3& a = P[i]; double bx = P[i + 1].x - a.x, by = P[i + 1].y - a.y, bz = P[i + 1].z - a.z, l2 = bx * bx + by * by + bz * bz;
    double s = l2 > 0 ? ((p.x - a.x) * bx + (p.y - a.y) * by + (p.z - a.z) * bz) / l2 : 0; s = s < 0 ? 0 : s > 1 ? 1 : s;
    double dx = a.x + bx * s - p.x, dy = a.y + by * s - p.y, dz = a.z + bz * s - p.z, d = dx * dx + dy * dy + dz * dz;
    if (d < bd) { bd = d; best = double(i) + s; }
  }
  return best;
}
// (+v).toPrecision(3)
QString fmtLen(double v) {
  if (v == 0) return "0.00";
  int e = int(std::floor(std::log10(std::abs(v))));
  if (e >= 3 || e < -6) return QString::number(v, 'e', 2).replace("e+0", "e+").replace("e-0", "e-");
  return QString::number(v, 'f', std::max(0, 2 - e));
}
bool hasId(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// a cocoon's mesh as it is now (ringMeshOf, no forced loops)
bool meshOf(Scene& sc, HairState& hs, int k, LockMesh& out) {
  CocoonSystem* cs = hs.cocoonSystem(); LockInput in; if (!cs || !cs->lockInput(k, in)) return false;
  out = ringMeshOf(in, hs.walls, weldSettings(sc), sceneScale(sc));
  return !out.faces.empty();
}
// the parent and the child of a place (a related pair, the source first)
bool fixPair(HairState& hs, const std::vector<int>& locks, int& P, int& C) {
  CocoonSystem* cs = hs.cocoonSystem(); if (!cs) return false;
  for (int a : locks) for (int b : locks) if (a != b && hasId(cs->deps(b), a)) { P = a; C = b; return true; }
  return false;
}
// the child's strands: a live cocoon's curves, else its lock's walls' curves (not edge / junction curves)
std::vector<int> fixCurvesOf(Scene& sc, HairState& hs, int k) {
  CocoonSystem* cs = hs.cocoonSystem();
  auto cids = cs->strandCurves(k); std::vector<int> out;
  if (!cids.empty()) { for (int id : cids) if (const Item* c = sc.item(id); c && c->type == ItemType::Curve) out.push_back(id); return out; }
  LockInput in; if (!cs->lockInput(k, in)) return out;
  for (auto& w : in.walls) for (int id : {w.a, w.b}) if (!hasId(out, id)) if (const Item* c = sc.item(id); c && c->type == ItemType::Curve && !c->hasPath() && !c->junc) out.push_back(id);
  return out;
}
const Anchor* heldAnchor(const Scene& sc, int id) {                  // (an anchor some curve still holds)
  for (const Item* c : sc.curves()) if (hasId(c->anchors, id)) return sc.anchor(id);
  return nullptr;
}

}  // namespace

SceneState captureState(const Scene& sc) {
  SceneState s{sc.items, sc.anchors, sc.roots, {}};
  for (const char* k : WELD_KEYS) if (sc.fileRaw.contains(k)) s.weld.insert(k, sc.fileRaw.value(k));
  return s;
}

void restoreState(Scene& sc, HairState& hs, const SceneState& s) {
  sc.items = s.items; sc.anchors = s.anchors; sc.roots = s.roots;
  for (const char* k : WELD_KEYS) { if (s.weld.contains(k)) sc.fileRaw.insert(k, s.weld.value(k)); else sc.fileRaw.remove(k); }
  sc.reindex(); sc.revision++;
  hs.load(sc);
}

std::vector<FixOp> weldFixOps(Scene& sc, HairState& hs, const WeldProblem& p) {
  int P, C; if (!fixPair(hs, p.locks, P, C)) return {};
  CocoonSystem* cs = hs.cocoonSystem(); CocoonGeo G = cs->geo(C); double h = cs->loopStep(C);
  std::vector<FixOp> out;
  // the strands' root ends: each curve's end nearer the child's root; curves sharing one, near the place
  std::vector<int> rOrder; std::map<int, std::vector<int>> byR;
  for (int cid : fixCurvesOf(sc, hs, C)) {
    const Item* c = sc.item(cid); if (c->anchors.size() < 3) continue;
    const Anchor *e0 = sc.anchor(c->anchors.front()), *e1 = sc.anchor(c->anchors.back()); if (!e0 || !e1) continue;
    const Anchor* R = nearestF(G.c, e0->pos) <= nearestF(G.c, e1->pos) ? e0 : e1;
    if (R->pos.distanceTo(p.c) > 2 * h) continue;
    if (!byR.count(R->id)) rOrder.push_back(R->id); byR[R->id].push_back(cid);
  }
  for (int Rid : rOrder) {
    const auto& cs_ = byR[Rid]; if (cs_.size() < 2) continue;
    const Vec3& Rp = sc.anchor(Rid)->pos;
    for (int Aid : cs_) for (int Bid : cs_) {
      if (Aid == Bid) continue;
      const Item *A = sc.item(Aid), *B = sc.item(Bid);
      int X = A->anchors.front() == Rid ? A->anchors[1] : A->anchors[A->anchors.size() - 2];
      if (hasId(B->anchors, X)) continue;
      // (only a merge near the root and the place, B not dragged across)
      int pB = B->anchors.front() == Rid ? B->anchors[1] : B->anchors[B->anchors.size() - 2];
      const Vec3 &Xp = sc.anchor(X)->pos, &pBp = sc.anchor(pB)->pos;
      if (Xp.distanceTo(Rp) > 1.5 * h || Xp.distanceTo(p.c) > 1.5 * h || pBp.distanceTo(Xp) > pBp.distanceTo(Rp) + 0.25 * h) continue;
      FixOp op; op.kind = "merge"; op.P = P; op.C = C; op.B = Bid; op.R = Rid; op.X = X;
      op.txt = QString("%1 joins %2 below the root").arg(B->name, A->name);
      out.push_back(op);
    }
  }
  // 'snap' — the child's wall lies on the parent's a little off (an 'off' place)
  bool anyOff = false, allOff = true; double m = -INF;
  for (auto& e : p.edges) { if (e.kind == "off") { anyOff = true; m = std::max(m, e.a.distanceTo(e.b)); } else allOff = false; }
  if (anyOff) {
    FixOp op; op.kind = "snap"; op.P = P; op.C = C; op.c = p.c; op.ks = {P, C};
    op.txt = QString("%1's points welded onto %2's (up to %3 off)").arg(sc.item(C)->name, sc.item(P)->name, fmtLen(m));
    if (allOff) return {op};                                       // (only that there: no anchor merge tried first)
    out.push_back(op);
  }
  return out;
}

bool weldFixDo(Scene& sc, HairState& hs, const FixOp& op) {
  if (op.kind == "merge") {
    Item* B = sc.item(op.B); const Anchor *R = heldAnchor(sc, op.R), *X = heldAnchor(sc, op.X);
    if (!B || !R || !X) return false;
    auto it = std::find(B->anchors.begin(), B->anchors.end(), op.R); if (it == B->anchors.end() || hasId(B->anchors, op.X)) return false;
    Vec3 rp = R->pos; double rs = R->smooth;
    Anchor& x = sc.addAnchor(rp); x.smooth = rs; *it = x.id; B->ovr.erase(op.R);
    std::vector<int> sel, asel;
    Network net(sc, hs.curves, sel, asel);
    net.weldInto(x.id, op.X); net.extendYBranches(op.X); net.segmentNetwork();
    sc.revision++; hs.load(sc);                                    // (every curve, wall and cocoon made anew)
    return true;
  }
  if (op.kind == "snap") {
    QJsonArray a = sc.fileRaw.value("weldSnap").toArray(); QJsonArray ks; for (int k : op.ks) ks.append(k);
    a.append(QJsonObject{{"c", QJsonArray{op.c.x, op.c.y, op.c.z}}, {"ks", ks}}); sc.fileRaw.insert("weldSnap", a);
    return true;
  }
  return false;
}

int weldFixLeft(Scene& sc, HairState& hs, const Vec3& c, const std::vector<int>& ks) {
  if (ks.size() < 2 || !sc.item(ks[0]) || !sc.item(ks[1])) return -1;
  double h = hs.cocoonSystem()->loopStep(ks[1]);
  LockMesh mp, mc; std::vector<LockIn> list;
  if (meshOf(sc, hs, ks[0], mp)) list.push_back({ks[0], &mp});
  if (meshOf(sc, hs, ks[1], mc)) list.push_back({ks[1], &mc});
  if (list.size() < 2) return -1;
  int n = 0; for (auto& p : weldLocks(sc, hs, list).problems) if (p.c.distanceTo(c) < 1.5 * h) n++;
  return n;
}

AutoFixResult weldAutoFix(Scene& sc, HairState& hs, const std::vector<WeldProblem>& problems, const std::function<void(double)>& onStep) {
  AutoFixResult r; r.base = captureState(sc);
  for (size_t q = 0; q < problems.size(); q++) {
    const WeldProblem& p = problems[q]; if (onStep) onStep(double(q) / problems.size());
    int P, C; if (!fixPair(hs, p.locks, P, C)) continue;
    std::vector<int> ks{P, C};
    int left0 = weldFixLeft(sc, hs, p.c, ks);
    QString tr = QString("place %1: left %2").arg(q).arg(left0);
    if (left0 == 0) { r.trace << tr; continue; }                   // (not this pair's own — another lock's doing)
    WeldProblem pp = p; pp.locks = ks;
    auto ops = weldFixOps(sc, hs, pp);
    for (auto& op : ops) tr += " | " + op.kind + " " + op.txt;
    r.trace << tr;
    for (const FixOp& op : ops) {
      SceneState keep = captureState(sc);
      // the child's mesh away from the place (farther than 3 loop steps): a fix mends the joint only
      double h = hs.cocoonSystem()->loopStep(C); std::vector<Vec3> far; LockMesh m0;
      bool haveFar = meshOf(sc, hs, C, m0); if (haveFar) for (auto& v : m0.pos) if (v.distanceTo(p.c) > 3 * h) far.push_back(v);
      bool ok = weldFixDo(sc, hs, op) && weldFixLeft(sc, hs, p.c, ks) == 0;
      if (ok && haveFar && !far.empty()) {
        LockMesh m1; if (!meshOf(sc, hs, C, m1) && m1.pos.empty()) ok = false;
        else {
          double tol = 0.15 * h; int off = 0;
          for (auto& v : far) { double mn = INF; for (auto& w : m1.pos) { double d = (v - w).lengthSq(); if (d < mn) mn = d; if (mn < tol * tol) break; } if (std::sqrt(mn) > tol) off++; }
          ok = off <= std::max(2.0, 0.02 * far.size());
        }
      }
      if (ok) { r.fixes.push_back({p.c, ks, op, true, false}); r.trace.back() += " => kept: " + op.kind; break; }
      if (op.kind == "merge") restoreState(sc, hs, keep);
      else { for (const char* k : WELD_KEYS) { if (keep.weld.contains(k)) sc.fileRaw.insert(k, keep.weld.value(k)); else sc.fileRaw.remove(k); } }
    }
  }
  if (onStep) onStep(1);
  return r;
}

void weldFixApply(Scene& sc, HairState& hs, const AutoFixResult& r) {
  restoreState(sc, hs, r.base);
  for (auto& f : r.fixes) if (f.on) weldFixDo(sc, hs, f.op);
}

}  // namespace hm
