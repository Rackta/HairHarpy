// ◠ The crown (item 'crown' in its own group): the parting line (upper connector, curve conn 'part'), the lower connector
// (conn 'low', a horseshoe open at the front) and crown curves from the lower connector up to the parting line (their
// ends on the connectors: an anchor of the connector, or attached — anchor raw `at` { c, t }). The crown's mesh is its
// own — built from the connectors and crown curves, no cocoon: rings parallel to the lower connector, vertical loops up
// to the parting line, converging in a fan at the whorl. Lock curves attached to the lower connector add loops there (a
// lock wall gets the same columns), so the lock's loops meet the crown's. Ported from the HTML version (crownData,
// createCrown, crownMakeSymmetric, crownSymSync, updateAttachments, connectToConnector …).
#pragma once
#include "scene/Display.h"
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <QString>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace hm::crown {

struct Rad { int c; double u, s; bool rev; };            // a crown curve: where it meets the lower connector (u) and the parting (s)
std::vector<Rad> crownCurves(const Scene& sc, const CurveCache& cc, const Item& k);   // (sorted along the lower connector)
std::vector<int> crowns(const Scene& sc);
double nearestT(const CurveSampling& c, const Vec3& p);   // arc fraction of the point of c nearest p
std::optional<Vec3> pointOnCurve(const CurveSampling* c, double t);
double connT(const Scene& sc, const CurveCache& cc, const Item& h, int a);
bool onConnector(const Scene& sc, int a, const Item& h);  // attached to h, or one of h's anchors

struct CrownMesh { std::vector<Vec3> pos; std::vector<std::vector<int>> faces; int loops = 0, rings = 0, part = 0; std::vector<int> root; std::vector<int> ring; };   // root: each loop's vertex on the lower connector; ring: each vertex's ring (−1 — the parting line's)
std::optional<CrownMesh> crownData(const Scene& sc, const CurveCache& cc, const Item& k);

// a ray at the head → the hit (nullopt: none); outermost — the last hit along the ray
using HeadHit = std::function<std::optional<Vec3>(const Vec3& o, const Vec3& d, bool outermost)>;
// a new crown from the reference (bangs: the bangs preset — the reference turned round, shorter front to back): fitted on the top of the head, or with its whorl on `target`; → the crown's id
// the head of the meshes marked as the head (from topY down): a whole character marked — only what is above its neck
Box3 headRegion(const Scene& sc, double topY);
int createCrown(Scene& sc, const std::optional<Vec3>& target, const std::optional<Vec3>& normal, const HeadHit& hit, const Vec3& center, double scale, bool sym, Selection& sel, QString* msg, const std::vector<Vec3>* coll = nullptr, bool bangs = false);
QString addCrownCurve(Scene& sc, const CurveCache& cc, int k, Selection& sel);
// the continuation: from every anchor of the lower connector a straight curve down, all leaning out alike (an even wall,
// the least lean that keeps it off the head — not hugging it),
// walls between neighbours — a skirt round the head growing out of the crown, to be cut into locks. len — × the crown's
// height. Into a group of its own
QString addContinuation(Scene& sc, const CurveCache& cc, int k, double len, const HeadHit& hit, Selection& sel);
// a crown's group and its continuation's: their point where they appear — a crown's middle; a continuation's roots
// (the crown's lower connector) — not the scene's origin. Made so; on loading, a group with none or one far off its
// contents given it (a point set by hand near them stays)
void contPivots(Scene& sc);
// one half copied onto the other across the head's centre X (master 1 — +X, −1 — −X)
QString makeSymmetric(Scene& sc, CurveCache& cc, int k, int master, double cx, double scale, Selection* sel);
// every frame: symmetric crowns — a moved anchor moves its pair, the middle ones stay on the plane (last: per crown, the
// positions after the previous sync) → the anchors moved
std::vector<int> symSync(Scene& sc, std::map<int, std::map<int, Vec3>>& last, double cx);
// anchors attached to a connector follow it at their arc fraction; moved away (a plain drag) they come off it (last: their
// positions after the previous update) → the anchors moved; msg — a toast
std::vector<int> updateAttachments(Scene& sc, const CurveCache& cc, std::map<int, Vec3>& last, double scale, QString* msg);
void insertIntoConnector(Scene& sc, const CurveCache& cc, int conn, int a);
QString connectToConnector(Scene& sc, const CurveCache& cc, int a, int conn);
QString detachFromConnector(Scene& sc, int a, Selection& sel);   // "!…" — a crown curve stays on its connector
// a sharp corner of a crown's lower connector at its anchor a: the connector alone breaks there, a lock attached at a
// keeps its smooth start (no hard step between the crown and the lock); a symmetric crown — on its pair too
int lowConnAt(const Scene& sc, int a);                   // the lower connector through a (-1: none)
bool sharpCorner(const Scene& sc, int a);
void setSharpCorner(Scene& sc, int a, bool on);
bool attachable(const Scene& sc, int a);                 // a lock curve's end, not attached yet (checkAttach: the caller picks the connector on screen)

}  // namespace hm::crown
