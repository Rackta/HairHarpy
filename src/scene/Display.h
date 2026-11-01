// What the Hair room shows and lets you pick, as syncVisuals / rebuildAnchors decide in the HTML version: a group's
// curves and anchors in its 'auto' mode only while you work inside it (activeGroups — the selection's groups), free
// curves (no wall) shown anyway, service curves (HC / JC / EC) only while selected.
#pragma once
#include "scene/Scene.h"
#include <algorithm>
#include <set>
#include <vector>

namespace hm {

struct Selection {
  std::vector<int> items;                // S.sel — in selection order (the last one is the active one)
  std::vector<int> anchors;              // S.asel
  bool hasItem(int id) const { return std::find(items.begin(), items.end(), id) != items.end(); }
  bool hasAnchor(int id) const { return std::find(anchors.begin(), anchors.end(), id) != anchors.end(); }
  bool empty() const { return items.empty() && anchors.empty(); }
};

struct DisplayState {
  std::set<int> activeGroups, looseCurves;
  bool showCurves = true, showAnchors = true, showSurfaces = true, showGuides = true;
};

DisplayState displayState(const Scene& sc, const Selection& sel);
bool isServiceGroup(const Item& g);
bool guidesShown(const Scene& sc, const DisplayState& ds, const Selection& sel, const Item& it);
bool curveShown(const Scene& sc, const DisplayState& ds, const Selection& sel, const Item& c);   // c.obj.visible

// the anchors shown (and pickable): S.anchorList, with their look
struct ShownAnchor { int id; float rgb[3]; float size; int shape; };   // shape: 0 circle, 1 diamond (start), 2 ring (end)
// weldCand: the anchor a moved one would weld to (magenta, larger) while the weld menu is up
std::vector<ShownAnchor> anchorList(const Scene& sc, const DisplayState& ds, const Selection& sel, int weldCand = -1);

}  // namespace hm
