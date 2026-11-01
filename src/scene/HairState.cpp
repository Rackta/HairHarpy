#include "scene/HairState.h"
#include <QJsonObject>
#include <unordered_set>

namespace hm {

void HairState::load(const Scene& sc) {
  cocoons_.reset();
  curves = sampleAllCurves(sc);
  walls.clear();
  for (auto& w : buildWalls(sc, curves, true)) walls[w.surfaceId] = std::move(w);
  cocoons_ = std::make_unique<CocoonSystem>(sc, curves, walls);
  cocoons_->rebuildAll();
  draftWalls_.clear(); draftCocoons_.clear();
}

const std::map<int, CocoonResult>& HairState::cocoons() const {
  static const std::map<int, CocoonResult> none;
  return cocoons_ ? cocoons_->results() : none;
}

HairChanges HairState::apply(const Scene& sc, const std::set<int>& changed, bool draft) {
  HairChanges ch; ch.curves = changed;
  for (int id : changed) {
    const Item* c = sc.item(id);
    if (c && c->anchors.size() >= 2) curves[id] = sampleCurveFull(sc, *c); else curves.erase(id);
  }
  for (auto& [id, it] : sc.items) if (it.type == ItemType::Surface && (changed.count(it.a) || changed.count(it.b))) ch.walls.insert(id);
  if (!ch.walls.empty()) {
    for (int id : ch.walls) walls.erase(id);
    for (auto& w : buildWalls(sc, curves, true, &ch.walls, draft)) walls[w.surfaceId] = std::move(w);
    if (draft) draftWalls_.insert(ch.walls.begin(), ch.walls.end());
    else for (int id : ch.walls) draftWalls_.erase(id);
  }
  if (cocoons_) ch.cocoons = cocoons_->update(changed);
  // a disc's rings moved: the cocoon it is on (a scene disc — every cocoon) takes its new place
  if (cocoons_) {
    std::set<int> extra; bool all = false;
    for (int id : changed) if (const Item* c = sc.item(id); c && c->ring >= 0) if (const Item* g = sc.item(c->ring)) {
      const Item* p = g->parent >= 0 ? sc.item(g->parent) : nullptr;
      if (p && p->type == ItemType::Cocoon) extra.insert(p->id); else all = true;
    }
    if (all) for (auto& [id, it] : sc.items) if (it.type == ItemType::Cocoon) extra.insert(id);
    for (int k : ch.cocoons) extra.erase(k);
    if (!extra.empty()) { cocoons_->rebuild(extra); ch.cocoons.insert(extra.begin(), extra.end()); }
  }
  if (draft) draftCocoons_.insert(ch.cocoons.begin(), ch.cocoons.end());
  return ch;
}

HairChanges HairState::finishDraft(const Scene& sc) {
  HairChanges ch;
  if (!draftWalls_.empty()) {
    ch.walls = draftWalls_;
    for (int id : ch.walls) walls.erase(id);
    for (auto& w : buildWalls(sc, curves, true, &ch.walls, false)) walls[w.surfaceId] = std::move(w);
  }
  // (the HTML version leaves the rings cut on the draft walls until the next edit; here they follow the final walls)
  if (cocoons_ && !draftCocoons_.empty()) { cocoons_->rebuild(draftCocoons_); ch.cocoons = draftCocoons_; }
  draftWalls_.clear(); draftCocoons_.clear();
  return ch;
}

HairChanges HairState::rebuildWalls(const Scene& sc, const std::set<int>& ws) {
  HairChanges ch; ch.walls = ws;
  for (int id : ws) walls.erase(id);
  for (auto& w : buildWalls(sc, curves, true, &ch.walls, false)) walls[w.surfaceId] = std::move(w);
  if (cocoons_) {
    std::set<int> ks;
    for (auto& [id, it] : sc.items) if (it.type == ItemType::Cocoon) { int g = cocoons_->lockGroup(id); if (g >= 0) for (int w : ws) if (sc.isDescendant(w, g)) { ks.insert(id); break; } }
    if (!ks.empty()) { cocoons_->rebuild(ks); ch.cocoons = ks; }
  }
  return ch;
}

std::set<int> curvesTouching(const Scene& sc, const std::vector<int>& anchors) {
  std::unordered_set<int> set(anchors.begin(), anchors.end());
  auto cs = sc.curves();
  std::set<int> dirty;
  for (const Item* c : cs) for (int a : c->anchors) if (set.count(a)) { dirty.insert(c->id); break; }
  std::unordered_set<int> touched;
  for (int id : dirty) { const Item* c = sc.item(id); if (!c->hasPath()) touched.insert(c->anchors.begin(), c->anchors.end()); }
  for (const Item* c : cs) if (!c->hasPath()) for (int a : c->anchors) if (touched.count(a)) { dirty.insert(c->id); break; }
  std::unordered_set<int> onDirty;
  for (int id : dirty) { const Item* c = sc.item(id); if (!c->hasPath()) onDirty.insert(c->anchors.begin(), c->anchors.end()); }
  for (const Item* c : cs) if (c->hasPath()) for (int a : c->anchors) if (onDirty.count(a)) { dirty.insert(c->id); break; }
  return dirty;
}

int groupLogic(const Scene& sc, const Item& it) {
  for (const Item* p = it.parent >= 0 ? sc.item(it.parent) : nullptr; p; p = p->parent >= 0 ? sc.item(p->parent) : nullptr)
    if (p->type == ItemType::Group) return p->raw.value("logic").toInt(1);
  return sc.fileRaw.value("settings").toObject().value("rootLogic").toInt(2) == 2 ? 2 : 1;
}

std::vector<RemovedOverride> releaseFrozenAround(Scene& sc, const std::vector<int>& list) {
  std::unordered_set<int> moved(list.begin(), list.end()), rel;
  std::vector<Item*> nl;
  for (auto& [id, c] : sc.items) {
    bool segNet = !(c.junc || c.hasPath() || c.ring >= 0 || !c.conn.isEmpty() || c.closed || c.hasRib);
    if (c.type == ItemType::Curve && segNet && !c.ovr.empty() && groupLogic(sc, c) == 2) nl.push_back(&c);
  }
  for (Item* c : nl) {
    const auto& A = c->anchors; int n = int(A.size());
    for (int k = 0; k < n; k++) {
      if (!moved.count(A[k])) continue;
      bool allMoved = true;
      for (int j : {k - 1, k + 1}) if (j >= 0 && j < n && !moved.count(A[j])) allMoved = false;
      if (allMoved) continue;
      rel.insert(A[k]);
      for (int d : {-2, -1, 1, 2}) { int j = k + d; if (j >= 0 && j < n && !moved.count(A[j])) rel.insert(A[j]); }
    }
  }
  std::vector<RemovedOverride> out;
  for (Item* c : nl) for (int a : rel) { auto it = c->ovr.find(a); if (it != c->ovr.end()) { out.push_back({c->id, a, it->second}); c->ovr.erase(it); } }
  return out;
}

}  // namespace hm
