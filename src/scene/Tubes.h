// Curve modifiers (item 'tube'): a cross-section profile swept along its source curve — a temporary surface that follows
// the curve while it is edited. Profile points are in [-1, 1]²: x — across the lock, y — outwards from the model's
// vertical axis. Radius and rotation are set at the start, middle and end; the tip can collapse to a point. Three
// profiles (start, middle, end — raw `profs`; older saves: one, `profile`). Extra modifiers (item 'xmod', `tube`):
// Mask / Thickness / Twist painted along the lock, Noise, Curls. Convert turns it into curves and walls (a closed
// volume lock). Ported from the HTML version (tubeRings, applyExtras, rebuildTube, applyModifier, convertTube …).
#pragma once
#include "scene/Display.h"
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <QJsonArray>
#include <QString>
#include <array>
#include <optional>
#include <vector>

namespace hm::tube {

using Profile = std::vector<std::array<double, 2>>;
struct Preset { QString name; Profile pts; };
const std::vector<Preset>& builtinProfiles();
std::vector<Preset> userProfiles();                      // (QSettings "profiles")
void saveUserProfiles(const std::vector<Preset>& l);
std::optional<Profile> profileByName(const QString& name);
Profile profileOf(const QJsonValue& v);
QJsonArray profileJson(const Profile& p);
std::array<Profile, 3> tubeProfs(const Item& t);         // start, middle, end (one profile — all three)
bool samePt(const std::array<double, 2>& a, const std::array<double, 2>& b);
std::vector<int> gluedRun(const Profile& pts, int i);    // the points glued to point i (a run at the very same place)
QColor profColor(int j);                                 // the dim colour of profile point j (its guide along the lock)

const int MASK_N = 96, TUBE_NR = 96;
enum class XKind { Mask, Thick, Twist, Noise, Curl, Xform, Unknown };
XKind xkind(const QString& k);
QString xkindName(const QString& k);                     // 'Mask', 'Thickness' …
QString xkindIcon(const QString& k);
bool painted(const QString& k);                          // painted along the lock with the weight brush: mask, thick, twist
std::vector<int> extrasOf(const Scene& sc, int tube);    // its extra modifiers (outliner order)
std::vector<int> tubesOf(const Scene& sc);
int tubeOfCurve(const Scene& sc, int curve);             // -1: none
// a curve with a profile modifier is never joined to a crown's connectors: is anchor a on such a curve; is the curve on a connector
// (an anchor shared with one, or attached to one)
bool profiledAt(const Scene& sc, int anchor);
bool onConnector(const Scene& sc, int curve);
std::vector<int> onlyList(const Scene& sc, const Item& m);   // a mask's "Affects only" list
double maskAt(const std::vector<double>& m, double t);
std::vector<double> maskOf(const QJsonValue& v);

double autoTubeRadius(const Scene& sc, const CurveCache& cc, int curve);
void ensureTubeRef(Scene& sc, const CurveCache& cc, Item& t);   // (modifiers made before rRef: the automatic radius once)

// the built modifier: NR rings of n profile points (pos[i * n + j]); col — per vertex (linear, empty: plain); cl — the
// (displaced) centre line, the mask brush paints along the source curve
struct TubeMesh { int n = 0, NR = 0; std::vector<Vec3> pos; std::vector<std::array<float, 3>> col; };
std::optional<TubeMesh> buildTube(const Scene& sc, const CurveCache& cc, const Item& t, const Vec3& center);
// rings[i][j] — profile point j on ring i
std::vector<std::vector<Vec3>> tubeRings(const Scene& sc, const Item& t, const std::vector<Vec3>& curvePts, int NR, const Vec3& center,
                                         const QJsonObject* pOverride = nullptr);

// the curves Apply works on: selected curves, the source curves of selected modifiers
std::vector<int> modTargets(const Scene& sc, const Selection& sel, const CurveCache& cc);
// the modifiers Extra works on: selected modifiers, modifiers of selected curves, of selected extras
std::vector<int> modTubes(const Scene& sc, const Selection& sel);
QString applyModifier(Scene& sc, const CurveCache& cc, Selection& sel, const Profile& prof);
struct Clip { QJsonValue profile, profs; QJsonObject p; };
QString pasteModifier(Scene& sc, const CurveCache& cc, Selection& sel, const Clip& clip);
QString convertTube(Scene& sc, const CurveCache& cc, Selection& sel, int tubeId, const Vec3& center);
int newExtra(Scene& sc, const CurveCache& cc, int tubeId, const QString& kind);
QString addExtra(Scene& sc, const CurveCache& cc, Selection& sel, const QString& kind);

}  // namespace hm::tube
