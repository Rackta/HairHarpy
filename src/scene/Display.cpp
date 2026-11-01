#include "scene/Display.h"
#include <QJsonObject>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace hm {

namespace {
const Item* parentOf(const Scene& sc, const Item* it) { return it && it->parent >= 0 ? sc.item(it->parent) : nullptr; }

// looseCurveIds: curves joined to no wall and no modifier (nor connectors, junctions, disc rings, crown curves)
std::set<int> looseCurveIds(const Scene& sc) {
  std::set<int> used;
  for (auto& [id, it] : sc.items) {
    if (it.type == ItemType::Surface) { used.insert(it.a); used.insert(it.b); }
    else if (it.type == ItemType::Tube || it.type == ItemType::Braid) used.insert(it.raw.value("src").toInt(-1));
    else if (it.type == ItemType::Crown) { used.insert(it.raw.value("part").toInt(-1)); used.insert(it.raw.value("low").toInt(-1)); }
  }
  auto sk = [](int a, int b) { return a < b ? (uint64_t(uint32_t(a)) << 32 | uint32_t(b)) : (uint64_t(uint32_t(b)) << 32 | uint32_t(a)); };
  std::unordered_set<uint64_t> onWall;
  for (const Item* c : sc.curves()) if (c->junc && used.count(c->id)) for (size_t k = 0; k + 1 < c->anchors.size(); k++) onWall.insert(sk(c->anchors[k], c->anchors[k + 1]));
  std::set<int> out;
  for (const Item* c : sc.curves()) {
    if (used.count(c->id) || c->ring >= 0 || !c->conn.isEmpty() || c->junc) continue;
    bool att = false; for (int a : c->anchors) if (const Anchor* an = sc.anchor(a)) if (an->raw.contains("at")) att = true;
    if (att) continue;
    bool covered = c->anchors.size() > 1;
    for (size_t k = 1; k < c->anchors.size() && covered; k++) if (!onWall.count(sk(c->anchors[k - 1], c->anchors[k]))) covered = false;
    if (!covered) out.insert(c->id);
  }
  return out;
}
}  // namespace

bool isServiceGroup(const Item& g) {
  return g.raw.value("guidesGroup").toBool() || g.raw.value("meshesGroup").toBool() || g.raw.value("importGroup").toBool();
}

DisplayState displayState(const Scene& sc, const Selection& sel) {
  DisplayState ds;
  QJsonObject view = sc.fileRaw.value("settings").toObject().value("view").toObject();
  ds.showCurves = view.value("showCurves").toBool(true); ds.showAnchors = view.value("showAnchors").toBool(true);
  ds.showSurfaces = view.value("showSurfaces").toBool(true); ds.showGuides = view.value("showGuides").toBool(true);
  // activeGroups: every group holding the selection (and the selected groups' sub-groups)
  auto up = [&](const Item* it) { for (const Item* p = it && it->type == ItemType::Group ? it : parentOf(sc, it); p; p = parentOf(sc, p)) ds.activeGroups.insert(p->id); };
  std::function<void(const Item*)> down = [&](const Item* g) {
    ds.activeGroups.insert(g->id);
    for (int id : g->children) if (const Item* c = sc.item(id)) if (c->type == ItemType::Group) down(c);
  };
  for (int id : sel.items) { const Item* it = sc.item(id); if (!it || it->type == ItemType::Head) continue; up(it); if (it->type == ItemType::Group) down(it); }
  if (!sel.anchors.empty()) {
    std::unordered_set<int> set(sel.anchors.begin(), sel.anchors.end());
    for (const Item* c : sc.curves()) for (int a : c->anchors) if (set.count(a)) { up(c); break; }
  }
  ds.looseCurves = looseCurveIds(sc);
  return ds;
}

bool guidesShown(const Scene& sc, const DisplayState& ds, const Selection& sel, const Item& it) {
  const Item* g = parentOf(sc, &it); if (!g) return true;
  if (g->type == ItemType::Cocoon) return sel.hasItem(it.id) || sel.hasItem(g->id);   // (a guide line on a cocoon; Shell modifiers tool — later)
  if (isServiceGroup(*g)) return true;
  QString mode = g->raw.value("guides").toString("auto"); if (mode.isEmpty()) mode = "auto";
  // (a crown's connectors: shown while working in any group, nothing selected too — the locks are attached to them;
  // off by the crown's group: its 'Connectors visible' unticked, or its curves hidden)
  if (it.type == ItemType::Curve && !it.conn.isEmpty()) {
    const bool connOn = !(g->raw.value("labels").toObject().value("conn").isBool() && !g->raw.value("labels").toObject().value("conn").toBool());
    return mode == "show" || (mode == "auto" && (connOn || ds.activeGroups.count(g->id)));
  }
  bool looseOn = !(g->raw.value("labels").toObject().value("loose").isBool() && !g->raw.value("labels").toObject().value("loose").toBool());
  if (mode == "auto" && it.type == ItemType::Curve && looseOn && ds.looseCurves.count(it.id)) return true;
  return mode == "show" || (mode == "auto" && ds.activeGroups.count(g->id));
}

bool curveShown(const Scene& sc, const DisplayState& ds, const Selection& sel, const Item& c) {
  if (c.ring >= 0) {                                         // a NURBS disc's ring
    const Item* d = sc.item(c.ring);
    return d && sc.isVisible(*d) && ds.showGuides && guidesShown(sc, ds, sel, *d);
  }
  return sc.isVisible(c) && ds.showCurves && c.anchors.size() >= 2 && guidesShown(sc, ds, sel, c) && !(c.junc && !sel.hasItem(c.id));
}

std::vector<ShownAnchor> anchorList(const Scene& sc, const DisplayState& ds, const Selection& sel, int weldCand) {
  static const float NORMAL[3] = {1, .82f, .29f}, SHARED[3] = {.23f, .84f, 1}, SEL[3] = {1, .55f, .12f}, LAST[3] = {1, .25f, .05f},
                     RING[3] = {.45f, .95f, .6f}, ATT[3] = {.75f, .5f, 1}, START[3] = {.35f, .95f, .4f}, WELD[3] = {1, .2f, 1};
  std::vector<ShownAnchor> out;
  if (!ds.showAnchors) return out;
  auto curves = sc.curves();
  std::unordered_map<int, int> count; std::unordered_set<int> starts, ends, ringA, seen;
  for (const Item* c : curves) if (!c->hasPath()) for (int a : c->anchors) count[a]++;
  for (const Item* c : curves) if (c->ring < 0 && !c->closed && c->anchors.size() > 1) { starts.insert(c->anchors.front()); ends.insert(c->anchors.back()); }
  std::vector<int> list;
  std::unordered_set<int> asel(sel.anchors.begin(), sel.anchors.end());
  for (const Item* c : curves) {
    if (!sc.isVisible(*c)) continue;
    if (c->ring >= 0) {                                      // disc points: only while that disc (or one of its points) is selected
      bool on = sel.hasItem(c->ring); for (int a : c->anchors) if (asel.count(a)) on = true;
      if (!ds.showGuides || !on) continue;
      for (int a : c->anchors) ringA.insert(a);
    } else if (!guidesShown(sc, ds, sel, *c)) {
      // curves of other groups (Auto): their anchors attached to a crown connector stay visible
      const Item* g = parentOf(sc, c);
      if ((g ? g->raw.value("guides").toString("auto") : QString("auto")) != "hide" && !ds.activeGroups.empty())
        for (int a : c->anchors) { const Anchor* an = sc.anchor(a); if (an && an->raw.contains("at") && seen.insert(a).second) list.push_back(a); }
      continue;
    }
    for (int a : c->anchors) if (seen.insert(a).second) list.push_back(a);
  }
  int last = sel.anchors.empty() ? -1 : sel.anchors.back();
  for (int a : list) {
    const Anchor* an = sc.anchor(a); if (!an) continue;
    bool dia = starts.count(a) > 0, end = !dia && ends.count(a) > 0;
    const float* col = ringA.count(a) ? RING : an->raw.contains("at") ? ATT : count[a] > 1 ? SHARED : dia ? START : NORMAL;
    float s = 9;
    if (asel.count(a)) { col = SEL; s = 11; }
    if (a == last) { col = LAST; s = 13; }
    if (a == weldCand) { col = WELD; s = 16; }
    out.push_back({a, {col[0], col[1], col[2]}, dia ? s * 1.35f : end ? s * 1.3f : s, dia ? 1 : end ? 2 : 0});
  }
  return out;
}

}  // namespace hm
