// UV Sets and UV Mats (Viewport.h ▦ UV Sets). A UV Set is a UV layout of its own over some of the mesh's polygons (the
// PolyMesh's uvSetNames / faceSets / setUV; a polygon can be in several — a head laid out twice); the polygon shows one of
// them (faceLive — its UV in cornerUV, where the UV Editor and the textures read it; the set's own copy is brought up to
// date — flushed — before a set is read). The UV Mats — the mesh's materials (faceMat into matNames; none — Default) —
// each use a set (raw "uvSets": [{ name, mats: [mat names; "" — Default] }]). Every edit is one undo step: the mesh written
// as its new source (the topology shared — it stays), the mats' sets into the raw.
#include "scene/Uv.h"
#include "scene/MeshMods.h"
#include "scene/Topo.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <QJsonDocument>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
int matOf(const PolyMesh& B, int f) { if (size_t(f) >= B.faceMat.size()) return -1; const int k = B.faceMat[size_t(f)]; return k >= 0 && size_t(k) < B.matNames.size() ? k : -1; }
QString matName(const PolyMesh& B, int k) { return k < 0 || size_t(k) >= B.matNames.size() ? QStringLiteral("Default") : B.matNames[size_t(k)]; }
std::set<QString> gone(const Item& it) { std::set<QString> s; for (const QJsonValue& x : it.raw.value("uvMatGone").toArray()) s.insert(x.toString()); return s; }
QString freeName(const QString& base, const std::set<QString>& taken) { if (!taken.count(base)) return base; for (int i = 2;; i++) { QString n = QString("%1 %2").arg(base).arg(i); if (!taken.count(n)) return n; } }
bool inSet(const PolyMesh& d, int f, int s) { return s >= 0 && size_t(f) < d.faceSets.size() && (d.faceSets[size_t(f)] >> s & 1u); }

// ---------- the layouts ----------
// (none yet: one set of everything, its UV the mesh's)
void ensureSets(PolyMesh& d) {
  if (topo::hasUvSets(d)) return;
  const int nf = d.faceCount(); d.uvSetNames = {topo::defaultUvSetName(d)}; d.faceSets.assign(size_t(nf), 1u); d.faceLive.assign(size_t(nf), 0);
  d.setUV.assign(1, d.cornerUV.size() == d.faceVerts.size() ? d.cornerUV : std::vector<std::array<float, 2>>(d.faceVerts.size(), {0, 0}));
}
// the shown UV back into the set each polygon shows
void flush(PolyMesh& d) {
  if (d.cornerUV.size() != d.faceVerts.size()) return;
  for (int f = 0; f < d.faceCount(); f++) { const int s = d.faceLive[size_t(f)]; if (s < 0 || size_t(s) >= d.setUV.size()) continue;
    for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) d.setUV[size_t(s)][size_t(k)] = d.cornerUV[size_t(k)]; }
}
// polygon f shows set s (its UV into cornerUV) → changed
bool showSet(PolyMesh& d, int f, int s) {
  if (!inSet(d, f, s) || d.cornerUV.size() != d.faceVerts.size()) return false;
  bool ch = d.faceLive[size_t(f)] != s; d.faceLive[size_t(f)] = int8_t(s);
  for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) if (d.cornerUV[size_t(k)] != d.setUV[size_t(s)][size_t(k)]) { d.cornerUV[size_t(k)] = d.setUV[size_t(s)][size_t(k)]; ch = true; }
  return ch;
}
// the sets put in a new order: perm[new] = old
void reorder(PolyMesh& d, std::vector<Viewport::UvSetL>& S, const std::vector<int>& perm) {
  std::vector<int> inv(perm.size()); for (size_t i = 0; i < perm.size(); i++) inv[size_t(perm[i])] = int(i);
  std::vector<QString> nn; std::vector<std::vector<std::array<float, 2>>> uu; std::vector<Viewport::UvSetL> SS;
  for (int o : perm) { nn.push_back(d.uvSetNames[size_t(o)]); uu.push_back(std::move(d.setUV[size_t(o)])); SS.push_back(S[size_t(o)]); }
  d.uvSetNames = nn; d.setUV = std::move(uu); S = SS;
  for (size_t f = 0; f < d.faceSets.size(); f++) { uint32_t m = 0; for (size_t o = 0; o < perm.size(); o++) if (d.faceSets[f] >> o & 1u) m |= 1u << inv[o]; d.faceSets[f] = m;
    if (d.faceLive[f] >= 0) d.faceLive[f] = int8_t(inv[size_t(d.faceLive[f])]); }
}
// set k gone: its polygons that showed it show their first other set (else keep their UV, no set); its mats to `heir`
void dropSet(PolyMesh& d, std::vector<Viewport::UvSetL>& S, int k, int heir) {
  for (int f = 0; f < d.faceCount(); f++) if (d.faceLive[size_t(f)] == k) { d.faceLive[size_t(f)] = -1; for (int s = 0; s < int(S.size()); s++) if (s != k && inSet(d, f, s)) { showSet(d, f, s); break; } }
  for (int m : S[size_t(k)].mats) S[size_t(heir)].mats.push_back(m);
  d.uvSetNames.erase(d.uvSetNames.begin() + k); d.setUV.erase(d.setUV.begin() + k); S.erase(S.begin() + k);
  for (size_t f = 0; f < d.faceSets.size(); f++) { const uint32_t m = d.faceSets[f], lo = m & ((1u << k) - 1u), hi = (m >> (k + 1)) << k; d.faceSets[f] = lo | hi;
    if (d.faceLive[f] > k) d.faceLive[f] = int8_t(d.faceLive[f] - 1); }
}
}  // namespace

// ---------- the lists ----------
std::vector<Viewport::UvMat> Viewport::uvMats(int m) const {
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr; if (!B) return {};
  const QByteArray key = QByteArray::number(quintptr(B)) + "|" + QByteArray::number(qulonglong(B->faceMat.size())) + "|" + QByteArray::number(qulonglong(B->faceSets.size())) + QJsonDocument(it->raw.value("uvMatGone").toArray()).toJson(QJsonDocument::Compact);   // (counted once per geometry)
  if (auto c = uvMatsC_.find(m); c != uvMatsC_.end() && c->second.first == key) return c->second.second;
  auto& slot = uvMatsC_[m]; slot.first = key; slot.second = [&] {
  std::vector<int> cnt(B->matNames.size(), 0); int def = 0;
  for (int f = 0; f < B->faceCount(); f++) { int k = matOf(*B, f); if (k < 0) def++; else cnt[size_t(k)]++; }
  const auto G = gone(*it); std::vector<UvMat> out;
  if (def || B->matNames.empty()) out.push_back({-1, "Default", def});
  for (size_t k = 0; k < B->matNames.size(); k++) if (cnt[k] || !G.count(B->matNames[k])) out.push_back({int(k), B->matNames[k], cnt[k]});
  return out;
  }(); return slot.second;
}

std::vector<Viewport::UvSetL> Viewport::uvSets(int m) const {
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr; if (!B) return {};
  const QByteArray key = QByteArray::number(quintptr(B)) + "|" + QByteArray::number(qulonglong(B->faceMat.size())) + "|" + QByteArray::number(qulonglong(B->faceSets.size())) + QJsonDocument(it->raw.value("uvSets").toArray()).toJson(QJsonDocument::Compact) + QJsonDocument(it->raw.value("uvMatGone").toArray()).toJson(QJsonDocument::Compact);
  if (auto c = uvSetsC_.find(m); c != uvSetsC_.end() && c->second.first == key) return c->second.second;
  auto& slot = uvSetsC_[m]; slot.first = key; slot.second = [&] {
  std::vector<UvSetL> out;
  if (topo::hasUvSets(*B)) for (size_t s = 0; s < B->uvSetNames.size(); s++) { UvSetL x; x.name = B->uvSetNames[s]; for (uint32_t fm : B->faceSets) x.faces += int(fm >> s & 1u); out.push_back(x); }
  else out.push_back({topo::defaultUvSetName(*B), {}, B->faceCount()});   // (a mesh with no sets: in its material's, as an imported model's are)
  // (the mats' sets, by name; a mat in none — the first set's)
  auto mats = uvMats(m); std::map<QString, int> byName; for (auto& x : mats) byName[x.idx < 0 ? QString() : x.name] = x.idx;
  std::set<int> placed;
  for (const QJsonValue& v : it->raw.value("uvSets").toArray()) {
    QJsonObject o = v.toObject(); auto at = std::find_if(out.begin(), out.end(), [&](const UvSetL& s) { return s.name == o.value("name").toString(); }); if (at == out.end()) continue;
    for (const QJsonValue& n : o.value("mats").toArray()) if (auto x = byName.find(n.toString()); x != byName.end() && placed.insert(x->second).second) at->mats.push_back(x->second);
  }
  // (a mat in none: the set its polygons show the most — an imported model's UV Set per material — else the first)
  for (auto& x : mats) if (!placed.count(x.idx)) { std::map<int, int> votes;
    if (topo::hasUvSets(*B)) for (int f = 0; f < B->faceCount(); f++) if (matOf(*B, f) == x.idx && B->faceLive[size_t(f)] >= 0 && size_t(B->faceLive[size_t(f)]) < out.size()) votes[B->faceLive[size_t(f)]]++;
    int best = 0, bn = 0; for (auto& [s, n] : votes) if (n > bn) { bn = n; best = s; }
    out[size_t(best)].mats.push_back(x.idx); }
  return out;
  }(); return slot.second;
}

int Viewport::uvSetOfMat(int m, int mat) const {
  auto S = uvSets(m); for (size_t i = 0; i < S.size(); i++) if (std::find(S[i].mats.begin(), S[i].mats.end(), mat) != S[i].mats.end()) return int(i);
  return -1;
}

std::vector<char> Viewport::uvHidden(const Item& it, const PolyMesh& B) const {
  std::vector<char> h = meshmods::hiddenFaces(it, B);
  if (uvPicked.id < 0) return h;
  if (uvPicked.mat != -2) { if (uvPicked.id != it.id) return h; h.resize(size_t(B.faceCount()), 0); for (int f = 0; f < B.faceCount(); f++) if (matOf(B, f) != uvPicked.mat) h[size_t(f)] = 1; return h; }
  if (!uvSetIso) return h;                                     // (a set: the rest hidden only while it is isolated — on every mesh shown, by the set's name: the face of Head out of Body_UVSet picked on Legs)
  const int s = uvWorkSet(it.id, B); if (s == -2) return h; h.resize(size_t(B.faceCount()), 0);
  if (s == -1) { std::fill(h.begin(), h.end(), char(1)); return h; }   // (the mesh hasn't that set: nothing of it in the set)
  if (topo::hasUvSets(B)) for (int f = 0; f < B.faceCount(); f++) if (!inSet(B, f, s)) h[size_t(f)] = 1;
  return h;
}
int Viewport::uvWorkSet(int id, const PolyMesh& B) const {
  if (uvPicked.id < 0 || uvPicked.mat != -2 || uvPicked.set < 0) return -2;
  if (uvPicked.id == id) { if (topo::hasUvSets(B) && size_t(uvPicked.set) >= B.uvSetNames.size()) return -2;   // (the set picked is gone — its making undone: no set worked in, not an empty editor)
    return topo::hasUvSets(B) ? uvPicked.set : 0; }
  const auto S0 = uvSets(uvPicked.id); if (size_t(uvPicked.set) >= S0.size()) return -2; const QString& want = S0[size_t(uvPicked.set)].name;
  if (!topo::hasUvSets(B)) { const auto S = uvSets(id); return !S.empty() && S[0].name == want ? 0 : -1; }
  for (size_t k = 0; k < B.uvSetNames.size(); k++) if (B.uvSetNames[k] == want) return int(k);
  return -1;
}

std::vector<int> Viewport::uvMatFaces(int m, const std::set<int>& mats) const {
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr; std::vector<int> o; if (!B) return o;
  for (int f = 0; f < B->faceCount(); f++) if (mats.count(matOf(*B, f))) o.push_back(f);
  return o;
}
std::vector<std::vector<int>> Viewport::uvMatGroups(int m) const {
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr; if (!B) return {};
  // (what Pack and the checks work on: a set worked in — its polygons only, shown or not; no set — each set's polygons apart (they lie in their own tiles: one never in the way of another, never an overlap with another), each UV Mat apart)
  const auto h = uvHidden(*it, *B); std::map<std::pair<int, int>, std::vector<int>> by; const bool sets = topo::hasUvSets(*B);
  const int ws = sets ? uvWorkSet(m, *B) : -2; if (ws == -1) return {};   // (the set worked in, by name — this mesh hasn't it: nothing of it packed or checked)
  const int ps = ws >= 0 ? ws : -1;
  for (int f = 0; f < B->faceCount(); f++) { if (!(h.empty() || size_t(f) >= h.size() || !h[size_t(f)])) continue; if (ps >= 0 && !inSet(*B, f, ps)) continue;
    by[{ps >= 0 || !sets || size_t(f) >= B->faceLive.size() ? 0 : B->faceLive[size_t(f)], matOf(*B, f)}].push_back(f); }
  std::vector<std::vector<int>> o; for (auto& [k, fs] : by) o.push_back(std::move(fs)); return o;
}
int Viewport::uvSelectMats(int m, const std::set<int>& mats) {
  auto fs = uvMatFaces(m, mats); if (fs.empty()) { ui::toast(tr("No polygons")); return 0; }
  if (topoMode != "poly") setTopoMode("poly");
  return tSelectFaces(m, fs);
}

// ---------- the edits ----------
namespace {
QJsonArray setsJson(const std::vector<Viewport::UvSetL>& S, const PolyMesh& B) {
  QJsonArray a; for (auto& s : S) { QJsonArray ms; for (int k : s.mats) ms.append(k < 0 ? QString() : matName(B, k)); a.append(QJsonObject{{"name", s.name}, {"mats", ms}}); }
  return a;
}
// fn on a copy of the mesh (its sets made, flushed) and the sets: written together — one undo step
QString uvMatsEdit(Viewport* vp, Scene* sc, int m, const std::function<QString(PolyMesh&, std::vector<Viewport::UvSetL>&, QJsonObject&)>& fn, bool needUV = false) {
  Item* it = sc->item(m); if (!it || !baseOf(*it)) return "!";
  
  auto B = baseSp(*it);
  if (needUV && B->cornerUV.size() != B->faceVerts.size()) { ui::toast(QObject::tr("This mesh has no UV yet — Unfold it first")); return "!"; }
  auto nb = std::make_shared<PolyMesh>(*B); PolyMesh& d = *nb;
  if (int(d.faceMat.size()) != d.faceCount()) d.faceMat.assign(size_t(d.faceCount()), -1);
  if (d.cornerUV.size() == d.faceVerts.size()) { ensureSets(d); flush(d); }
  auto S = vp->uvSets(m); QJsonObject raw = it->raw;
  QString msg = fn(d, S, raw);
  if (msg.startsWith('!')) { if (msg.size() > 1) ui::toast(msg.mid(1)); return msg; }
  for (size_t i = 0; i < S.size() && i < d.uvSetNames.size(); i++) d.uvSetNames[i] = S[i].name;
  const size_t n = d.matNames.size(); d.matMaps.resize(n); d.matColor.resize(n); d.matAlpha.resize(n, 0);   // (a new slot: no maps, no colour of its own)
  raw.insert("uvSets", setsJson(S, d)); it->raw = raw;
  vp->topoMeshEdited(m, nb, B);
  if (!msg.isEmpty()) ui::toast(msg);
  return msg;
}
}  // namespace

void Viewport::topoMeshEdited(int m, std::shared_ptr<PolyMesh> nb, const std::shared_ptr<PolyMesh>& old) {
  topo::shareTopo(nb, old); topoWrite(m, nb); uvStamp_++; uvRev_++;
  if (uvRenameBatch_) return;                                  // (a rename over several meshes: one commit at its end)
  scene_->revision++; staticRev_++; commit(true); emit topoState();
}

// ---------- the pick: what the UV Editor shows (and the polygons show the picked set's layout) ----------
namespace {
bool showAll(PolyMesh& d, int s, const std::vector<int>& fs) { bool ch = false; for (int f : fs) ch |= showSet(d, f, s); return ch; }
}  // namespace
void Viewport::uvPickSet(int m, int set) {
  if (uvPicked.id == m && uvPicked.set == set && uvPicked.mat == -2) { uvPicked = {}; uvChanged(); emit topoState(); return; }
  uvPicked = {m, set, -2}; uvFreePicked.clear();
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr;
  if (B && topo::hasUvSets(*B) && size_t(set) < B->uvSetNames.size()) {
    // (the set's layout shown on EVERY mesh that has a set of that name — not only the one it was picked on. Before, the other meshes' polygons of
    //  the set kept showing another set's layout: the checks found overlaps that were not there, and edits moved the wrong UV, 2026-11-10)
    const int h0 = histIdx_;                                     // (every mesh switched: one undo step)
    for (const auto& [id, oi] : scene_->items) { if (!oi.poly) continue; const PolyMesh* OB = baseOf(oi); if (!OB || !topo::hasUvSets(*OB)) continue;
      const int s = id == m ? set : uvWorkSet(id, *OB); if (s < 0) continue;
      std::vector<int> fs; for (int f = 0; f < OB->faceCount(); f++) if (inSet(*OB, f, s) && OB->faceLive[size_t(f)] != s) fs.push_back(f);
      if (!fs.empty()) uvMatsEdit(this, scene_, id, [&](PolyMesh& d, std::vector<UvSetL>&, QJsonObject&) -> QString { showAll(d, s, fs); return QString(); }); }
    histMerge(h0);
    ui::hud(tr("%1: its layout shown, only it in the UV Editor").arg(B->uvSetNames[size_t(set)]), 1200);
  }
  uvChanged(); emit topoState();
}
std::set<int> Viewport::uvSetsOfSel(int m) const {
  std::set<int> o; const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr; if (!B || topoMode == "object" || tsel_.id != m) return o;
  const auto fs = tSelFaces(m); const int n = int(uvSets(m).size());
  for (int f : fs) { if (f >= B->faceCount()) continue; if (!topo::hasUvSets(*B)) { o.insert(0); break; } for (int si = 0; si < n; si++) if (inSet(*B, f, si)) o.insert(si); }
  return o;
}
namespace { QString putIntoSet(PolyMesh& d, std::vector<Viewport::UvSetL>& S, int si, const std::vector<int>& fs); }
// (objects picked, no polygon picked — Object mode, or Polygon mode with objects picked: the UV Set buttons mean whole objects)
bool Viewport::uvObjScope() const {
  if (topoMode == "object") return true;
  const int m = topoMesh(); const bool polys = m >= 0 && ((tsel_.id == m && !tSelFaces(m).empty()) || !tselMeshes().empty());
  return !polys && !sel_.items.empty();
}
QString Viewport::uvSetFromShells(int set) {
  if (topoMode == "object" && set < 0) {                        // (Object mode: the picked objects go into the new set; none picked — an empty one, Add to fills it)
    bool any = false; for (int id : sel_.items) if (const Item* it = scene_->item(id); it && it->type == ItemType::IMesh && baseOf(*it) && baseOf(*it)->faceCount() && !it->raw.value("tref").toBool()) any = true;
    if (!any) return uvSetNewFree();
  }
  if (uvObjScope()) {                                 // (objects: all of each one's polygons — a new set under one name in each)
    std::vector<int> ms; for (int id : sel_.items) if (const Item* it = scene_->item(id); it && it->type == ItemType::IMesh && baseOf(*it) && baseOf(*it)->faceCount() && !it->raw.value("tref").toBool()) ms.push_back(id);   // (the picked ones — not every mesh shown)
    if (ms.empty()) { ui::toast(tr("Select objects (Object mode) — or the shells on the model (a polygon of each is enough)")); return "!"; }
    if (set >= 0) { const int m = topoMesh(); if (m < 0) return "!"; std::vector<int> all(size_t(baseOf(*scene_->item(m))->faceCount())); for (size_t f = 0; f < all.size(); f++) all[f] = int(f);
      return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString { if (size_t(set) >= S.size()) return "!"; return putIntoSet(d, S, set, all); }, false); }
    const auto names = uvSetNamesAll(); std::set<QString> taken(names.begin(), names.end());
    const QString nm = freeName(QString("UV Set %1").arg(names.size() + 1), taken);   // (one name for all: the project's one set)
    QString r; const int h0 = histIdx_; for (int m : ms) { std::vector<int> all(size_t(baseOf(*scene_->item(m))->faceCount())); for (size_t f = 0; f < all.size(); f++) all[f] = int(f);
      r = uvSetPut(m, nm, all, false); if (r.startsWith('!')) { histMerge(h0); return r; } }
    histMerge(h0);                                               // (every object's part: one undo step)
    return ms.size() > 1 ? tr("%1: %2 objects").arg(nm).arg(ms.size()) : r;
  }
  if (set < 0 && !eachBusy_ && tselMeshes().size() > 1) {       // (polygons picked on several meshes: one new set for them all, under one name)
    const auto all = uvSetNamesAll(); std::set<QString> taken(all.begin(), all.end());
    const QString nm = freeName(QString("UV Set %1").arg(all.size() + 1), taken);
    return tEachMesh([&]() -> QString { const int mm = topoMesh(); const auto fs = tSelFaces(mm); if (mm < 0 || fs.empty()) return "!"; uvOutlineShell(mm, fs); return uvSetPut(mm, nm, fs, false); });
  }
  const int m = topoMesh();
  if (set < 0 && (m < 0 || tsel_.id != m || tSelFaces(m).empty())) return uvSetNewFree();   // (nothing selected: an empty set that belongs to nothing — the polygons come later)
  if (m < 0 || tsel_.id != m || tSelFaces(m).empty()) { ui::toast(tr("Select polygons on the model — or objects (Object mode)")); return "!"; }
  return set < 0 ? uvSetNewFrom(m, tSelFaces(m)) : uvSetApply(m, set);   // (exactly the selected polygons — the selection is not widened to shells; the Shells button does that)
}

void Viewport::uvPickMat(int m, int mat) {
  if (uvPicked.id == m && uvPicked.mat == mat) { uvPicked = {}; uvChanged(); emit topoState(); return; }
  const int s = uvSetOfMat(m, mat); uvPicked = {m, s, mat};
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr;
  if (B && topo::hasUvSets(*B) && s >= 0) {                     // (its polygons show its set's layout)
    std::vector<int> fs; for (int f : uvMatFaces(m, {mat})) if (inSet(*B, f, s) && B->faceLive[size_t(f)] != s) fs.push_back(f);
    if (!fs.empty()) uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>&, QJsonObject&) -> QString { showAll(d, s, fs); return QString(); });
  }
  if (B) ui::hud(tr("%1: only it in the UV Editor").arg(matName(*B, mat)), 1200);
  uvChanged(); emit topoState();
}

namespace {
std::vector<int> selPolys(const Viewport* vp, int m) { return vp->topoMode != "object" && vp->topoSel().id == m ? vp->tSelFaces(m) : std::vector<int>{}; }
// the polygons fs into set si: its layout takes their UV as shown (those already in it: show its own), they show it; a mat
// wholly among them uses the set from then on, a part of a mat becomes a new mat of the set
QString putIntoSet(PolyMesh& d, std::vector<Viewport::UvSetL>& S, int si, const std::vector<int>& fs) {
  const bool uvOk = d.cornerUV.size() == d.faceVerts.size();   // (no UV yet: the polygons only join the set — they show it, so the UV an Unfold makes is its own)
  for (int f : fs) { if (!inSet(d, f, si)) { d.faceSets[size_t(f)] |= 1u << si; if (uvOk) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) d.setUV[size_t(si)][size_t(k)] = d.cornerUV[size_t(k)]; } if (uvOk) showSet(d, f, si); else d.faceLive[size_t(f)] = int8_t(si); }
  std::map<int, std::vector<int>> by; for (int f : fs) by[matOf(d, f)].push_back(f);
  std::vector<int> all(d.matNames.size() + 1, 0); for (int f = 0; f < d.faceCount(); f++) all[size_t(matOf(d, f) + 1)]++;
  std::set<QString> taken(d.matNames.begin(), d.matNames.end()); int made = 0;
  auto take = [&](int k) { for (auto& s : S) s.mats.erase(std::remove(s.mats.begin(), s.mats.end(), k), s.mats.end()); };
  for (auto& [k, list] : by) {
    if (int(list.size()) == all[size_t(k + 1)]) { take(k); S[size_t(si)].mats.push_back(k); continue; }
    // (a part of a mat: the mat stays as it is — no new material is made for the set; nothing appears that the user did not make)
  }
  (void)taken; (void)made;
  QString r = QObject::tr("%1 polygon%2 into %3").arg(fs.size()).arg(fs.size() == 1 ? "" : "s").arg(S[size_t(si)].name);
  if (made) r += QObject::tr(" · %1 part%2 of a mat became a new mat").arg(made).arg(made == 1 ? "" : "s");
  return r;
}
QString tooMany() { return QObject::tr("!%1 UV Sets at most — merge or delete one first").arg(tedit::kUvSets); }
}  // namespace

QString Viewport::uvSetNew(int m, const std::vector<int>& faces, const QString& name) {
  const Item* mi = scene_->item(m); const PolyMesh* mb = mi ? baseOf(*mi) : nullptr; if (!mb) return "!";
  const bool noUV = mb->cornerUV.size() != mb->faceVerts.size();   // (no UV yet: nothing to put into a set — an empty one, the polygons come after Unfold)
  (void)noUV;                                                      // (no UV yet: the polygons still join the set — they show it, the Unfold lays them out in it)
  const auto fs = faces.empty() ? selPolys(this, m) : faces;   // (none: an empty set — no polygon needed to make one)
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (int(S.size()) >= tedit::kUvSets) return tooMany();
    if (!topo::hasUvSets(d)) {                                  // (a mesh with no UV at all: its first set, empty, instead of the one of everything)
      const int nf = d.faceCount(); d.uvSetNames.clear(); d.setUV.clear(); d.faceSets.assign(size_t(nf), 0u); d.faceLive.assign(size_t(nf), -1); S.clear(); }
    std::set<QString> taken; for (auto& s : S) taken.insert(s.name); const auto every = uvSetNamesAll(); for (const QString& n : every) taken.insert(n);   // (a name no set of the project has)
    const QString nm = freeName(name.isEmpty() ? QString("UV Set %1").arg(every.size() + 1) : name, taken);
    S.push_back({nm, {}, 0}); d.uvSetNames.push_back(nm); d.setUV.push_back(std::vector<std::array<float, 2>>(d.faceVerts.size(), {0, 0}));
    uvPicked = {m, int(S.size()) - 1, -2};
    if (fs.empty()) return tr("%1: an empty UV Set").arg(nm);
    return tr("%1: ").arg(nm) + putIntoSet(d, S, int(S.size()) - 1, fs); }, false);   // (no UV needed: the polygons join the set, the Unfold lays them out in it)
}

// ---------- the project's sets: a set is the project's — a new empty one belongs to no mesh; polygons are put into it later (a mesh
// gets the set of that name when its first polygons go in), or shells moved into it from other sets ----
std::vector<QString> Viewport::uvFreeSets() const {
  std::vector<QString> o; for (const QJsonValue& v : scene_->fileRaw.value("uvSetsFree").toArray()) o.push_back(v.toString()); return o;
}
std::vector<QString> Viewport::uvSetNamesAll() const {
  std::vector<QString> o = uvFreeSets(); std::set<QString> seen(o.begin(), o.end());
  for (const auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh) if (const PolyMesh* B = baseOf(it)) {
    if (topo::hasUvSets(*B)) { for (const QString& n : B->uvSetNames) if (seen.insert(n).second) o.push_back(n); }
    else if (!it.headTag && !B->cornerUV.empty()) { const QString n = topo::defaultUvSetName(*B); if (seen.insert(n).second) o.push_back(n); } }   // (a mesh with none: in its material's — the list shows it as the UV Sets bar does)
  return o;
}
namespace {
// a set's project properties follow its name (to empty: dropped)
void moveProps(Scene& sc, const QString& from, const QString& to) {
  QJsonObject P = sc.fileRaw.value("uvSetProps").toObject(); if (!P.contains(from)) return;
  const QJsonValue v = P.value(from); P.remove(from); if (!to.isEmpty()) P.insert(to, v);
  if (P.isEmpty()) sc.fileRaw.remove("uvSetProps"); else sc.fileRaw.insert("uvSetProps", P);
}
void setFree(Scene& sc, const std::vector<QString>& names) { QJsonArray a; for (const QString& n : names) a.append(n); if (a.isEmpty()) sc.fileRaw.remove("uvSetsFree"); else sc.fileRaw.insert("uvSetsFree", a); }
}  // namespace
QString Viewport::uvSetNewFree(const QString& name) {
  const auto all = uvSetNamesAll(); std::set<QString> taken(all.begin(), all.end());
  const QString nm = freeName(name.trimmed().isEmpty() ? QString("UV Set %1").arg(all.size() + 1) : name.trimmed(), taken);
  auto f = uvFreeSets(); f.push_back(nm); setFree(*scene_, f);
  stepLabel_ = tr("New UV Set: %1").arg(nm); scene_->revision++; commit(false); emit topoState();
  const QString msg = tr("%1: an empty UV Set — it belongs to nothing yet; put polygons or shells into it (Add to)").arg(nm); ui::toast(msg); return msg;
}
// a new set of exactly these polygons: a name no set of the project has, then the polygons put in (no UV needed)
QString Viewport::uvSetNewFrom(int m, const std::vector<int>& faces) {
  const auto all = uvSetNamesAll(); std::set<QString> taken(all.begin(), all.end()); const int h0 = histIdx_;
  uvOutlineShell(m, faces);                                      // (polygons picked with no seams round them: they become a shell first)
  const QString r = uvSetPut(m, freeName(QString("UV Set %1").arg(all.size() + 1), taken), faces, false);
  histMerge(h0); return r;                                       // (the seams and the set: one undo step)
}
// polygons picked in the Polygon mode: their outline made seams (the border edges that are not seams yet), so they are a shell of
// their own — which then goes into the set. All polygons of the mesh picked: nothing to cut round
void Viewport::uvOutlineShell(int m, const std::vector<int>& faces) {
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr;
  if (!B || topoMode != "poly" || tsel_.id != m || faces.empty() || int(faces.size()) >= B->faceCount()) return;
  uvMarkSeams(true);
}
QString Viewport::uvSetFreeDelete(const QString& name) {
  auto f = uvFreeSets(); const auto at = std::find(f.begin(), f.end(), name); if (at == f.end()) return "!";
  f.erase(at); setFree(*scene_, f); moveProps(*scene_, name, QString()); if (uvFreePicked == name) uvFreePicked.clear(); stepLabel_ = tr("UV Set deleted: %1").arg(name); scene_->revision++; commit(false); emit topoState(); return tr("%1 deleted").arg(name);
}
QString Viewport::uvSetFreeRename(const QString& from, const QString& to) {
  const QString n = to.trimmed(); if (n.isEmpty() || n == from) return "!";
  for (const QString& x : uvSetNamesAll()) if (x == n) { ui::toast(tr("%1 — that name is taken").arg(n)); return "!"; }
  auto f = uvFreeSets(); const auto at = std::find(f.begin(), f.end(), from); if (at == f.end()) return "!";
  *at = n; setFree(*scene_, f); moveProps(*scene_, from, n); if (uvFreePicked == from) uvFreePicked = n; stepLabel_ = tr("UV Set renamed: %1").arg(n); scene_->revision++; commit(false); emit topoState(); return n;
}
// the faces into the set of that name (the mesh gets it when it has none); move — out of the mesh's other sets
QString Viewport::uvSetPut(int m, const QString& name, const std::vector<int>& faces, bool move) {
  if (faces.empty()) { ui::toast(tr("Select polygons first — they go into the set")); return "!"; }
  const QJsonValue free0 = scene_->fileRaw.value("uvSetsFree");
  { auto f = uvFreeSets(); f.erase(std::remove(f.begin(), f.end(), name), f.end()); setFree(*scene_, f); }   // (it holds polygons now: no longer the empty one)
  const QString r = uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (!topo::hasUvSets(d)) {                                  // (a mesh with no UV at all: the listed default set (its material's) is not a set — its first one is made here, none of everything)
      const int nf = d.faceCount(); d.uvSetNames.clear(); d.setUV.clear(); d.faceSets.assign(size_t(nf), 0u); d.faceLive.assign(size_t(nf), -1); S.clear(); }
    int si = -1; for (size_t i = 0; i < S.size(); i++) if (S[i].name == name) { si = int(i); break; }
    if (si < 0) {
      if (int(S.size()) >= tedit::kUvSets) return tooMany();
      S.push_back({name, {}, 0}); d.uvSetNames.push_back(name); d.setUV.push_back(std::vector<std::array<float, 2>>(d.faceVerts.size(), {0, 0})); si = int(S.size()) - 1; }
    std::vector<int> had(S.size(), 0); for (int f = 0; f < d.faceCount(); f++) for (size_t s = 0; s < S.size(); s++) if (inSet(d, f, int(s))) had[s] = 1;
    const QString msg = putIntoSet(d, S, si, faces);
    if (move) for (int f : faces) { for (int s = 0; s < int(S.size()); s++) if (s != si && inSet(d, f, s)) d.faceSets[size_t(f)] &= ~(1u << s); showSet(d, f, si); }
    if (move) {                                                 // (a set the move left with no polygon — it had some before — goes: the mesh joins the set, it does not keep an empty one beside it)
      for (int k = int(S.size()) - 1; k >= 0; k--) { if (k == si || had[size_t(k)] == 0) continue; int n = 0; for (int f = 0; f < d.faceCount(); f++) if (inSet(d, f, k)) { n++; break; }
        if (n == 0 && S.size() > 1) { dropSet(d, S, k, si > k ? si - 1 : si); if (k < si) si--; } }
      uvPicked = {m, si, -2}; }
    uvPicked = {m, si, -2}; uvFreePicked.clear();
    return move ? tr("%1 — moved").arg(msg) : msg; }, false);   // (no UV needed: the polygons join the set, the Unfold lays them out in it)
  if (r.startsWith('!')) { if (free0.isArray()) scene_->fileRaw.insert("uvSetsFree", free0); else scene_->fileRaw.remove("uvSetsFree"); }
  return r;
}
// the selected polygons (Polygon mode) that lie in a set other than `name`
Viewport::SetClashInfo Viewport::uvSetClash(const QString& name) const {
  SetClashInfo o; if (topoMode != "poly") return o;
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; const PolyMesh* B = it ? baseOf(*it) : nullptr;
  if (!B || tsel_.id != m || !topo::hasUvSets(*B)) return o;
  o.mesh = m; std::set<QString> names;
  for (int f : tSelFaces(m)) { if (f >= B->faceCount()) continue; bool hit = false;
    for (size_t s = 0; s < B->uvSetNames.size(); s++) if (B->uvSetNames[s] != name && inSet(*B, f, int(s))) { hit = true; names.insert(B->uvSetNames[s]); }
    o.n += hit; }
  for (const QString& n : names) o.sets << n;
  return o;
}
// what is picked into the set: the picked objects (all of their polygons), or the shells of the selected polygons.
// Polygons with no UV (a mesh that has none) are cut into a shell and unfolded in the set's texture; ready UV is only moved / copied
QString Viewport::uvSetPutPicked(const QString& name, bool move, SetClash mode) {
  if (uvObjScope()) {
    QString r; int n = 0; const int h0 = histIdx_;
    for (int id : std::vector<int>(sel_.items)) if (const Item* it = scene_->item(id); it && it->type == ItemType::IMesh && baseOf(*it) && baseOf(*it)->faceCount() && !it->raw.value("tref").toBool()) {
      std::vector<int> all(size_t(baseOf(*it)->faceCount())); for (size_t f = 0; f < all.size(); f++) all[f] = int(f);
      r = uvSetPut(id, name, all, move); n++; if (r.startsWith('!')) { histMerge(h0); return r; } }
    histMerge(h0);                                               // (every object's part: one undo step)
    if (!n) { ui::toast(tr("Select objects (Object mode) — or the shells on the model (a polygon of each is enough)")); return "!"; }
    return r;
  }
  if (!eachBusy_ && tselMeshes().size() > 1) return tEachMesh([&]() -> QString { return uvSetPutPicked(name, move, mode); });   // (several meshes with polygons picked: each puts its own)
  const int m = topoMesh(); if (m < 0 || tsel_.id != m || tSelFaces(m).empty()) { ui::toast(tr("Select the shells on the model (a polygon of each is enough) — or objects (Object mode)")); return "!"; }
  auto fs = tSelFaces(m);
  const Item* mi = scene_->item(m); const PolyMesh* mb = mi ? baseOf(*mi) : nullptr;
  const bool noUV = mb && mb->cornerUV.size() != mb->faceVerts.size();   // (no UV at all: the area is unfolded by this very Add)
  std::vector<int> clash;                                         // (the polygons already in another set)
  if (mb && topo::hasUvSets(*mb)) for (int f : fs) { if (f >= mb->faceCount()) continue;
    for (size_t s = 0; s < mb->uvSetNames.size(); s++) if (mb->uvSetNames[s] != name && inSet(*mb, f, int(s))) { clash.push_back(f); break; } }
  if (mode == SetClash::Exclude && !clash.empty()) {
    std::set<int> bad(clash.begin(), clash.end()); fs.erase(std::remove_if(fs.begin(), fs.end(), [&](int f) { return bad.count(f) > 0; }), fs.end()); clash.clear();
    if (fs.empty()) { ui::toast(tr("Every selected polygon is in another set already — nothing to add")); return "!"; } }
  const bool mv = move || mode == SetClash::MoveNew;
  const std::vector<int> picked = fs; const int h0 = histIdx_;   // (the seams, the set and the unfold: one undo step)
  if (fs.size() != tSelFaces(m).size() && topoMode == "poly") {   // (some left out (Exclude): the outline is cut round the polygons that go in)
    const TSel s0 = tsel_; tsel_.f = std::set<int>(fs.begin(), fs.end()); uvOutlineShell(m, fs); tsel_ = s0; }
  else uvOutlineShell(m, fs);                                     // (an area with no seams round it becomes a shell before it goes in)
  const QString r = uvSetPut(m, name, fs, mv);
  if (r.startsWith('!')) { histMerge(h0); return r; }
  std::vector<int> unf;                                           // (what needs a new unwrap)
  if (mode == SetClash::MoveNew || mode == SetClash::CopyNew) unf = clash;   // (a mesh with no UV yet: the polygons only join the set — no Unfold of its own; it is the user's to press)
  if (unf.empty()) { histMerge(h0); return r; }
  tSelectFaces(m, unf);                                           // (the unfold works on the selection; the shells of the area, along the outline just cut)
  const QString u = uvUnfold(false);
  tSelectFaces(m, picked);
  histMerge(h0);
  return u.startsWith('!') ? r : r + " · " + u;
}
QString Viewport::uvSetApply(int m, int set) {
  const auto fs = selPolys(this, m); if (fs.empty()) { ui::toast(tr("Select polygons first — they go into the set's layout")); return "!"; }
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(set) >= S.size()) return "!"; return putIntoSet(d, S, set, fs); }, false);
}
QString Viewport::uvSetTakeOut(int m, int set) {
  const auto fs = selPolys(this, m); if (fs.empty()) { ui::toast(tr("Select the polygons to take out of the set")); return "!"; }
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(set) >= S.size()) return "!"; int n = 0;
    for (int f : fs) if (inSet(d, f, set)) { d.faceSets[size_t(f)] &= ~(1u << set); n++;
      if (d.faceLive[size_t(f)] == set) { d.faceLive[size_t(f)] = -1; for (int s = 0; s < int(S.size()); s++) if (inSet(d, f, s)) { showSet(d, f, s); break; } } }
    if (!n) return tr("!None of them is in %1").arg(S[size_t(set)].name);
    return tr("%1 polygon%2 out of %3").arg(n).arg(n == 1 ? "" : "s").arg(S[size_t(set)].name); }, true);
}
QString Viewport::uvSetRename(int m, int set, const QString& name) {
  const QString n = name.trimmed(); if (n.isEmpty()) return "!";
  QString old; { const auto S0 = uvSets(m); if (size_t(set) >= S0.size()) return "!"; old = S0[size_t(set)].name; }
  const bool prev = uvRenameBatch_; uvRenameBatch_ = true;       // (a set is the project's — one name over the meshes: renamed in every mesh that has it, as one undo step)
  QString r = uvMatsEdit(this, scene_, m, [&](PolyMesh&, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(set) >= S.size() || S[size_t(set)].name == n) return "!";
    for (auto& s : S) if (s.name == n) return tr("!%1 — that name is taken").arg(n);
    moveProps(*scene_, S[size_t(set)].name, n); S[size_t(set)].name = n; return QString(); });
  int others = 0;
  if (!r.startsWith('!')) for (const auto& [id, it] : scene_->items) { if (id == m || it.type != ItemType::IMesh) continue; const auto So = uvSets(id); int at = -1; bool clash = false;
      for (size_t i = 0; i < So.size(); i++) { if (So[i].name == old) at = int(i); if (So[i].name == n) clash = true; }
      if (at < 0 || clash) continue;
      const QString r2 = uvMatsEdit(this, scene_, id, [&](PolyMesh&, std::vector<UvSetL>& S, QJsonObject&) -> QString { if (size_t(at) >= S.size() || S[size_t(at)].name != old) return "!"; S[size_t(at)].name = n; return QString(); });
      if (!r2.startsWith('!')) others++; }
  uvRenameBatch_ = prev;
  if (!r.startsWith('!')) { scene_->revision++; staticRev_++; commit(true); emit topoState(); if (others) ui::toast(tr("UV Set renamed: %1 — in %2 more mesh%3 too").arg(n).arg(others).arg(others == 1 ? "" : "es")); }
  return r;
}
QString Viewport::uvSetDelete(int m, int set) {
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (S.size() < 2) return tr("!The last UV Set stays — every mat uses one"); if (size_t(set) >= S.size()) return "!";
    const QString nm = S[size_t(set)].name; const int heir = set == 0 ? 1 : 0; const QString hn = S[size_t(heir)].name;
    if (topo::hasUvSets(d)) dropSet(d, S, set, heir); else { for (int k : S[size_t(set)].mats) S[size_t(heir)].mats.push_back(k); S.erase(S.begin() + set); }
    if (uvPicked.id == m) uvPicked = {};
    return tr("%1 deleted — its mats use %2").arg(nm, hn); });
}
QString Viewport::uvSetDuplicate(int m, int set) {
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(set) >= S.size()) return "!"; if (int(S.size()) >= tedit::kUvSets) return tooMany();
    std::set<QString> taken; for (auto& s : S) taken.insert(s.name);
    const QString nm = freeName(S[size_t(set)].name + " copy", taken); const int k = int(S.size());
    S.push_back({nm, {}, 0}); d.uvSetNames.push_back(nm); d.setUV.push_back(d.setUV[size_t(set)]);
    for (auto& fm : d.faceSets) if (fm >> set & 1u) fm |= 1u << k;
    std::vector<int> perm; for (int i = 0; i < k; i++) { perm.push_back(i); if (i == set) perm.push_back(k); } reorder(d, S, perm);   // (right under it)
    for (int f = 0; f < d.faceCount(); f++) showSet(d, f, set + 1);   // (picked: its polygons show it)
    uvPicked = {m, set + 1, -2};
    return tr("%1: a layout of its own (the same UV for now)").arg(nm); }, true);
}
QString Viewport::uvSetMove(int m, int from, int to) {
  if (from == to) return "!";
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(from) >= S.size() || size_t(to) >= S.size()) return "!";
    std::vector<int> perm; for (int i = 0; i < int(S.size()); i++) if (i != from) perm.push_back(i); perm.insert(perm.begin() + to, from);
    if (topo::hasUvSets(d)) reorder(d, S, perm); else { std::vector<UvSetL> SS; for (int o : perm) SS.push_back(S[size_t(o)]); S = SS; }
    if (uvPicked.id == m) uvPicked = {};
    return QString(); });
}
// merge: one layout over both sets' polygons, in `into`'s place; where both lay a polygon out — the first one's UV (by
// the list, or `other` when otherFirst — the one dragged onto the other with Alt); the name — the winner's; the mats of both
QString Viewport::uvSetMerge(int m, int into, int other, bool otherFirst) {
  if (into == other) return "!";
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(into) >= S.size() || size_t(other) >= S.size()) return "!";
    const int win = otherFirst ? other : std::min(into, other), lose = win == into ? other : into;
    const QString wn = S[size_t(win)].name, ln = S[size_t(lose)].name;
    auto L = d.setUV[size_t(into)];
    for (int f = 0; f < d.faceCount(); f++) { const bool a = inSet(d, f, win), b = inSet(d, f, lose); if (!a && !b) continue;
      const int src = a ? win : lose; for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) L[size_t(k)] = d.setUV[size_t(src)][size_t(k)];
      d.faceSets[size_t(f)] |= 1u << into; }
    d.setUV[size_t(into)] = std::move(L);
    for (int f = 0; f < d.faceCount(); f++) if (d.faceLive[size_t(f)] == into || d.faceLive[size_t(f)] == other) { d.faceLive[size_t(f)] = -1; showSet(d, f, into); }
    S[size_t(into)].name = wn;
    dropSet(d, S, other, into);
    uvPicked = {m, other < into ? into - 1 : into, -2};
    return tr("%1 and %2 merged — where both lay a polygon out, %3's UV").arg(wn, ln, wn); }, true);
}

QString Viewport::uvMatNew(int m) {
  const auto fs = selPolys(this, m);
  const int si = uvPicked.id == m && uvPicked.set >= 0 ? uvPicked.set : 0;
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject& raw) -> QString {
    std::set<QString> taken(d.matNames.begin(), d.matNames.end());
    const QString nm = freeName(QString("Mat %1").arg(d.matNames.size() + 1), taken);
    d.matNames.push_back(nm); const int k = int(d.matNames.size()) - 1;
    for (int f : fs) d.faceMat[size_t(f)] = k;
    const int s = std::min<int>(si, int(S.size()) - 1); S[size_t(s)].mats.push_back(k);
    if (topo::hasUvSets(d)) for (int f : fs) showSet(d, f, s);
    QJsonArray G = raw.value("uvMatGone").toArray(); for (int i = G.size() - 1; i >= 0; i--) if (G[i].toString() == nm) G.removeAt(i); raw.insert("uvMatGone", G);
    uvPicked = {m, s, k};
    return fs.empty() ? tr("%1 — select polygons and Apply it").arg(nm) : tr("%1: %2 polygon%3").arg(nm).arg(fs.size()).arg(fs.size() == 1 ? "" : "s"); });
}
QString Viewport::uvMatApply(int m, int mat) {
  const auto fs = selPolys(this, m); if (fs.empty()) { ui::toast(tr("Select polygons first — they get the mat")); return "!"; }
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    for (int f : fs) d.faceMat[size_t(f)] = mat;
    if (topo::hasUvSets(d)) { int s = 0; for (size_t i = 0; i < S.size(); i++) if (std::find(S[i].mats.begin(), S[i].mats.end(), mat) != S[i].mats.end()) s = int(i); for (int f : fs) showSet(d, f, s); }
    return tr("%1: %2 polygon%3").arg(matName(d, mat)).arg(fs.size()).arg(fs.size() == 1 ? "" : "s"); });
}
// ---------- the materials library ----------
std::vector<Viewport::MatDef> Viewport::matLibrary() const {
  std::vector<MatDef> out; std::map<QString, size_t> at;
  for (const auto& [id, it] : scene_->items) {
    if (it.type != ItemType::IMesh) continue; const PolyMesh* B = baseOf(it); if (!B) continue;
    for (size_t k = 0; k < B->matNames.size(); k++) {
      const QString& n = B->matNames[k]; auto f = at.find(n);
      if (f == at.end()) { MatDef d; d.name = n; if (k < B->matColor.size()) d.color = B->matColor[k]; if (k < B->matAlpha.size()) d.alpha = B->matAlpha[k]; if (k < B->matMaps.size()) d.maps = B->matMaps[k];
        at[n] = out.size(); out.push_back(std::move(d)); f = at.find(n); }
      for (int mk : B->faceMat) if (mk == int(k)) out[f->second].faces++;
    }
  }
  return out;
}
QString Viewport::matAssign(int m, const std::vector<int>& faces, const MatDef& def) {
  const Item* it = scene_->item(m); if (!it || it->type != ItemType::IMesh || !baseOf(*it)) return "!";
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject& raw) -> QString {
    int k = -1; for (size_t i = 0; i < d.matNames.size(); i++) if (d.matNames[i] == def.name) { k = int(i); break; }
    if (k < 0) {                                                // (the mesh has not got it yet: its slot — the file's colour and maps with it)
      d.matNames.push_back(def.name); k = int(d.matNames.size()) - 1; const size_t n = d.matNames.size();
      d.matMaps.resize(n); d.matColor.resize(n); d.matAlpha.resize(n, 0); d.matMaps[size_t(k)] = def.maps; d.matColor[size_t(k)] = def.color; d.matAlpha[size_t(k)] = def.alpha;
      const int s = std::min<int>(uvPicked.id == m && uvPicked.set >= 0 ? uvPicked.set : 0, int(S.size()) - 1); if (s >= 0) S[size_t(s)].mats.push_back(k);
      QJsonArray G = raw.value("uvMatGone").toArray(); for (int i = G.size() - 1; i >= 0; i--) if (G[i].toString() == def.name) G.removeAt(i); raw.insert("uvMatGone", G);
    }
    std::vector<int> fs = faces; if (fs.empty()) { fs.resize(size_t(d.faceCount())); for (size_t f = 0; f < fs.size(); f++) fs[f] = int(f); }
    for (int f : fs) d.faceMat[size_t(f)] = k;
    if (topo::hasUvSets(d)) { int s = 0; for (size_t i = 0; i < S.size(); i++) if (std::find(S[i].mats.begin(), S[i].mats.end(), k) != S[i].mats.end()) s = int(i); for (int f : fs) showSet(d, f, s); }
    return tr("%1: %2 polygon%3").arg(def.name).arg(fs.size()).arg(fs.size() == 1 ? "" : "s"); });
}
// a material let go at a screen point: over the UV Editor — the shells picked there get it; over a 3D view — the object under it
QString Viewport::matDrop(const QPoint& global, const MatDef& def) {
  const QPoint w = mapFromGlobal(global); int pane = -1;
  for (int i : shownPanes()) if (paneRect(i).contains(w)) { pane = i; break; }
  if (pane < 0) return "!";
  if (pane != act_) activatePane(pane);
  if (cam_.view == "uv") {
    const int m = topoMesh();
    if (m < 0 || tsel_.id != m || tSelFaces(m).empty()) { ui::toast(tr("Pick the shells in the UV Editor first — the material goes onto them")); return "!"; }
    if (uvSelectShells() == "!") return "!";                    // (the whole shells of what is picked)
    return matAssign(m, tSelFaces(m), def);
  }
  const auto hit = pickSurface(fromGlobal(global));
  const Item* it = hit ? scene_->item(hit->itemId) : nullptr;
  if (!it || it->type != ItemType::IMesh) { ui::toast(tr("Drop the material onto an object")); return "!"; }
  return matAssign(hit->itemId, {}, def);
}
QString Viewport::uvMatRename(int m, int mat, const QString& name) {
  const QString n = name.trimmed(); if (n.isEmpty()) return "!";
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject& raw) -> QString {
    if (n == matName(d, mat)) return "!";
    for (auto& x : d.matNames) if (x == n) return tr("!%1 — that name is taken").arg(n);
    if (mat < 0) {                                              // (Default named: a mat of its own, its polygons in it)
      d.matNames.push_back(n); const int k = int(d.matNames.size()) - 1;
      for (int f = 0; f < d.faceCount(); f++) if (matOf(d, f) < 0) d.faceMat[size_t(f)] = k;
      for (auto& s : S) for (int& x : s.mats) if (x == -1) x = k;
      return QString(); }
    const QString old = d.matNames[size_t(mat)]; d.matNames[size_t(mat)] = n;
    QJsonObject L = raw.value("look").toObject(); QJsonArray LS = L.value("sets").toArray();   // (the textures' set of that name follows)
    for (int i = 0; i < LS.size(); i++) { QJsonObject o = LS[i].toObject(); if (o.value("name").toString() == old) { o.insert("name", n); LS[i] = o; } }
    if (!LS.isEmpty()) { L.insert("sets", LS); raw.insert("look", L); }
    return QString(); });
}
QString Viewport::uvMatDelete(int m, int mat) {
  if (mat < 0) { ui::toast(tr("Default stays — the polygons with no mat")); return "!"; }
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject& raw) -> QString {
    int n = 0; for (int f = 0; f < d.faceCount(); f++) if (matOf(d, f) == mat) { d.faceMat[size_t(f)] = -1; n++; }
    const int si = [&] { for (size_t i = 0; i < S.size(); i++) if (std::find(S[i].mats.begin(), S[i].mats.end(), mat) != S[i].mats.end()) return int(i); return 0; }();
    for (auto& s : S) s.mats.erase(std::remove(s.mats.begin(), s.mats.end(), mat), s.mats.end());
    bool any = false; for (auto& s : S) any |= std::find(s.mats.begin(), s.mats.end(), -1) != s.mats.end(); if (n && !any) S[size_t(si)].mats.push_back(-1);
    QJsonArray G = raw.value("uvMatGone").toArray(); G.append(d.matNames[size_t(mat)]); raw.insert("uvMatGone", G);
    if (uvPicked.id == m && uvPicked.mat == mat) uvPicked = {};
    return tr("%1 deleted — %2 polygon%3 to Default").arg(d.matNames[size_t(mat)]).arg(n).arg(n == 1 ? "" : "s"); });
}
// Alt + a mat dragged onto another: the other's polygons get the dragged one (it wins — its name, its textures, its set)
QString Viewport::uvMatMerge(int m, int from, int into) {
  if (from == into) return "!";
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject& raw) -> QString {
    int n = 0; for (int f = 0; f < d.faceCount(); f++) if (matOf(d, f) == into) { d.faceMat[size_t(f)] = from; n++; }
    const QString fn = matName(d, from), in = matName(d, into);
    for (auto& s : S) s.mats.erase(std::remove(s.mats.begin(), s.mats.end(), into), s.mats.end());
    if (into >= 0) { QJsonArray G = raw.value("uvMatGone").toArray(); G.append(in); raw.insert("uvMatGone", G); }
    int s = 0; for (size_t i = 0; i < S.size(); i++) if (std::find(S[i].mats.begin(), S[i].mats.end(), from) != S[i].mats.end()) s = int(i);
    if (topo::hasUvSets(d)) for (int f = 0; f < d.faceCount(); f++) if (matOf(d, f) == from) showSet(d, f, s);
    uvPicked = {m, s, from};
    return tr("%1 merged into %2: %3 polygon%4").arg(in, fn).arg(n).arg(n == 1 ? "" : "s"); });
}
QString Viewport::uvMatToSet(int m, int mat, int set) {
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(set) >= S.size()) return "!";
    for (auto& s : S) s.mats.erase(std::remove(s.mats.begin(), s.mats.end(), mat), s.mats.end());
    S[size_t(set)].mats.push_back(mat); if (uvPicked.id == m && uvPicked.mat == mat) uvPicked.set = set;
    if (topo::hasUvSets(d)) for (int f = 0; f < d.faceCount(); f++) if (matOf(d, f) == mat) showSet(d, f, set);
    return tr("%1 uses %2").arg(matName(d, mat), S[size_t(set)].name); });
}

// ---------- the texture a set is laid out for ----------
namespace {
Viewport::UvTex texFrom(const QJsonObject& o) {
  Viewport::UvTex t;
  t.w = std::clamp(o.value("w").toInt(t.w), 16, 16384); t.h = std::clamp(o.value("h").toInt(t.h), 16, 16384); t.pad = std::clamp(o.value("pad").toDouble(t.pad), 0.0, 256.0); t.td = std::max(0.0, o.value("td").toDouble(0));
  t.color = o.value("color").toString(); t.tex = o.value("tex").toString();
  return t;
}
}  // namespace
// a set's properties are the project's (raw "uvSetProps": { set name: { w, h, pad, td, color, tex } }), whatever mesh holds its
// layout; a set with none there (an older project) reads the mesh's own "uvTex"
Viewport::UvTex Viewport::uvSetProps(const QString& name) const {
  const QJsonObject P = scene_->fileRaw.value("uvSetProps").toObject();
  // (an older project: the set's numbers kept in a mesh's own "uvTex" under the set's name — read from whichever mesh has them, the lowest id first:
  //  the set then reads the same with any mesh active, not 4096 on the mesh that kept them and the default on the others, 2026-11-12)
  if (!P.contains(name) && !name.isEmpty()) for (const auto& [id, it] : scene_->items) { const QJsonObject all = it.raw.value("uvTex").toObject(); if (all.contains(name)) return texFrom(all.value(name).toObject()); }
  // (a set with no numbers of its own anywhere — made before sets had them: those of the meshes laid out in it, the biggest texture of them. Each
  //  mesh read its own before: one set, 4096 with one mesh active and 2048 with another)
  if (!P.contains(name) && !name.isEmpty()) { UvTex best; bool any = false;
    for (const auto& [id, it] : scene_->items) { if (it.type != ItemType::IMesh || !it.poly) continue; const PolyMesh* B = baseOf(it); if (!B || !topo::hasUvSets(*B) || std::find(B->uvSetNames.begin(), B->uvSetNames.end(), name) == B->uvSetNames.end()) continue;
      const QJsonObject all = it.raw.value("uvTex").toObject(); if (!all.contains("")) continue; const UvTex t = texFrom(all.value("").toObject()); if (!any || double(t.w) * t.h > double(best.w) * best.h) { best = t; any = true; } }
    if (any) { best.color.clear(); return best; } }
  return texFrom(P.value(name).toObject());
}
// the set whose properties a mesh works by: the one picked on it; else the set picked on another mesh, where this mesh has a set of that
// name (several meshes laid out in one set — the texture is the set's, whichever mesh is active: before, Cloth read 4096 with the head
// active and 2048 with the cape, 2026-11-12); else its only set. Empty: none — the mesh's own numbers
QString Viewport::uvWorkSet(int m) const {
  const auto S = uvSets(m);
  if (uvPicked.id == m) { if (uvPicked.mat == -2 && uvPicked.set >= 0 && size_t(uvPicked.set) < S.size()) return S[size_t(uvPicked.set)].name; }
  else if (uvPicked.mat == -2 && uvPicked.set >= 0 && scene_->item(uvPicked.id)) { const auto O = uvSets(uvPicked.id);
    if (size_t(uvPicked.set) < O.size()) for (const auto& s : S) if (s.name == O[size_t(uvPicked.set)].name) return s.name; }
  const Item* it = scene_->item(m); return S.size() == 1 && it && it->poly && topo::hasUvSets(*baseOf(*it)) ? S[0].name : QString();   // (its only set — a real one, not a mesh with no sets)
}
Viewport::UvTex Viewport::uvTex(int m, int set) const {
  const bool picked = set == -2; const Item* it = scene_->item(m);
  QString nm;                                                    // (the set the numbers are for: the free one lit, or the picked one's name)
  if (picked && !uvFreePicked.isEmpty() && !(uvPicked.id == m && uvPicked.mat == -2 && uvPicked.set >= 0)) nm = uvFreePicked;
  if (!it && nm.isEmpty()) return UvTex{};
  if (it && nm.isEmpty()) {
    if (picked) nm = uvWorkSet(m);
    else if (set >= 0) { const auto S = uvSets(m); if (size_t(set) < S.size()) nm = S[size_t(set)].name; }
  }
  if (!nm.isEmpty() && scene_->fileRaw.value("uvSetProps").toObject().contains(nm)) return uvSetProps(nm);
  if (!nm.isEmpty()) for (const auto& [id, oi] : scene_->items) { if (oi.raw.value("uvTex").toObject().contains(nm)) return uvSetProps(nm);   /* (an older project's: from the mesh that kept them) */
    if (oi.type == ItemType::IMesh && oi.poly) if (const PolyMesh* B = baseOf(oi); B && topo::hasUvSets(*B) && std::find(B->uvSetNames.begin(), B->uvSetNames.end(), nm) != B->uvSetNames.end()) return uvSetProps(nm); }   /* (a real set: its numbers are the set's, never one mesh's) */
  if (!it) return UvTex{};
  const QJsonObject all = it->raw.value("uvTex").toObject(); QJsonObject o = all.value("").toObject();   // (the mesh's own; a set's over it)
  if (!nm.isEmpty() && all.contains(nm)) o = all.value(nm).toObject();
  return texFrom(o);
}
uv::PackOpts Viewport::uvPackOpts(int m) const { const UvTex t = uvTex(m); uv::PackOpts o; o.w = t.w; o.h = t.h; o.pad = t.pad; o.rotate = uvo.rotate; o.freeTurn = uvo.packFree; o.equalize = uvo.packEqual; o.final = uvo.packFinal; o.passes = uvo.packFinal ? 6 : 1; return o; }   // (Quick: one try; Final: the orders, the ways of placing and the fine grid)
void Viewport::setUvSetSize(const QString& name, int w, int h) {
  if (name.isEmpty() || w <= 0 || h <= 0) return; const UvTex t = uvSetProps(name);
  QJsonObject P = scene_->fileRaw.value("uvSetProps").toObject(); P.insert(name, QJsonObject{{"w", w}, {"h", h}, {"pad", t.pad}, {"td", t.td}, {"color", t.color}, {"tex", t.tex}}); scene_->fileRaw.insert("uvSetProps", P);
  stepLabel_ = tr("UV set %1: %2 × %3").arg(name).arg(w).arg(h); scene_->revision++; commit(false); uvChanged(); emit topoState();
}
QString Viewport::uvSetTex(int m, const UvTex& t) {
  Item* it = scene_->item(m);
  const bool lit = !uvFreePicked.isEmpty() && !(uvPicked.id == m && uvPicked.mat == -2 && uvPicked.set >= 0);   // (an empty set lit: its numbers, no mesh needed)
  if (!it && !lit) return "!";
  QString key = lit ? uvFreePicked : QString();
  if (!lit) key = uvWorkSet(m);
  const QJsonObject val{{"w", t.w}, {"h", t.h}, {"pad", t.pad}, {"td", t.td}, {"color", t.color}, {"tex", t.tex}};
  if (!key.isEmpty()) { QJsonObject P = scene_->fileRaw.value("uvSetProps").toObject(); P.insert(key, val); scene_->fileRaw.insert("uvSetProps", P); }   // (a named set: the project's)
  else { QJsonObject all = it->raw.value("uvTex").toObject(); all.insert(key, val); it->raw.insert("uvTex", all); }
  stepLabel_ = tr("UV set %1: %2 × %3, padding %4 px").arg(key.isEmpty() ? tr("(mesh)") : key).arg(t.w).arg(t.h).arg(t.pad); scene_->revision++; commit(false); uvChanged(); emit topoState();
  return tr("%1: texture %2 × %3, padding %4 px").arg(key.isEmpty() ? tr("the mesh") : key).arg(t.w).arg(t.h).arg(t.pad);
}

}  // namespace hm

namespace hm {
bool Viewport::uvNone(int m) const { const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr; return B && !topo::hasUvSets(*B) && !uv::has(*B); }
}  // namespace hm

namespace hm {
// checks: the topology mesh as if it had never been unwrapped
QString Viewport::uvTestStrip() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || !baseOf(*it)) return "!";
  auto B = baseSp(*it); auto nb = std::make_shared<PolyMesh>(*B);
  nb->cornerUV.clear(); nb->uvSetNames.clear(); nb->faceSets.clear(); nb->faceLive.clear(); nb->setUV.clear(); nb->seams.clear();
  topoMeshEdited(m, nb, B); return "stripped";
}
}  // namespace hm
