// The actions of the Shift+RMB pie on anchors and curves, ported from the HTML version: select a lock, a curve through /
// between anchors, rotate a junction, ⫘ zip the curves at an anchor, ⚙ optimize anchors (the shape kept), ⇶ snap a part
// onto another lock's path (linked anchor pairs: a.lk — moving one moves the other), ⌖ paths through the forks, ⤳ change
// a service curve's route, ✂ separate a trunk, ✂ split a wall into its parts. Each works on the scene; the caller commits.
// Messages: "" — none, "!…" — nothing done.
#pragma once
#include "scene/Display.h"
#include "scene/Network.h"
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <QString>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace hm::aops {

// the curves of the locks the given items / anchors belong to (a lock — curves joined by walls)
std::vector<int> lockCurvesFrom(const Scene& sc, const std::vector<int>& items, const std::vector<int>& anchors);

QString curveThroughAnchors(Scene& sc, const CurveCache& cc, const std::vector<int>& list, Selection& sel);
// headLift: the middle point lifted off the head (nullopt — it stays)
QString curveBetween(Scene& sc, const CurveCache& cc, int a, int b, int fallbackGroup, const std::function<std::optional<Vec3>(const Vec3&, double)>& headLift, Selection& sel);

struct JunctionChoice { QString label; std::optional<std::pair<int, int>> pass; };
std::vector<JunctionChoice> junctionChoices(const Scene& sc, int a);

struct ZipPart { int c; bool fwd; std::vector<int> anchors; std::vector<Vec3> pts; };
struct ZipParts { std::vector<ZipPart> up, down; };
ZipParts zipParts(const Scene& sc, int J);
std::vector<Vec3> zipAverage(const std::vector<ZipPart>& parts);
QString zipApply(Scene& sc, const CurveCache& cc, Selection& sel, int J, bool up, int target);   // target: a curve of that side, -1 — the averaged line

QString optimizeAnchors(Scene& sc, const CurveCache& cc, const std::vector<int>& curves, const std::set<int>* only);

// ⇶ linked anchor pairs: one moved — the other follows (last: their positions after the previous sync) → the anchors moved
std::vector<int> linkSync(Scene& sc, std::map<int, Vec3>& last);

struct SnapTrack { std::vector<int> ids; std::vector<PathStep> steps; std::vector<Vec3> pts; std::vector<std::pair<int, int>> meta; std::vector<double> arc;
                   struct Anc { int an, c, k, si; }; std::vector<Anc> A; QString name; bool has(const Scene& sc, int a) const; };
std::optional<SnapTrack> snapTrack(Scene& sc, const CurveCache& cc, const std::vector<int>& ids);
struct SnapSeg { int c, k0, k1; };
struct SnapOption { int d; bool up; std::vector<SnapSeg> segs; int E; std::vector<Vec3> pts, tpts; QString names; bool part; };
std::vector<SnapOption> snapOptions(Scene& sc, const CurveCache& cc, int a, const SnapTrack& T);
QString snapApply(Scene& sc, const CurveCache& cc, Selection& sel, int a, const std::vector<int>& trackIds, const SnapOption& o);

std::vector<NetPath> pathsFor(Scene& sc, const CurveCache& cc, const std::vector<int>& sel);   // (visible curves: as the pie shows them)
std::vector<Vec3> pathPts(const Scene& sc, const CurveCache& cc, const NetPath& p);
QString selectPath(Scene& sc, const CurveCache& cc, Selection& sel, const NetPath& p);

struct RouteAlt { std::vector<int> N; std::vector<Network::Stretch> st; int shared = 0; std::vector<int> add, drop; };
std::vector<RouteAlt> routeAlternatives(Scene& sc, const CurveCache& cc, int c);
// wall >= 0: only that side of the wall (side 'a' / 'b'); else the service curve itself (all its walls)
QString setRoute(Scene& sc, const CurveCache& cc, Selection& sel, int c, const std::vector<int>& N, int wall, char side);

struct TrunkOpt { int e, X, P; };
std::vector<TrunkOpt> trunkSplitOptions(Scene& sc, const CurveCache& cc, int T);
QString separateTrunk(Scene& sc, const CurveCache& cc, Selection& sel, int T, const TrunkOpt& o);

struct Zone { int a0, a1, b0, b1; };
std::vector<Zone> wallZones(const Scene& sc, int w);     // a wall whose sides meet in the middle: its parts (2+), else none
QString splitWall(Scene& sc, const CurveCache& cc, Selection& sel, int w);

}  // namespace hm::aops
