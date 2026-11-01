// A group's symmetry (raw `sym` { on, axis }): everything inside is shown mirrored across the head's centre plane, live;
// Apply bakes the mirrored half into real items. Duplicate (D): a copy of the selected items next to them. Ported from
// the HTML version (symOf / mirrorPt / applySymmetry / duplicateSelection). The caller commits.
#pragma once
#include "scene/Display.h"
#include "scene/Scene.h"
#include <QString>

namespace hm::sym {

// the axis (0 x, 1 y, 2 z) of the nearest group — the item itself or one above it — with symmetry on; -1: none
int symAxis(const Scene& sc, const Item& it);
// the plane's centre for it: the point of the group holding the symmetric group (the nearest one above it with a point of its
// own — moved, it takes the plane along), else `head` (the head's centre)
Vec3 symCenter(const Scene& sc, const Item& it, const Vec3& head);
Vec3 groupPoint(const Item& g);              // (a group's own point — 0, 0, 0 unless set)
inline Vec3 mirrorPt(Vec3 p, int ax, const Vec3& center) {
  if (ax == 0) p.x = 2 * center.x - p.x; else if (ax == 1) p.y = 2 * center.y - p.y; else p.z = 2 * center.z - p.z;
  return p;
}

// "!…" — nothing done
QString applySymmetry(Scene& sc, int gid, const Vec3& center, Selection& sel);
QString duplicateSelection(Scene& sc, Selection& sel);

}  // namespace hm::sym
