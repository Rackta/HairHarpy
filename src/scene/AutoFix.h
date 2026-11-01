// Auto-fix (the Inspector's Report button, the HTML version's weldAutoFix): a place where a child lock doesn't meet its
// parent, of a kind the program knows how to mend — each fix tried on the state the ones before it left, kept only if
// the place is gone and the child's mesh away from it stays as it was; the user then reviews each (before / after,
// accept, reject). Kinds:
//   'merge' — two strands of the child running separately to one root anchor: the other strand's end welded into the
//             first one's anchor next to the root (the strands meet below it, one trunk curve up to the root);
//   'snap'  — the child's wall lying on the parent's a little off: those points welded onto the parent's there
//             (S.weldSnap — Weld locks does it at this place from now on).
// A fix is an operation by ids, so it can be done again on the state before all fixes.
#pragma once
#include "scene/Convert.h"
#include "scene/HairState.h"
#include "scene/Scene.h"
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <vector>

namespace hm {

struct FixOp {
  QString kind;                                  // merge | snap
  int P = -1, C = -1;                            // the parent and the child cocoon
  int B = -1, R = -1, X = -1;                    // merge: the curve, its root anchor, the anchor it is welded into
  Vec3 c; std::vector<int> ks;                   // snap: the place, [P, C]
  QString txt;
};
struct WeldFix { Vec3 c; std::vector<int> ks; FixOp op; bool on = true, done = false; };

// the scene as a step of undo keeps it (items, anchors, roots, the weld settings and marks)
struct SceneState { std::map<int, Item> items; std::map<int, Anchor> anchors; std::vector<int> roots; QJsonObject weld; };
SceneState captureState(const Scene& sc);
void restoreState(Scene& sc, HairState& hs, const SceneState& s);   // (the geometry built anew)

std::vector<FixOp> weldFixOps(Scene& sc, HairState& hs, const WeldProblem& p);   // the operations to try, in order
bool weldFixDo(Scene& sc, HairState& hs, const FixOp& op);                        // false — its curves / anchors are gone
int weldFixLeft(Scene& sc, HairState& hs, const Vec3& c, const std::vector<int>& ks);   // problems left there (-1: no meshes)

struct AutoFixResult { SceneState base; std::vector<WeldFix> fixes; QStringList trace; };   // (trace: per place, what was tried)
// every place tried in turn; onStep(done 0…1) between them
AutoFixResult weldAutoFix(Scene& sc, HairState& hs, const std::vector<WeldProblem>& problems, const std::function<void(double)>& onStep = {});
// the state for the fixes switched on: the base, then their operations again
void weldFixApply(Scene& sc, HairState& hs, const AutoFixResult& r);

}  // namespace hm
