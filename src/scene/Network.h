// The curve network of the Hair room's new logic and the edits on it, ported from the HTML version: curves cut at the
// forks (segmentNetwork), the same curve twice merged (mergeDuplicateCurves), two pieces meeting with nothing else there
// joined (joinThroughNodes), the network names (autoNetNames, C_ / S_); edge curves (EC, c.path) laid along the pieces;
// welding anchors (+ a Y branch extended along its host), joining two curves, anchors put in (with or without keeping
// the shape). Everything here works on the scene directly; the caller rebuilds the geometry afterwards.
#pragma once
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hm {

struct PathStep { int c; int from; };                 // a path through the forks: the curve, the anchor it is entered at
using NetPath = std::vector<PathStep>;

class Network {
public:
  // samples: the curves' samples as the HTML version's c.samples / c.arc are at that moment — from the last frame (a
  // curve made since has none: its anchors stand in); sel / asel: the selection, kept in step with deletions
  Network(Scene& sc, const CurveCache& samples, std::vector<int>& sel, std::vector<int>& asel);

  // commit()'s upkeep, in its order → whether the curves / walls changed (renamed: names only)
  struct Upkeep { int merged = 0, cut = 0, joined = 0; bool renamed = false; bool changed() const { return merged || cut || joined; } };
  Upkeep upkeep(std::map<std::string, std::string>& netSig);
  // as a project opens: cut, merged, joined, the names given (their signatures start anew)
  Upkeep loadUpkeep(std::map<std::string, std::string>& netSig);

  int segmentNetwork();
  int mergeDuplicateCurves();
  int repairPathCurves();
  int joinThroughNodes();
  bool autoNetNames(std::map<std::string, std::string>& netSig);
  std::map<int, QString> netNamePlan(int group);       // (group −1: outside the groups)

  void weldInto(int a, int b);
  std::vector<QString> extendYBranches(int J);
  void weld(int a, int b, const std::function<void()>& before = {});
  void joinCurves(int a, int b);
  void reverseCurves(const std::vector<int>& cs);
  void recalcSilent(const std::vector<int>& cs);
  void insertOnSegment(int c, int seg, int a);
  int insertKeepShape(int c, int seg, double u);       // → the new anchor
  struct SoleEnd { int c; bool start; };
  std::optional<SoleEnd> soleEnd(int a) const;

  void deleteItem(int id);
  Item& makeCurve(int parent);
  void pruneAnchors();                                 // anchors no curve holds any more go

  std::vector<NetPath> curvePaths(const std::vector<int>& sel, bool all = true) const;   // all: hidden curves too
  Vec3 leaveDir(const Item& c, int a) const;
  int dropOrphanServiceCurves();                       // Hole curves left by walls deleted before

  struct Stretch { int c, i0, i1; };
  std::optional<std::vector<Stretch>> routeStretches(const std::vector<int>& L, const std::set<int>& skip = {}) const;
  std::optional<std::vector<int>> pathAnchors(const std::vector<std::array<int, 3>>& path) const;
  int pathCurve(const std::vector<Stretch>& st, int parent);   // the service curve (EC) along stretches (an existing one reused)
  QString serviceName(const QString& pre) const;       // the next free "<pre>_n" (HC, JC, EC)

private:
  std::vector<const Item*> curves() const { return sc_.curves(); }
  std::vector<const Item*> surfaces() const;
  const std::vector<Vec3>* samplesOf(int id) const;
  double curveLen(const Item& c) const;
  bool isNewLogic(const Item& c) const;
  std::optional<std::vector<int>> bridgeAnchors(const std::vector<int>& L) const;
  QString nextName(ItemType type, const QString& base);
  void dedupeWalls();
  void dropSel(int id);
  void remapCurveRefs(int from, int to, const std::function<double(double)>& t);   // guide hits, attachments, tubes

  Scene& sc_;
  const CurveCache& samples_;
  std::vector<int>& sel_;
  std::vector<int>& asel_;
};

}  // namespace hm
