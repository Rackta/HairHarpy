// UV Sets and UV Mats (Viewport.h ▦ UV Sets). A UV Set is a UV layout of its own over some of the mesh's polygons (the
// PolyMesh's uvSetNames / faceSets / setUV; a polygon can be in several — a head laid out twice); the polygon shows one of
// them (faceLive — its UV in cornerUV, where the UV Editor and the textures read it; the set's own copy is brought up to
// date — flushed — before a set is read). The UV Mats — the mesh's materials (faceMat into matNames; none — Default) —
// each use a set (raw "uvSets": [{ name, mats: [mat names; "" — Default] }]). Every edit is one undo step: the mesh written
// as its new source (the topology shared — it stays), the mats' sets into the raw.
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
  const int nf = d.faceCount(); d.uvSetNames = {"UV Set 1"}; d.faceSets.assign(size_t(nf), 1u); d.faceLive.assign(size_t(nf), 0);
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
  else out.push_back({"UV Set 1", {}, B->faceCount()});
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
  if (uvPicked.id != it.id) return h;
  h.resize(size_t(B.faceCount()), 0);
  if (uvPicked.mat != -2) { for (int f = 0; f < B.faceCount(); f++) if (matOf(B, f) != uvPicked.mat) h[size_t(f)] = 1; }
  else if (topo::hasUvSets(B) && uvSetIso) { for (int f = 0; f < B.faceCount(); f++) if (!inSet(B, f, uvPicked.set)) h[size_t(f)] = 1; }   // (a set: the rest hidden only while it is isolated)
  return h;
}

std::vector<int> Viewport::uvMatFaces(int m, const std::set<int>& mats) const {
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr; std::vector<int> o; if (!B) return o;
  for (int f = 0; f < B->faceCount(); f++) if (mats.count(matOf(*B, f))) o.push_back(f);
  return o;
}
std::vector<std::vector<int>> Viewport::uvMatGroups(int m) const {
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr; if (!B) return {};
  const auto h = uvHidden(*it, *B); std::map<int, std::vector<int>> by;
  for (int f = 0; f < B->faceCount(); f++) if (h.empty() || size_t(f) >= h.size() || !h[size_t(f)]) by[matOf(*B, f)].push_back(f);
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
  if (vp->topoHeavy(m)) { ui::toast(QObject::tr("A high-poly mesh (over 300 000 triangles) is not edited")); return "!"; }
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
  scene_->revision++; staticRev_++; commit(true); emit topoState();
}

// ---------- the pick: what the UV Editor shows (and the polygons show the picked set's layout) ----------
namespace {
bool showAll(PolyMesh& d, int s, const std::vector<int>& fs) { bool ch = false; for (int f : fs) ch |= showSet(d, f, s); return ch; }
}  // namespace
void Viewport::uvPickSet(int m, int set) {
  if (uvPicked.id == m && uvPicked.set == set && uvPicked.mat == -2) { uvPicked = {}; uvChanged(); emit topoState(); return; }
  uvPicked = {m, set, -2};
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr;
  if (B && topo::hasUvSets(*B) && size_t(set) < B->uvSetNames.size()) {
    std::vector<int> fs; for (int f = 0; f < B->faceCount(); f++) if (inSet(*B, f, set) && B->faceLive[size_t(f)] != set) fs.push_back(f);
    if (!fs.empty()) uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>&, QJsonObject&) -> QString { showAll(d, set, fs); return QString(); });
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
QString Viewport::uvSetFromShells(int set) {
  if (topoMode == "object") {                                   // (objects: all of each one's polygons — a new set under one name in each)
    std::vector<int> ms; for (int id : topoMeshes()) if (const Item* it = scene_->item(id)) if (baseOf(*it) && baseOf(*it)->faceCount()) ms.push_back(id);
    if (ms.empty()) { ui::toast(tr("Select objects (Object mode) — or the shells on the model (a polygon of each is enough)")); return "!"; }
    if (set >= 0) { const int m = topoMesh(); if (m < 0) return "!"; std::vector<int> all(size_t(baseOf(*scene_->item(m))->faceCount())); for (size_t f = 0; f < all.size(); f++) all[f] = int(f);
      return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString { if (size_t(set) >= S.size()) return "!"; return putIntoSet(d, S, set, all); }, true); }
    QString nm, r; for (int m : ms) { std::vector<int> all(size_t(baseOf(*scene_->item(m))->faceCount())); for (size_t f = 0; f < all.size(); f++) all[f] = int(f);
      if (nm.isEmpty()) { r = uvSetNew(m, all); if (r.startsWith('!')) return r; nm = uvSets(m).empty() ? QString() : uvSets(m).back().name; } else uvSetNew(m, all, nm); }
    return ms.size() > 1 ? tr("%1: %2 objects").arg(nm).arg(ms.size()) : r;
  }
  const int m = topoMesh(); if (m < 0 || tsel_.id != m || tSelFaces(m).empty()) { ui::toast(tr("Select the shells on the model (a polygon of each is enough) — or objects (Object mode)")); return "!"; }
  if (uvSelectShells() == "!") return "!";                      // (the whole shells of what is picked)
  return set < 0 ? uvSetNew(m) : uvSetApply(m, set);
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
  for (int f : fs) { if (!inSet(d, f, si)) { d.faceSets[size_t(f)] |= 1u << si; for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) d.setUV[size_t(si)][size_t(k)] = d.cornerUV[size_t(k)]; } showSet(d, f, si); }
  std::map<int, std::vector<int>> by; for (int f : fs) by[matOf(d, f)].push_back(f);
  std::vector<int> all(d.matNames.size() + 1, 0); for (int f = 0; f < d.faceCount(); f++) all[size_t(matOf(d, f) + 1)]++;
  std::set<QString> taken(d.matNames.begin(), d.matNames.end()); int made = 0;
  auto take = [&](int k) { for (auto& s : S) s.mats.erase(std::remove(s.mats.begin(), s.mats.end(), k), s.mats.end()); };
  for (auto& [k, list] : by) {
    if (int(list.size()) == all[size_t(k + 1)]) { take(k); S[size_t(si)].mats.push_back(k); continue; }
    const QString nm = freeName(matName(d, k) + " · " + S[size_t(si)].name, taken); taken.insert(nm);
    d.matNames.push_back(nm); const int nk = int(d.matNames.size()) - 1; for (int f : list) d.faceMat[size_t(f)] = nk;
    S[size_t(si)].mats.push_back(nk); made++;
  }
  QString r = QObject::tr("%1 polygon%2 into %3").arg(fs.size()).arg(fs.size() == 1 ? "" : "s").arg(S[size_t(si)].name);
  if (made) r += QObject::tr(" · %1 part%2 of a mat became a new mat").arg(made).arg(made == 1 ? "" : "s");
  return r;
}
QString tooMany() { return QObject::tr("!%1 UV Sets at most — merge or delete one first").arg(tedit::kUvSets); }
}  // namespace

QString Viewport::uvSetNew(int m, const std::vector<int>& faces, const QString& name) {
  const auto fs = faces.empty() ? selPolys(this, m) : faces; if (fs.empty()) { ui::toast(tr("Select the polygons the new UV Set lays out — or objects (Object mode): all of them")); return "!"; }
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (int(S.size()) >= tedit::kUvSets) return tooMany();
    std::set<QString> taken; for (auto& s : S) taken.insert(s.name);
    const QString nm = freeName(name.isEmpty() ? QString("UV Set %1").arg(S.size() + 1) : name, taken);
    S.push_back({nm, {}, 0}); d.uvSetNames.push_back(nm); d.setUV.push_back(std::vector<std::array<float, 2>>(d.faceVerts.size(), {0, 0}));
    uvPicked = {m, int(S.size()) - 1, -2};
    return tr("%1: ").arg(nm) + putIntoSet(d, S, int(S.size()) - 1, fs); }, true);
}
QString Viewport::uvSetApply(int m, int set) {
  const auto fs = selPolys(this, m); if (fs.empty()) { ui::toast(tr("Select polygons first — they go into the set's layout")); return "!"; }
  return uvMatsEdit(this, scene_, m, [&](PolyMesh& d, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(set) >= S.size()) return "!"; return putIntoSet(d, S, set, fs); }, true);
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
  return uvMatsEdit(this, scene_, m, [&](PolyMesh&, std::vector<UvSetL>& S, QJsonObject&) -> QString {
    if (size_t(set) >= S.size() || S[size_t(set)].name == n) return "!";
    for (auto& s : S) if (s.name == n) return tr("!%1 — that name is taken").arg(n);
    S[size_t(set)].name = n; return QString(); });
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
Viewport::UvTex Viewport::uvTex(int m, int set) const {
  UvTex t; const Item* it = scene_->item(m); if (!it) return t;
  if (set == -2) set = uvPicked.id == m && uvPicked.mat == -2 ? uvPicked.set : -1;
  const QJsonObject all = it->raw.value("uvTex").toObject(); QJsonObject o = all.value("").toObject();   // (the mesh's own; a set's over it)
  if (set >= 0) { const auto S = uvSets(m); if (size_t(set) < S.size() && all.contains(S[size_t(set)].name)) o = all.value(S[size_t(set)].name).toObject(); }
  t.w = std::clamp(o.value("w").toInt(t.w), 16, 16384); t.h = std::clamp(o.value("h").toInt(t.h), 16, 16384); t.pad = std::clamp(o.value("pad").toDouble(t.pad), 0.0, 256.0); t.td = std::max(0.0, o.value("td").toDouble(0));
  return t;
}
uv::PackOpts Viewport::uvPackOpts(int m) const { const UvTex t = uvTex(m); uv::PackOpts o; o.w = t.w; o.h = t.h; o.pad = t.pad; o.rotate = uvo.rotate; return o; }
QString Viewport::uvSetTex(int m, const UvTex& t) {
  Item* it = scene_->item(m); if (!it) return "!";
  const int set = uvPicked.id == m && uvPicked.mat == -2 ? uvPicked.set : -1; const auto S = uvSets(m);
  const QString key = set >= 0 && size_t(set) < S.size() ? S[size_t(set)].name : QString();
  QJsonObject all = it->raw.value("uvTex").toObject(); all.insert(key, QJsonObject{{"w", t.w}, {"h", t.h}, {"pad", t.pad}, {"td", t.td}}); it->raw.insert("uvTex", all);
  stepLabel_ = tr("UV texture %1 × %2, padding %3 px").arg(t.w).arg(t.h).arg(t.pad); scene_->revision++; commit(false); uvChanged(); emit topoState();
  return tr("%1: texture %2 × %3, padding %4 px").arg(key.isEmpty() ? tr("the mesh") : key).arg(t.w).arg(t.h).arg(t.pad);
}

}  // namespace hm
