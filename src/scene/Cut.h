// ✂ Scissors on curves (cutCurves / splitAtAnchor of the HTML version): a curve split at an anchor into two (the walls
// between two cut curves split too — first pieces together, second pieces together; identical trunk pieces merged into
// one); a stroke across curves: each split where the stroke crosses it (an anchor put in keeping the shape), optionally
// the upper or the lower part deleted.
#pragma once
#include "scene/Display.h"
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <QString>
#include <array>
#include <map>
#include <vector>

namespace hm::cut {

struct Hit { int c, seg; double u; };                 // where a stroke crosses curve c (Bézier segment seg at u)

// → the pieces of every curve split at J ([first, second] by curve); mergedInto: a piece dropped as identical → the one kept
std::vector<std::array<int, 2>> splitAtAnchor(Scene& sc, const CurveCache& cc, Selection& sel, int J, std::map<int, int>& mergedInto);
// mode: split | up | down → the message ("!…" — nothing cut)
QString cutCurves(Scene& sc, const CurveCache& cc, Selection& sel, const std::vector<Hit>& hits, const QString& mode);

// X junctions (separateAtX): an anchor two or more curves of a lock pass through with two or more different ways on BOTH
// sides. "Separate the zones" cuts the lock's walls across there — the curves stay whole, the walls above and below run
// on edge curves along their parts: two locks touching at the cut
std::vector<int> xAnchors(const Scene& sc, const std::vector<int>& curves);
std::vector<int> wallsAcrossX(const Scene& sc, int J);
bool isX(const Scene& sc, int J);                       // (any curves through J — the anchor pie)
bool separateAtX(Scene& sc, Selection& sel, int J, int* made = nullptr, int* kept = nullptr);
QString separateGroupX(Scene& sc, Selection& sel, int gid);   // every X of the group's locks whose walls still run across it

}  // namespace hm::cut
