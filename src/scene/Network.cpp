#include "scene/Network.h"
#include "scene/CurveMath.h"
#include "scene/HairState.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace hm {

namespace {
const double NEW_ROUND = 0.6, NEW_FAIR = 0.5;           // NEW_CURVE: the look of a new curve
int idxOf(const std::vector<int>& A, int a) { auto it = std::find(A.begin(), A.end(), a); return it == A.end() ? -1 : int(it - A.begin()); }
std::string keyOf(const std::vector<int>& A) {           // the same anchors either way → one key
  std::string f, r;
  for (size_t i = 0; i < A.size(); i++) { if (i) f += ','; f += std::to_string(A[i]); }
  for (size_t i = A.size(); i-- > 0;) { if (i + 1 < A.size()) r += ','; r += std::to_string(A[i]); }
  return f < r ? f : r;
}
std::string joinIds(const std::vector<int>& ids, char sep = ',') { std::string s; for (size_t i = 0; i < ids.size(); i++) { if (i) s += sep; s += std::to_string(ids[i]); } return s; }

// String.prototype.localeCompare(…, { numeric: true }) for the network tags: runs of digits by value; punctuation before
// digits, digits before letters
int naturalCompare(const QString& a, const QString& b) {
  auto cls = [](QChar c) { return c.isDigit() ? 1 : c.isLetter() ? 2 : 0; };
  int i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    QChar x = a[i], y = b[j];
    if (x.isDigit() && y.isDigit()) {
      int i2 = i, j2 = j; while (i2 < a.size() && a[i2].isDigit()) i2++; while (j2 < b.size() && b[j2].isDigit()) j2++;
      qlonglong u = a.mid(i, i2 - i).toLongLong(), v = b.mid(j, j2 - j).toLongLong();
      if (u != v) return u < v ? -1 : 1;
      i = i2; j = j2; continue;
    }
    if (cls(x) != cls(y)) return cls(x) < cls(y) ? -1 : 1;
    if (cls(x) == 0) {                                          // (punctuation in the root collation's order: _ - , ; : …)
      static const QString P = QStringLiteral("_-,;:!?.\x27\"()[]{}@*/\\&#%`^+<=>|~$");
      auto rank = [](QChar c) { int k = int(P.indexOf(c)); return k >= 0 ? k : 1000 + c.unicode(); };
      if (rank(x) != rank(y)) return rank(x) < rank(y) ? -1 : 1;
      i++; j++; continue;
    }
    QChar lx = x.toLower(), ly = y.toLower();
    if (lx != ly) return lx < ly ? -1 : 1;
    i++; j++;
  }
  return (a.size() - i) - (b.size() - j) < 0 ? -1 : (a.size() - i) == (b.size() - j) ? 0 : 1;
}
}  // namespace

Network::Network(Scene& sc, const CurveCache& samples, std::vector<int>& sel, std::vector<int>& asel)
    : sc_(sc), samples_(samples), sel_(sel), asel_(asel) { sc_.reindex(); }

std::vector<const Item*> Network::surfaces() const {
  std::vector<const Item*> out;
  for (auto& [id, it] : sc_.items) if (it.type == ItemType::Surface) out.push_back(&it);
  std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->order < b->order; });
  return out;
}

const std::vector<Vec3>* Network::samplesOf(int id) const {
  auto it = samples_.find(id);
  return it == samples_.end() || it->second.p.size() < 2 ? nullptr : &it->second.p;
}

// c.arc's last value, else the polyline of its anchors
double Network::curveLen(const Item& c) const {
  auto it = samples_.find(c.id);
  if (it != samples_.end() && !it->second.arc.empty()) return it->second.arc.back();
  double L = 0; for (size_t i = 1; i < c.anchors.size(); i++) L += sc_.anchor(c.anchors[i])->pos.distanceTo(sc_.anchor(c.anchors[i - 1])->pos);
  return L;
}

bool Network::isNewLogic(const Item& c) const { return groupLogic(sc_, c) == 2; }

void Network::dropSel(int id) { sel_.erase(std::remove(sel_.begin(), sel_.end(), id), sel_.end()); }

// nextNumber: one more than the highest "<Type> N" in the scene — kept in the project's counters too
QString Network::nextName(ItemType type, const QString& base) {
  QRegularExpression re("^" + QRegularExpression::escape(base) + " (\\d+)$"); int mx = 0;
  for (auto& [id, it] : sc_.items) if (it.type == type) { auto m = re.match(it.name); if (m.hasMatch()) mx = std::max(mx, m.captured(1).toInt()); }
  QJsonObject counters = sc_.fileRaw.value("counters").toObject();
  counters.insert(type == ItemType::Curve ? "curve" : itemTypeName(type).toLower(), mx + 1);
  sc_.fileRaw.insert("counters", counters);
  return QStringLiteral("%1 %2").arg(base).arg(mx + 1);
}

Item& Network::makeCurve(int parent) {
  Item& c = sc_.addItem(ItemType::Curve, parent, nextName(ItemType::Curve, "Curve"));
  return c;
}

QString Network::serviceName(const QString& pre) const {
  int n = 0; QRegularExpression re("^([A-Z]{2})(\\d+)$");
  for (const Item* c : curves()) { if (!c->junc) continue; auto m = re.match(c->name); if (m.hasMatch() && m.captured(1) == pre) n = std::max(n, m.captured(2).toInt()); }
  return pre + QString::number(n + 1);
}

// ---------- deleting ----------

// service curves (junc) no wall uses any more — left from older deletions — go
int Network::dropOrphanServiceCurves() {
  std::set<int> used; for (const Item* s : surfaces()) { used.insert(s->a); used.insert(s->b); }
  std::vector<int> kill;
  for (const Item* c : curves()) {
    if (!c->junc || used.count(c->id)) continue;
    bool pathUser = false; for (const Item* o : curves()) for (auto& p : o->path) if (p[0] == c->id) pathUser = true;
    bool tube = false; for (auto& [tid, t] : sc_.items) if (t.type == ItemType::Tube && t.raw.value("src").toInt(-1) == c->id) tube = true;
    if (!pathUser && !tube) kill.push_back(c->id);
  }
  int n = 0; for (int id : kill) if (sc_.item(id)) { deleteItem(id); n++; }
  return n;
}

void Network::deleteItem(int id) {
  Item* it = sc_.item(id); if (!it) return;
  if (it->type == ItemType::Group || it->type == ItemType::Cocoon) for (int c : std::vector<int>(it->children)) deleteItem(c);
  if (it->type == ItemType::Curve) {
    std::vector<int> kill;                                   // (ids first: a deletion may take others with it)
    for (const Item* s : surfaces()) if (s->a == id || s->b == id) kill.push_back(s->id);
    for (const Item* c : curves()) if (c->id != id) for (auto& p : c->path) if (p[0] == id) { kill.push_back(c->id); break; }
    for (auto& [tid, t] : sc_.items) if ((t.type == ItemType::Tube || t.type == ItemType::Braid) && t.raw.value("src").toInt(-1) == id) kill.push_back(tid);
    for (auto& [kid, k] : sc_.items) if (k.type == ItemType::Crown && (k.raw.value("part").toInt(-1) == id || k.raw.value("low").toInt(-1) == id)) kill.push_back(kid);
    for (auto& [gid, g] : sc_.items) {                       // guide lines on its curve lose their hits there
      if (g.type != ItemType::Guide || g.raw.value("kind").toString() == "disc") continue;
      QJsonArray hits = g.raw.value("hits").toArray(), keep;
      for (const QJsonValue& h : hits) if (h.toObject().value("c").toInt() != id) keep.append(h);
      if (keep.size() != hits.size()) { g.raw.insert("hits", keep); if (keep.size() < 2) kill.push_back(gid); }
    }
    for (int k : kill) deleteItem(k);
  }
  if (it->type == ItemType::Braid) { std::vector<int> x; for (const QJsonValue& v : it->raw.value("conns").toArray()) if (sc_.item(v.toInt(-1))) x.push_back(v.toInt()); for (int k : x) deleteItem(k); it = sc_.item(id); if (!it) return; }   // (its end connectors go with it)
  if (it->type == ItemType::Tube) { std::vector<int> x; for (auto& [xid, m] : sc_.items) if (m.type == ItemType::XMod && m.raw.value("tube").toInt(-1) == id) x.push_back(xid); for (int k : x) deleteItem(k); }
  it = sc_.item(id); if (!it) return;
  ItemType type = it->type; int a = it->a, b = it->b;
  sc_.removeFromParent(id);
  sc_.items.erase(id);
  dropSel(id);
  if (type == ItemType::Curve) {                             // (the selected anchors still on some curve stay)
    std::unordered_set<int> alive; for (const Item* c : curves()) alive.insert(c->anchors.begin(), c->anchors.end());
    asel_.erase(std::remove_if(asel_.begin(), asel_.end(), [&](int x) { return !alive.count(x); }), asel_.end());
  }
  // a wall made by Fill hole goes with its service curves — once no other wall uses them
  if (type == ItemType::Surface) for (int cid : {a, b}) {
    const Item* c = sc_.item(cid); if (!c || !c->junc) continue;
    bool used = false; for (const Item* s : surfaces()) if (s->a == cid || s->b == cid) used = true;
    bool pathUser = false; for (const Item* e : curves()) for (auto& p : e->path) if (p[0] == cid) pathUser = true;
    if (!used && !pathUser) deleteItem(cid);
  }
}

void Network::pruneAnchors() {
  std::unordered_set<int> used; for (auto& [id, c] : sc_.items) if (c.type == ItemType::Curve) used.insert(c.anchors.begin(), c.anchors.end());
  for (auto it = sc_.anchors.begin(); it != sc_.anchors.end();) it = used.count(it->first) ? std::next(it) : sc_.anchors.erase(it);
}

void Network::dedupeWalls() {
  std::set<std::pair<int, int>> seen; std::vector<int> ids;
  for (const Item* s : surfaces()) ids.push_back(s->id);
  for (int id : ids) {
    const Item* s = sc_.item(id); if (!s) continue;
    auto k = std::make_pair(std::min(s->a, s->b), std::max(s->a, s->b));
    if (s->a == s->b || seen.count(k)) deleteItem(id); else seen.insert(k);
  }
}

// guide hits / attachments / tubes of curve `from` go to `to` (t: their place along it → along `to`)
void Network::remapCurveRefs(int from, int to, const std::function<double(double)>& t) {
  for (auto& [gid, g] : sc_.items) {
    if (g.type != ItemType::Guide || g.raw.value("kind").toString() == "disc") continue;
    QJsonArray hits = g.raw.value("hits").toArray(); bool ch = false;
    for (int i = 0; i < hits.size(); i++) { QJsonObject h = hits[i].toObject(); if (h.value("c").toInt() == from) { h.insert("c", to); h.insert("t", t(h.value("t").toDouble())); hits[i] = h; ch = true; } }
    if (ch) g.raw.insert("hits", hits);
  }
  for (auto& [aid, a] : sc_.anchors) { QJsonObject at = a.raw.value("at").toObject(); if (!at.isEmpty() && at.value("c").toInt() == from) { at.insert("c", to); at.insert("t", t(at.value("t").toDouble())); a.raw.insert("at", at); } }
}

// ---------- routes and edge curves ----------

// anchors → [{ c, i0, i1 }] along the network curves
std::optional<std::vector<Network::Stretch>> Network::routeStretches(const std::vector<int>& L, const std::set<int>& skip) const {
  std::vector<const Item*> net; for (const Item* c : curves()) if (segNet(*c) && !skip.count(c->id)) net.push_back(c);
  std::vector<Stretch> out; size_t k = 0;
  while (k + 1 < L.size()) {
    const Item* p = nullptr;
    for (const Item* q : net) { int i = idxOf(q->anchors, L[k]); if (i >= 0 && ((i + 1 < int(q->anchors.size()) && q->anchors[i + 1] == L[k + 1]) || (i > 0 && q->anchors[i - 1] == L[k + 1]))) { p = q; break; } }
    if (!p) return std::nullopt;
    int i0 = idxOf(p->anchors, L[k]), dir = i0 + 1 < int(p->anchors.size()) && p->anchors[i0 + 1] == L[k + 1] ? 1 : -1, i1 = i0;
    while (k + 1 < L.size() && i1 + dir >= 0 && i1 + dir < int(p->anchors.size()) && p->anchors[i1 + dir] == L[k + 1]) { i1 += dir; k++; }
    out.push_back({p->id, i0, i1});
  }
  return out;
}

std::optional<std::vector<int>> Network::pathAnchors(const std::vector<std::array<int, 3>>& path) const {
  std::vector<int> out;
  for (auto& [cid, a0, a1] : path) {
    const Item* c = sc_.item(cid); if (!c) return std::nullopt;
    int i0 = idxOf(c->anchors, a0), i1 = idxOf(c->anchors, a1); if (i0 < 0 || i1 < 0) return std::nullopt;
    std::vector<int> run;
    if (i0 <= i1) run.assign(c->anchors.begin() + i0, c->anchors.begin() + i1 + 1);
    else { run.assign(c->anchors.begin() + i1, c->anchors.begin() + i0 + 1); std::reverse(run.begin(), run.end()); }
    for (int a : run) if (out.empty() || out.back() != a) out.push_back(a);
  }
  return out.size() >= 2 ? std::optional(out) : std::nullopt;
}

// anchors on no network curve go; a step between anchors that are no neighbours becomes the shortest way along the curves
std::optional<std::vector<int>> Network::bridgeAnchors(const std::vector<int>& L) const {
  std::unordered_map<int, std::vector<int>> nb; std::vector<int> nbOrder;
  auto add = [&](int a, int b) { auto& v = nb[a]; if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b); };
  for (const Item* c : curves()) {
    if (!segNet(*c) || c->anchors.size() < 2) continue;
    for (size_t k = 0; k < c->anchors.size(); k++) { if (!nb.count(c->anchors[k])) nb[c->anchors[k]]; if (k > 0) add(c->anchors[k], c->anchors[k - 1]); if (k + 1 < c->anchors.size()) add(c->anchors[k], c->anchors[k + 1]); }
  }
  auto way = [&](int a, int b) -> std::optional<std::vector<int>> {
    std::unordered_map<int, double> dist{{a, 0}}; std::unordered_map<int, int> prev; std::vector<int> Q{a};
    while (!Q.empty()) {
      size_t vi = 0; for (size_t i = 1; i < Q.size(); i++) if (dist[Q[i]] < dist[Q[vi]]) vi = i;
      int v = Q[vi]; Q.erase(Q.begin() + vi);
      if (v == b) { std::vector<int> p{b}; while (p.front() != a) p.insert(p.begin(), prev[p.front()]); return p; }
      for (int w : nb[v]) { double d = dist[v] + sc_.anchor(v)->pos.distanceTo(sc_.anchor(w)->pos); if (!dist.count(w) || d < dist[w]) { dist[w] = d; prev[w] = v; if (std::find(Q.begin(), Q.end(), w) == Q.end()) Q.push_back(w); } }
    }
    return std::nullopt;
  };
  std::vector<int> M; for (int a : L) if (nb.count(a)) M.push_back(a);
  std::vector<int> out;
  for (size_t k = 0; k < M.size(); k++) {
    if (!k) { out.push_back(M[0]); continue; }
    int a = out.back(), b = M[k]; if (a == b) continue;
    auto& n = nb[a]; if (std::find(n.begin(), n.end(), b) != n.end()) { out.push_back(b); continue; }
    auto p = way(a, b); if (!p) return std::nullopt;
    out.insert(out.end(), p->begin() + 1, p->end());
  }
  return out.size() >= 2 ? std::optional(out) : std::nullopt;
}

// stretches → one curve: the source curve itself when a stretch is a whole curve, else an edge curve along them
int Network::pathCurve(const std::vector<Stretch>& st, int parent) {
  if (st.size() == 1) { const Item* c = sc_.item(st[0].c); if (std::min(st[0].i0, st[0].i1) == 0 && std::max(st[0].i0, st[0].i1) == int(c->anchors.size()) - 1) return c->id; }
  std::vector<std::array<int, 3>> path, rpath;
  for (auto& s : st) { const Item* c = sc_.item(s.c); path.push_back({s.c, c->anchors[s.i0], c->anchors[s.i1]}); }
  for (auto it = path.rbegin(); it != path.rend(); ++it) rpath.push_back({(*it)[0], (*it)[2], (*it)[1]});
  for (const Item* x : curves()) if (x->hasPath() && (x->path == path || x->path == rpath)) return x->id;
  Item& e = makeCurve(parent); e.junc = true; e.path = path; e.name = serviceName("EC");
  e.anchors = pathAnchors(path).value_or(std::vector<int>{});
  return e.id;
}

// ---------- paths through the forks ----------

Vec3 Network::leaveDir(const Item& c, int a) const {
  std::vector<Vec3> A; const std::vector<Vec3>* P = samplesOf(c.id);
  if (!P) { for (int x : c.anchors) A.push_back(sc_.anchor(x)->pos); P = &A; }
  int n = int(P->size()), k = std::min(3, n - 1);
  Vec3 d = c.anchors.front() == a ? (*P)[k] - (*P)[0] : (*P)[n - 1 - k] - (*P)[n - 1];
  return d.lengthSq() > 1e-14 ? d.normalized() : d;
}

std::vector<NetPath> Network::curvePaths(const std::vector<int>& sel, bool all) const {
  std::vector<const Item*> net; for (const Item* c : curves()) if (segNet(*c) && c->anchors.size() >= 2 && (all || sc_.isVisible(*c))) net.push_back(c);
  std::unordered_map<int, std::vector<const Item*>> ends;
  for (const Item* c : net) for (int a : {c->anchors.front(), c->anchors.back()}) { auto& l = ends[a]; if (std::find(l.begin(), l.end(), c) == l.end()) l.push_back(c); }
  auto other = [](const Item& c, int a) { return c.anchors.front() == a ? c.anchors.back() : c.anchors.front(); };
  using Walk = std::vector<std::pair<const Item*, int>>;
  std::function<std::vector<Walk>(const Item*, int, std::set<const Item*>&, int)> walk = [&](const Item* c, int a, std::set<const Item*>& used, int depth) {
    Vec3 din = -leaveDir(*c, a); std::vector<const Item*> next;
    auto it = ends.find(a);
    if (it != ends.end()) for (const Item* e : it->second) if (e != c && !used.count(e) && leaveDir(*e, a).dot(din) > 0) next.push_back(e);
    if (next.empty() || depth > 60) return std::vector<Walk>{Walk{}};
    std::vector<Walk> out;
    for (const Item* e : next) {
      used.insert(e);
      for (auto& rest : walk(e, other(*e, a), used, depth + 1)) { Walk w{{e, a}}; w.insert(w.end(), rest.begin(), rest.end()); out.push_back(std::move(w)); if (out.size() > 400) break; }
      used.erase(e);
      if (out.size() > 400) break;
    }
    return out;
  };
  const Item* c0 = sc_.item(sel.back()); int A0 = c0->anchors.front(), A1 = c0->anchors.back();
  std::set<const Item*> u1{c0}, u2{c0};
  auto fw = walk(c0, A1, u1, 0), bw = walk(c0, A0, u2, 0);
  std::vector<NetPath> out; std::set<std::string> seen;
  for (auto& b : bw) for (auto& f : fw) {
    std::set<int> ids{c0->id}; for (auto& x : b) ids.insert(x.first->id); for (auto& x : f) ids.insert(x.first->id);
    if (ids.size() != 1 + b.size() + f.size()) continue;
    bool all = true; for (int c : sel) if (!ids.count(c)) all = false; if (!all) continue;
    NetPath steps;
    for (auto it = b.rbegin(); it != b.rend(); ++it) steps.push_back({it->first->id, other(*it->first, it->second)});
    steps.push_back({c0->id, A0});
    for (auto& x : f) steps.push_back({x.first->id, x.second});
    std::vector<int> key; for (auto& s : steps) key.push_back(s.c);
    if (!seen.insert(joinIds(key)).second) continue;
    out.push_back(steps);
    if (out.size() >= 200) break;
  }
  for (auto& p : out) {                                      // the root first: the higher of the path's two ends
    int e0 = p.front().from, eN = other(*sc_.item(p.back().c), p.back().from);
    if (sc_.anchor(eN)->pos.y > sc_.anchor(e0)->pos.y) { std::reverse(p.begin(), p.end()); for (auto& s : p) s.from = other(*sc_.item(s.c), s.from); }
  }
  return out;
}

// ---------- segmentNetwork: the network cut at its forks ----------

int Network::segmentNetwork() {
  sc_.reindex();
  std::vector<Item*> net;
  for (const Item* c : curves()) if (segNet(*c) && c->anchors.size() >= 2) net.push_back(sc_.item(c->id));
  std::set<int> crownUse;
  for (auto& [kid, k] : sc_.items) if (k.type == ItemType::Crown) { crownUse.insert(k.raw.value("part").toInt(-1)); crownUse.insert(k.raw.value("low").toInt(-1)); }
  std::vector<Item*> mine; for (Item* c : net) if (isNewLogic(*c) && !crownUse.count(c->id)) mine.push_back(c);
  if (mine.empty()) return 0;
  for (Item* c : mine) { c->seg = true; if (!c->roundSet) { c->round = NEW_ROUND; c->roundSet = true; } if (!c->fairSet) { c->fair = NEW_FAIR; c->fairSet = true; } }
  std::unordered_map<int, std::set<int>> nb; std::unordered_set<int> node;
  for (Item* c : net) {
    const auto& A = c->anchors; node.insert(A.front()); node.insert(A.back());
    for (size_t k = 0; k < A.size(); k++) { auto& s = nb[A[k]]; if (k > 0) s.insert(A[k - 1]); if (k + 1 < A.size()) s.insert(A[k + 1]); }
  }
  for (auto& [a, s] : nb) if (s.size() != 2) node.insert(a);
  auto cuts = [&](const Item* c) { std::vector<int> out{0}; for (int i = 1; i + 1 < int(c->anchors.size()); i++) if (node.count(c->anchors[i])) out.push_back(i); out.push_back(int(c->anchors.size()) - 1); return out; };
  std::vector<Item*> todo; for (Item* c : mine) if (cuts(c).size() > 2) todo.push_back(c);
  if (todo.empty()) return 0;
  std::set<Item*> todoSet(todo.begin(), todo.end());
  // every handle as it is now: after the cut, the ones that came out different are frozen to these
  std::map<int, std::vector<Handles>> oldH;
  for (Item* c : net) { std::vector<Handles> h; for (size_t i = 0; i < c->anchors.size(); i++) h.push_back(curveHandles(sc_, *c, int(i))); oldH[c->id] = h; }
  std::vector<std::pair<int, std::vector<int>>> ecRoute;
  for (const Item* e : curves()) if (e->hasPath()) if (auto L = pathAnchors(e->path)) ecRoute.push_back({e->id, *L});
  std::map<std::string, int> byKey; for (Item* c : net) if (!todoSet.count(c)) byKey[keyOf(c->anchors)] = c->id;
  struct Part { std::vector<int> A; std::vector<Handles> H; double f0, f1; int cv = -1; };
  struct Plan { int c; std::vector<int> route; std::vector<Part> parts; OvrMap ovr; int keep = -1; QString name; int parent; double round, fair; bool visible; };
  std::vector<Plan> plan;
  for (Item* c : todo) {
    auto cs = cuts(c); auto si = samples_.find(c->id);
    const std::vector<double>* L = si != samples_.end() && !si->second.arc.empty() ? &si->second.arc : nullptr;
    double tot = L ? (L->back() ? L->back() : 1) : 1;
    auto fr = [&](int i) { return L ? (*L)[std::min<size_t>(size_t(i) * 24, L->size() - 1)] / tot : double(i) / (c->anchors.size() - 1); };
    Plan p{c->id, c->anchors, {}, c->ovr, -1, c->name, c->parent, c->round, c->fair, c->visible};
    for (size_t k = 0; k + 1 < cs.size(); k++) {
      Part q; q.A.assign(c->anchors.begin() + cs[k], c->anchors.begin() + cs[k + 1] + 1);
      q.H.assign(oldH[c->id].begin() + cs[k], oldH[c->id].begin() + cs[k + 1] + 1); q.f0 = fr(cs[k]); q.f1 = fr(cs[k + 1]);
      p.parts.push_back(q);
    }
    plan.push_back(p);
  }
  std::map<std::string, int> uses; for (auto& p : plan) for (auto& q : p.parts) uses[keyOf(q.A)]++;
  for (auto& p : plan) {                                     // the piece the curve keeps (its id, name)
    int best = -1;
    for (int i = 0; i < int(p.parts.size()); i++) { auto k = keyOf(p.parts[i].A); if (uses[k] == 1 && !byKey.count(k) && (best < 0 || p.parts[i].A.size() > p.parts[best].A.size())) best = i; }
    p.keep = best;
    if (best >= 0) byKey[keyOf(p.parts[best].A)] = p.c;
  }
  auto trunkName = [&] { std::set<QString> has; for (const Item* c : curves()) has.insert(c->name); int n = 1; while (has.count(QStringLiteral("Trunk %1").arg(n))) n++; return QStringLiteral("Trunk %1").arg(n); };
  for (auto& p : plan) {
    int k = 0;
    for (int qi = 0; qi < int(p.parts.size()); qi++) {
      Part& q = p.parts[qi]; auto key = keyOf(q.A);
      if (qi == p.keep) { q.cv = p.c; continue; }
      auto f = byKey.find(key); int cv = f != byKey.end() ? f->second : -1;
      if (cv < 0) {
        Item& n = makeCurve(p.parent); n.anchors = q.A; n.seg = true; n.round = p.round; n.fair = p.fair; n.roundSet = n.fairSet = true; n.visible = p.visible;
        n.name = uses[key] > 1 ? trunkName() : QStringLiteral("%1.%2").arg(p.name).arg(++k);
        for (int a : q.A) if (auto o = p.ovr.find(a); o != p.ovr.end()) n.ovr[a] = o->second;
        std::vector<int>& arr = p.parent >= 0 ? sc_.item(p.parent)->children : sc_.roots;
        arr.erase(std::find(arr.begin(), arr.end(), n.id)); int at = idxOf(arr, p.c) + 1 + k;
        arr.insert(arr.begin() + std::min<int>(std::max(at, 0), int(arr.size())), n.id);
        cv = n.id; byKey[key] = cv;
      }
      q.cv = cv;
    }
  }
  for (auto& p : plan) if (p.keep >= 0) {
    Item* c = sc_.item(p.c); c->anchors = p.parts[p.keep].A; c->ovr.clear();
    for (auto& [id, o] : p.ovr) if (idxOf(c->anchors, id) >= 0) c->ovr[id] = o;
  }
  std::vector<std::pair<int, std::vector<Handles>>> was; std::set<int> wasHas;
  for (Item* c : net) if (!todoSet.count(c)) { was.push_back({c->id, oldH[c->id]}); wasHas.insert(c->id); }
  for (auto& p : plan) for (auto& q : p.parts) if (q.cv >= 0 && !wasHas.count(q.cv)) {
    const Item* cv = sc_.item(q.cv); std::vector<Handles> H = q.H;
    if (cv->anchors.front() != q.A.front()) { std::reverse(H.begin(), H.end()); for (auto& h : H) std::swap(h.hin, h.hout); }
    was.push_back({q.cv, H}); wasHas.insert(q.cv);
  }
  std::set<int> gone; for (auto& p : plan) if (p.keep < 0) gone.insert(p.c);
  for (auto& p : plan) {                                     // what hung on a cut curve
    std::vector<int> walls; for (const Item* s : surfaces()) if (s->a == p.c || s->b == p.c) walls.push_back(s->id);
    std::vector<int> tbs; for (auto& [tid, t] : sc_.items) if (t.type == ItemType::Tube && t.raw.value("src").toInt(-1) == p.c) tbs.push_back(tid);
    int ec = -1;
    if (!walls.empty() || !tbs.empty()) if (auto st = routeStretches(p.route, gone)) ec = pathCurve(*st, p.parent);
    if (ec >= 0 && ec != p.c) {
      for (int sid : walls) { Item* s = sc_.item(sid); if (s->a == p.c) s->a = ec; if (s->b == p.c) s->b = ec; }
      for (int tid : tbs) sc_.item(tid)->raw.insert("src", ec);
    }
    // guide hits and attachments: onto the edge curve (same t), else onto the piece by arc length
    auto place = [&](double t, int& c, double& tt) {
      if (ec >= 0 && ec != p.c) { c = ec; tt = t; return; }
      const Part* q = &p.parts.back(); for (auto& x : p.parts) if (t <= x.f1 + 1e-9) { q = &x; break; }
      double u = q->f1 > q->f0 ? clamp((t - q->f0) / (q->f1 - q->f0), 0, 1) : 0;
      c = q->cv; tt = sc_.item(q->cv)->anchors.front() == q->A.front() ? u : 1 - u;
    };
    for (auto& [gid, g] : sc_.items) {
      if (g.type != ItemType::Guide || g.raw.value("kind").toString() == "disc") continue;
      QJsonArray hits = g.raw.value("hits").toArray(); bool ch = false;
      for (int i = 0; i < hits.size(); i++) { QJsonObject h = hits[i].toObject(); if (h.value("c").toInt() != p.c) continue; int c; double t; place(h.value("t").toDouble(), c, t); h.insert("c", c); h.insert("t", t); hits[i] = h; ch = true; }
      if (ch) g.raw.insert("hits", hits);
    }
    for (auto& [aid, a] : sc_.anchors) { QJsonObject at = a.raw.value("at").toObject(); if (at.isEmpty() || at.value("c").toInt() != p.c) continue; int c; double t; place(at.value("t").toDouble(), c, t); at.insert("c", c); at.insert("t", t); a.raw.insert("at", at); }
  }
  for (auto& [id, L] : ecRoute) {                            // the edge curves laid anew along the pieces
    Item* e = sc_.item(id); if (!e) continue;
    if (auto st = routeStretches(L, gone)) {
      e->path.clear(); for (auto& s : *st) { const Item* c = sc_.item(s.c); e->path.push_back({s.c, c->anchors[s.i0], c->anchors[s.i1]}); }
      e->anchors = pathAnchors(e->path).value_or(L);
    }
  }
  for (int id : gone) { dropSel(id); deleteItem(id); }
  sc_.reindex();
  // the handles that changed → frozen as they were (again, while a frozen one changes an end aiming at it)
  for (int pass = 0; pass < 4; pass++) {
    int ch = 0;
    for (auto& [cid, Ho] : was) {
      Item* c = sc_.item(cid); if (!c) continue;
      int n = int(c->anchors.size()); if (int(Ho.size()) != n) continue;
      for (int i = 0; i < n; i++) {
        const Anchor* a = sc_.anchor(c->anchors[i]); double sm = a->smooth; if (sm < 1e-3) continue;
        Handles Hn = curveHandles(sc_, *c, i);
        HandleOverride r; auto o = c->ovr.find(a->id); if (o != c->ovr.end()) r = o->second;
        bool set = false;
        if (i > 0 && Hn.hin.distanceTo(Ho[i].hin) > 1e-4) { r.in = (Ho[i].hin - a->pos) / sm; set = true; }
        if (i < n - 1 && Hn.hout.distanceTo(Ho[i].hout) > 1e-4) { r.out = (Ho[i].hout - a->pos) / sm; set = true; }
        if (set) { c->ovr[a->id] = r; ch++; }
      }
    }
    if (!ch) break;
  }
  dedupeWalls();
  return int(todo.size());
}

// ---------- the upkeep after every edit ----------

int Network::repairPathCurves() {
  int n = 0;
  for (const Item* cc : curves()) {
    Item* c = sc_.item(cc->id); if (!c->hasPath()) continue;
    if (auto L0 = pathAnchors(c->path)) { if (*L0 != c->anchors) c->anchors = *L0; continue; }
    std::optional<std::vector<Stretch>> st;
    if (c->anchors.size() >= 2) st = routeStretches(c->anchors);
    if (!st) if (auto L = bridgeAnchors(c->anchors)) st = routeStretches(*L);
    if (!st) continue;
    c->path.clear(); for (auto& s : *st) { const Item* q = sc_.item(s.c); c->path.push_back({s.c, q->anchors[s.i0], q->anchors[s.i1]}); }
    if (auto L = pathAnchors(c->path)) c->anchors = *L;
    n++;
  }
  return n;
}

int Network::mergeDuplicateCurves() {
  repairPathCurves();
  std::map<std::string, int> by; int n = 0;
  std::vector<int> ids; for (const Item* c : curves()) ids.push_back(c->id);
  for (int id : ids) {
    const Item* c = sc_.item(id);
    if (!c || !segNet(*c) || c->anchors.size() < 2 || !isNewLogic(*c)) continue;
    auto k = keyOf(c->anchors); auto f = by.find(k); if (f == by.end()) { by[k] = c->id; continue; }
    int keep = f->second, cid = c->id; bool rev = sc_.item(keep)->anchors.front() != c->anchors.front();
    for (const Item* s : surfaces()) { Item* w = sc_.item(s->id); if (w->a == cid) w->a = keep; if (w->b == cid) w->b = keep; }
    for (auto& [tid, t] : sc_.items) if (t.type == ItemType::Tube && t.raw.value("src").toInt(-1) == cid) t.raw.insert("src", keep);
    remapCurveRefs(cid, keep, [rev](double t) { return rev ? 1 - t : t; });
    for (auto& [eid, e] : sc_.items) if (e.type == ItemType::Curve) for (auto& p : e.path) if (p[0] == cid) p[0] = keep;
    dropSel(cid); deleteItem(cid); n++;
  }
  if (n) repairPathCurves();
  std::map<std::string, int> be;                             // service curves along the very same route: one
  ids.clear(); for (const Item* c : curves()) ids.push_back(c->id);
  for (int id : ids) {
    const Item* c = sc_.item(id);
    if (!c || !c->hasPath() || c->anchors.size() < 2) continue;
    auto k = keyOf(c->anchors); auto f = be.find(k); if (f == be.end()) { be[k] = c->id; continue; }
    int keep = f->second, cid = c->id;
    for (const Item* s : surfaces()) { Item* w = sc_.item(s->id); if (w->a == cid) w->a = keep; if (w->b == cid) w->b = keep; }
    for (auto& [tid, t] : sc_.items) if (t.type == ItemType::Tube && t.raw.value("src").toInt(-1) == cid) t.raw.insert("src", keep);
    dropSel(cid); deleteItem(cid); n++;
  }
  if (n) dedupeWalls();
  return n;
}

int Network::joinThroughNodes() {
  int n = 0;
  for (int guard = 0; guard < 500; guard++) {
    std::vector<const Item*> net; for (const Item* c : curves()) if (segNet(*c) && c->anchors.size() >= 2) net.push_back(c);
    std::map<int, std::vector<const Item*>> ends; std::vector<int> endOrder; std::unordered_set<int> inner;
    for (const Item* c : net) for (size_t k = 0; k < c->anchors.size(); k++) {
      int a = c->anchors[k];
      if (k == 0 || k + 1 == c->anchors.size()) { if (!ends.count(a)) endOrder.push_back(a); ends[a].push_back(c); } else inner.insert(a);
    }
    int J = -1; const Item *A = nullptr, *B = nullptr;
    for (int j : endOrder) {
      auto& cs = ends[j];
      if (cs.size() != 2 || inner.count(j) || cs[0] == cs[1] || !cs[0]->seg || !cs[1]->seg || !isNewLogic(*cs[0]) || !isNewLogic(*cs[1]) || cs[0]->parent != cs[1]->parent) continue;
      bool other = false; for (const Item* h : curves()) if (!segNet(*h) && !h->hasPath() && !h->junc && idxOf(h->anchors, j) >= 0) other = true;
      if (other) continue;
      if (leaveDir(*cs[0], j).dot(leaveDir(*cs[1], j)) > -0.3) continue;   // (a V, not a run on)
      J = j; A = cs[0]; B = cs[1]; break;
    }
    if (J < 0) break;
    Item* a = sc_.item(A->id); Item* b = sc_.item(B->id);
    std::vector<std::pair<int, std::vector<int>>> ecs; for (const Item* c : curves()) if (c->hasPath()) ecs.push_back({c->id, c->anchors});
    double LA = curveLen(*a), LB = curveLen(*b); if (!LA) LA = 1; if (!LB) LB = 1; double T = LA + LB;
    bool aEnd = a->anchors.back() == J, bStart = b->anchors.front() == J, bRev = aEnd ? !bStart : bStart;
    std::vector<int> Bs = b->anchors; if (bRev) std::reverse(Bs.begin(), Bs.end());
    OvrMap ovB;
    for (auto& [k, o] : b->ovr) ovB[k] = bRev ? HandleOverride{o.out, o.in} : o;
    std::optional<Vec3> jin, jout;
    auto oA = a->ovr.find(J); auto oB = ovB.find(J);
    if (aEnd) { if (oA != a->ovr.end()) jin = oA->second.in; if (oB != ovB.end()) jout = oB->second.out; }
    else { if (oB != ovB.end()) jin = oB->second.in; if (oA != a->ovr.end()) jout = oA->second.out; }
    auto tA = [&](double t) { return aEnd ? t * LA / T : (LB + t * LA) / T; };
    auto tB = [&](double t) { double u = bRev ? 1 - t : t; return aEnd ? (LA + u * LB) / T : u * LB / T; };
    int aid = a->id, bid = b->id;
    // (guide hits / attachments of A move first, then B's go onto A)
    for (auto& [gid, g] : sc_.items) {
      if (g.type != ItemType::Guide || g.raw.value("kind").toString() == "disc") continue;
      QJsonArray hits = g.raw.value("hits").toArray(); bool ch = false;
      for (int i = 0; i < hits.size(); i++) { QJsonObject h = hits[i].toObject(); int c = h.value("c").toInt(); if (c == aid) { h.insert("t", tA(h.value("t").toDouble())); ch = true; } else if (c == bid) { h.insert("c", aid); h.insert("t", tB(h.value("t").toDouble())); ch = true; } hits[i] = h; }
      if (ch) g.raw.insert("hits", hits);
    }
    for (auto& [xid, x] : sc_.anchors) { QJsonObject at = x.raw.value("at").toObject(); if (at.isEmpty()) continue; int c = at.value("c").toInt(); if (c == aid) at.insert("t", tA(at.value("t").toDouble())); else if (c == bid) { at.insert("c", aid); at.insert("t", tB(at.value("t").toDouble())); } else continue; x.raw.insert("at", at); }
    if (aEnd) a->anchors.insert(a->anchors.end(), Bs.begin() + 1, Bs.end()); else a->anchors.insert(a->anchors.begin(), Bs.begin(), Bs.end() - 1);
    for (auto& [k, o] : ovB) if (k != J && !a->ovr.count(k)) a->ovr[k] = o;
    if (jin || jout) a->ovr[J] = HandleOverride{jin, jout}; else a->ovr.erase(J);
    for (const Item* s : surfaces()) { Item* w = sc_.item(s->id); if (w->a == bid) w->a = aid; if (w->b == bid) w->b = aid; }
    for (auto& [tid, t] : sc_.items) if (t.type == ItemType::Tube && t.raw.value("src").toInt(-1) == bid) t.raw.insert("src", aid);
    std::set<int> skip{bid};
    for (auto& [eid, L] : ecs) {
      Item* e = sc_.item(eid); if (!e) continue;
      if (auto st = routeStretches(L, skip)) { e->path.clear(); for (auto& s : *st) { const Item* q = sc_.item(s.c); e->path.push_back({s.c, q->anchors[s.i0], q->anchors[s.i1]}); } e->anchors = pathAnchors(e->path).value_or(L); }
    }
    dropSel(bid); deleteItem(bid);
    dedupeWalls();
    sc_.reindex();
    n++;
  }
  return n;
}

// ---------- the network names ----------

std::map<int, QString> Network::netNamePlan(int gid) {
  std::map<int, QString> out; std::vector<int> outOrder;
  auto setOut = [&](int id, const QString& nm) { if (!out.count(id)) outOrder.push_back(id); out[id] = nm; };
  std::vector<const Item*> cs;
  for (const Item* c : curves()) if (c->parent == gid && c->seg && segNet(*c) && c->anchors.size() >= 2) cs.push_back(c);
  if (cs.empty() || !isNewLogic(*cs[0])) return out;
  // (the paths go by the curves' shapes as they are now — sampled afresh)
  CurveCache fresh = sampleAllCurves(sc_);
  Network fn(sc_, fresh, sel_, asel_);
  std::vector<NetPath> strands; std::set<std::string> seenP;
  for (const Item* c : cs) for (auto& p : fn.curvePaths({c->id})) { std::vector<int> k; for (auto& s : p) k.push_back(s.c); if (seenP.insert(joinIds(k)).second) strands.push_back(p); }
  auto other = [&](int cid, int a) { const Item* c = sc_.item(cid); return c->anchors.front() == a ? c->anchors.back() : c->anchors.front(); };
  auto rootP = [&](const NetPath& p) { return sc_.anchor(p.front().from)->pos; };
  auto tipP = [&](const NetPath& p) { return sc_.anchor(other(p.back().c, p.back().from))->pos; };
  auto line = [](const std::vector<Vec3>& pts) {
    Vec3 cen; for (auto& p : pts) cen += p; cen = cen / double(std::max<size_t>(pts.size(), 1));
    Vec3 ax(1, 0.001, 0.002);
    for (int k = 0; k < 20; k++) { Vec3 n; for (auto& p : pts) { Vec3 d = p - cen; n += d * d.dot(ax); } if (n.lengthSq() < 1e-20) break; ax = n.normalized(); }
    if (ax.x < 0) ax = -ax;
    return std::function<double(const Vec3&)>([cen, ax](const Vec3& p) { return (p - cen).dot(ax); });
  };
  std::vector<Vec3> rps, tps; for (auto& p : strands) { rps.push_back(rootP(p)); tps.push_back(tipP(p)); }
  auto ra = line(rps), ta = line(tps);
  std::vector<int> rootC; std::map<int, const NetPath*> firstOf, lastOf; std::vector<int> lastOrder;
  for (auto& p : strands) { if (idxOf(rootC, p.front().c) < 0) rootC.push_back(p.front().c); firstOf[p.front().c] = &p; if (!lastOf.count(p.back().c)) lastOrder.push_back(p.back().c); lastOf[p.back().c] = &p; }
  std::stable_sort(rootC.begin(), rootC.end(), [&](int a, int b) { return ra(rootP(*firstOf[a])) < ra(rootP(*firstOf[b])); });
  std::vector<int> tipC; for (int c : lastOrder) if (idxOf(rootC, c) < 0) tipC.push_back(c);
  std::stable_sort(tipC.begin(), tipC.end(), [&](int a, int b) { return ta(tipP(*lastOf[a])) < ta(tipP(*lastOf[b])); });
  std::map<int, int> nr; { int i = 1; for (int c : rootC) nr[c] = i++; for (int c : tipC) nr[c] = i++; }
  std::map<int, std::vector<int>> R, T;                       // curve → the roots / tips of the paths through it (sets, first-seen order)
  auto addU = [](std::vector<int>& v, int x) { if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x); };
  for (auto& p : strands) { int r = nr[p.front().c], t = nr[p.back().c]; for (auto& s : p) { addU(R[s.c], r); addU(T[s.c], t); } }
  auto rng = [](std::vector<int> ns) {
    std::sort(ns.begin(), ns.end()); ns.erase(std::unique(ns.begin(), ns.end()), ns.end());
    QStringList parts;
    for (size_t i = 0; i < ns.size();) { size_t j = i; while (j + 1 < ns.size() && ns[j + 1] == ns[j] + 1) j++; parts << (j > i ? QStringLiteral("%1-%2").arg(ns[i]).arg(ns[j]) : QString::number(ns[i])); i = j + 1; }
    QString s = parts.join(',');
    return s.size() <= 8 || ns.size() < 2 ? s : QStringLiteral("%1-%2").arg(ns.front()).arg(ns.back());
  };
  auto tag = [&](int c) -> std::vector<int> {
    auto r = R.count(c) ? R[c] : std::vector<int>{}, t = T.count(c) ? T[c] : std::vector<int>{};
    if (idxOf(rootC, c) >= 0) return {nr[c]}; if (r.size() >= 2) return r; if (t.size() >= 2) return t; if (nr.count(c)) return {nr[c]}; return r;
  };
  std::set<int> num; for (const Item* c : cs) if (nr.count(c->id)) num.insert(c->id);
  for (const Item* c : cs) { auto tg = tag(c->id); setOut(c->id, "C_" + (tg.empty() ? QString("x") : rng(tg))); }
  auto mid = [&](int c) { const Item* x = sc_.item(c); return sc_.anchor(x->anchors[x->anchors.size() >> 1])->pos.y; };
  auto letters = [](int i) { QString s; i++; while (i > 0) { i--; s = QChar('a' + i % 26) + s; i /= 26; } return s; };
  auto suffix = [&](ItemType type) {
    std::vector<QString> names; std::map<QString, std::vector<int>> by;
    for (int id : outOrder) if (sc_.item(id)->type == type) { if (!by.count(out[id])) names.push_back(out[id]); by[out[id]].push_back(id); }
    for (auto& nm : names) {
      auto& list = by[nm]; if (list.size() <= 1) continue;
      int keep = -1; for (int id : list) if (num.count(id)) { keep = id; break; }
      std::vector<int> rest; for (int id : list) if (id != keep) rest.push_back(id);
      if (type == ItemType::Curve) std::stable_sort(rest.begin(), rest.end(), [&](int p, int q) { return mid(q) - mid(p) < 0; });
      for (size_t i = 0; i < rest.size(); i++) out[rest[i]] = nm + letters(int(i));
    }
  };
  suffix(ItemType::Curve);
  auto along = [&](const Item& c) {
    const auto& A = c.anchors; std::vector<int> o;
    for (const Item* q : curves()) {
      if (!segNet(*q) || q == &c) continue;
      bool hit = false;
      for (size_t i = 0; i + 1 < q->anchors.size() && !hit; i++) for (size_t j = 0; j + 1 < A.size() && !hit; j++)
        if ((A[j] == q->anchors[i] && A[j + 1] == q->anchors[i + 1]) || (A[j] == q->anchors[i + 1] && A[j + 1] == q->anchors[i])) hit = true;
      if (hit) o.push_back(q->id);
    }
    return o;
  };
  auto routeOf = [&](const Item& c) { std::vector<int> o; if (!c.hasPath()) return std::vector<int>{c.id}; for (auto& p : c.path) if (const Item* s = sc_.item(p[0])) if (idxOf(s->anchors, p[1]) >= 0) o.push_back(s->id); return o; };
  auto sideTag = [&](int id) -> std::vector<int> {
    const Item* c = sc_.item(id); if (!c) return {};
    std::vector<int> parts = c->hasPath() ? routeOf(*c) : c->junc ? along(*c) : std::vector<int>{c->id};
    if (parts.empty()) return {};
    std::vector<std::vector<int>> sets; std::vector<int> all;
    for (int q : parts) { auto t = tag(q); sets.push_back(t); for (int x : t) if (idxOf(all, x) < 0) all.push_back(x); }
    std::vector<int> common; for (int x : all) { bool every = true; for (auto& s : sets) if (idxOf(s, x) < 0) every = false; if (every) common.push_back(x); }
    return common.empty() ? all : common;
  };
  for (const Item* s : surfaces()) {
    if (s->parent != gid) continue;
    auto A = sideTag(s->a), B = sideTag(s->b); if (A.empty() && B.empty()) continue;
    QString x = rng(A), y = rng(B); if (x.isEmpty()) x = "x"; if (y.isEmpty()) y = "x";
    if (x == y) {
      auto route = [&](int id) { const Item* c = sc_.item(id); return c ? routeOf(*c) : std::vector<int>{}; };
      auto ra2 = route(s->a), rb2 = route(s->b);
      auto own = [&](const std::vector<int>& r, const std::vector<int>& o) { for (int c : r) if (idxOf(o, c) < 0) { QString nm = out.count(c) ? out[c] : sc_.item(c)->name; return nm.startsWith("C_") ? nm.mid(2) : nm; } return QString(); };
      QString ox = own(ra2, rb2), oy = own(rb2, ra2); if (!ox.isEmpty()) x = ox; if (!oy.isEmpty()) y = oy;
    }
    if (naturalCompare(y, x) < 0) std::swap(x, y);
    setOut(s->id, QStringLiteral("S_%1_%2").arg(x, y));
  }
  suffix(ItemType::Surface);
  return out;
}

bool Network::autoNetNames(std::map<std::string, std::string>& netSig) {
  static const QRegularExpression NET_AUTO(
      R"(^((curve|trunk|surface|average curve)\b[\s\d.]*((copy|part|mirror|cut)[\s\d]*)*|C_[\dx,\-]+[a-z]*(\.\d+)*(\s+(copy|part|mirror)[\s\d]*)*|S_[\dxa-z,\-+]+_[\dxa-z,\-+]+(\s+(copy|part|mirror)[\s\d]*)*)$)",
      QRegularExpression::CaseInsensitiveOption);
  bool ch = false;
  std::vector<int> gs{-1}; for (auto& [id, it] : sc_.items) if (it.type == ItemType::Group) gs.push_back(id);
  std::sort(gs.begin() + 1, gs.end(), [&](int a, int b) { return sc_.item(a)->order < sc_.item(b)->order; });
  for (int gid : gs) {
    if (gid >= 0) { const Item* g = sc_.item(gid); if (g->raw.value("guidesGroup").toBool() || g->raw.value("meshesGroup").toBool() || g->raw.value("importGroup").toBool()) continue; }
    std::string key = gid >= 0 ? std::to_string(gid) : "root";
    std::vector<const Item*> cs; for (const Item* c : curves()) if (c->parent == gid && c->seg && !c->junc) cs.push_back(c);
    if (cs.empty()) { netSig.erase(key); continue; }
    std::string sig;
    for (size_t i = 0; i < cs.size(); i++) { if (i) sig += '|'; sig += std::to_string(cs[i]->id) + ':' + std::to_string(cs[i]->anchors.front()) + '-' + std::to_string(cs[i]->anchors.back()); }
    sig += '#'; bool first = true;
    for (const Item* s : surfaces()) if (s->parent == gid) { if (!first) sig += '|'; first = false; sig += std::to_string(s->id) + ':' + std::to_string(s->a) + '-' + std::to_string(s->b); }
    sig += '#'; first = true;
    for (const Item* c : curves()) if (c->hasPath() && c->parent == gid) { if (!first) sig += '|'; first = false; std::vector<int> r; for (auto& p : c->path) r.push_back(p[0]); sig += std::to_string(c->id) + ':' + joinIds(r); }
    auto f = netSig.find(key); if (f != netSig.end() && f->second == sig) continue;
    for (auto& [id, nm] : netNamePlan(gid)) { Item* it = sc_.item(id); if (it->name != nm && NET_AUTO.match(it->name.trimmed()).hasMatch()) { it->name = nm; ch = true; } }
    netSig[key] = sig;
  }
  return ch;
}

Network::Upkeep Network::upkeep(std::map<std::string, std::string>& netSig) {
  Upkeep u;
  u.merged = mergeDuplicateCurves();
  u.cut = segmentNetwork();
  u.joined = joinThroughNodes();
  u.renamed = autoNetNames(netSig);
  pruneAnchors();
  sc_.reindex();
  return u;
}

Network::Upkeep Network::loadUpkeep(std::map<std::string, std::string>& netSig) {
  Upkeep u;
  u.cut = segmentNetwork();
  u.merged = mergeDuplicateCurves();
  u.joined = joinThroughNodes();
  netSig.clear();
  u.renamed = autoNetNames(netSig);
  pruneAnchors();
  sc_.reindex();
  return u;
}

// ---------- welding, joining, inserting ----------

void Network::weldInto(int a, int b) {
  for (auto& [id, c] : sc_.items) {
    if (c.type != ItemType::Curve) continue;
    bool ch = false;
    for (int& x : c.anchors) if (x == a) {
      x = b; ch = true;
      auto o = c.ovr.find(a);
      if (o != c.ovr.end()) { HandleOverride h = o->second; if (!c.ovr.count(b)) c.ovr[b] = h; c.ovr.erase(a); }
    }
    if (ch) { std::vector<int> d; for (int x : c.anchors) if (d.empty() || d.back() != x) d.push_back(x); c.anchors = d; }
  }
  sc_.reindex();
}

// Y: the END of a curve welded to the MIDDLE of a longer one — the short one goes on along the long one to its start
std::vector<QString> Network::extendYBranches(int J) {
  std::vector<QString> out;
  for (const Item* cc : curves()) {
    Item* c = sc_.item(cc->id);
    if (c->ring >= 0 || c->closed || !c->conn.isEmpty() || c->anchors.size() < 2) continue;
    int e = c->anchors.front() == J ? 0 : c->anchors.back() == J ? 1 : -1; if (e < 0) continue;
    // (the arcs as they were before the weld: the HTML version's c.arc is the last frame's)
    auto ci = samples_.find(c->id); bool cArc = ci != samples_.end() && !ci->second.arc.empty();
    double len = cArc ? ci->second.arc.back() : 0;
    const Item* host = nullptr;
    for (const Item* h : curves()) {
      if (h == cc || h->ring >= 0 || h->closed || !h->conn.isEmpty()) continue;
      int i = idxOf(h->anchors, J); if (i <= 0 || i >= int(h->anchors.size()) - 1) continue;
      auto hi = samples_.find(h->id); bool hArc = hi != samples_.end() && !hi->second.arc.empty();
      if (!hArc || !cArc || hi->second.arc.back() > len) { host = h; break; }
    }
    if (!host) continue;
    int i = idxOf(host->anchors, J); Vec3 free = sc_.anchor(e == 0 ? c->anchors.back() : c->anchors.front())->pos;
    std::vector<int> before(host->anchors.begin(), host->anchors.begin() + i), after(host->anchors.begin() + i + 1, host->anchors.end());
    std::reverse(after.begin(), after.end());                // (both ordered far end → J)
    const auto& side = sc_.anchor(before[0])->pos.distanceTo(free) >= sc_.anchor(after[0])->pos.distanceTo(free) ? before : after;
    bool shared = false; for (int a : side) if (idxOf(c->anchors, a) >= 0) shared = true; if (shared) continue;
    std::vector<int> n;
    if (e == 0) { n = side; n.insert(n.end(), c->anchors.begin(), c->anchors.end()); }
    else { n = c->anchors; n.insert(n.end(), side.rbegin(), side.rend()); }
    c->anchors = n; out.push_back(c->name);
  }
  sc_.reindex();
  return out;
}

void Network::weld(int a, int b, const std::function<void()>& before) {
  weldInto(a, b);
  extendYBranches(b);
  if (before) before();
  segmentNetwork();
  asel_ = {b};
}

std::optional<Network::SoleEnd> Network::soleEnd(int x) const {
  const Item* only = nullptr; int n = 0;
  for (const Item* c : curves()) if (idxOf(c->anchors, x) >= 0) { only = c; n++; }
  if (n != 1 || only->ring >= 0 || only->closed || !only->conn.isEmpty() || only->anchors.size() < 2) return std::nullopt;
  if (only->anchors.front() == x) return SoleEnd{only->id, true};
  if (only->anchors.back() == x) return SoleEnd{only->id, false};
  return std::nullopt;
}

// Join into one curve: the curve ending at the joint goes on with the other one's anchors; the other one is gone
void Network::joinCurves(int a, int b) {
  auto ea = soleEnd(a), eb = soleEnd(b);
  if (!ea || !eb || ea->c == eb->c || ea->start == eb->start) { weld(a, b); return; }
  int ceId = ea->start ? eb->c : ea->c, csId = ea->start ? ea->c : eb->c;
  Item* ce = sc_.item(ceId); Item* cs = sc_.item(csId);
  double LE = curveLen(*ce), LS = curveLen(*cs), tot = LE + LS; if (!tot) tot = 1;
  weldInto(a, b);
  for (auto& [gid, g] : sc_.items) {
    if (g.type != ItemType::Guide || g.raw.value("kind").toString() == "disc") continue;
    QJsonArray hits = g.raw.value("hits").toArray();
    for (int i = 0; i < hits.size(); i++) { QJsonObject h = hits[i].toObject(); int c = h.value("c").toInt(); double t = h.value("t").toDouble();
      if (c == ceId) h.insert("t", t * LE / tot); else if (c == csId) { h.insert("c", ceId); h.insert("t", (LE + t * LS) / tot); } hits[i] = h; }
    g.raw.insert("hits", hits);
  }
  for (auto& [xid, x] : sc_.anchors) { QJsonObject at = x.raw.value("at").toObject(); if (!at.isEmpty() && at.value("c").toInt() == csId) { at.insert("c", ceId); at.insert("t", (LE + at.value("t").toDouble() * LS) / tot); x.raw.insert("at", at); } }
  ce->anchors.insert(ce->anchors.end(), cs->anchors.begin() + 1, cs->anchors.end());
  for (auto& [k, o] : cs->ovr) if (!ce->ovr.count(k)) ce->ovr[k] = o;
  for (const Item* s : surfaces()) { Item* w = sc_.item(s->id); if (w->a == csId) w->a = ceId; if (w->b == csId) w->b = ceId; }
  bool own = false; for (auto& [tid, t] : sc_.items) if (t.type == ItemType::Tube && t.raw.value("src").toInt(-1) == ceId) own = true;
  std::vector<int> killT; for (auto& [tid, t] : sc_.items) if (t.type == ItemType::Tube && t.raw.value("src").toInt(-1) == csId) { if (own) killT.push_back(tid); else t.raw.insert("src", ceId); }
  for (int t : killT) deleteItem(t);
  deleteItem(csId);
  dedupeWalls();
  sc_.reindex();
  recalcSilent({ceId});
  segmentNetwork();
  asel_.clear(); sel_.clear(); if (sc_.item(ceId)) sel_.push_back(ceId);
}

void Network::reverseCurves(const std::vector<int>& ids) {
  std::set<int> set(ids.begin(), ids.end());
  for (int id : ids) {
    Item* c = sc_.item(id); if (!c || c->type != ItemType::Curve || c->ring >= 0) continue;
    std::reverse(c->anchors.begin(), c->anchors.end());
    for (auto& [k, o] : c->ovr) std::swap(o.in, o.out);
  }
  for (auto& [gid, g] : sc_.items) {
    if (g.type == ItemType::Guide && g.raw.value("kind").toString() != "disc") {
      QJsonArray hits = g.raw.value("hits").toArray();
      for (int i = 0; i < hits.size(); i++) { QJsonObject h = hits[i].toObject(); if (set.count(h.value("c").toInt())) { h.insert("t", 1 - h.value("t").toDouble()); hits[i] = h; } }
      g.raw.insert("hits", hits);
    }
  }
  std::set<int> tubes; for (auto& [tid, t] : sc_.items) if (t.type == ItemType::Tube && set.count(t.raw.value("src").toInt(-1))) tubes.insert(tid);
  for (auto& [xid, x] : sc_.items) if (x.type == ItemType::XMod && tubes.count(x.raw.value("tube").toInt(-1)) && x.raw.value("mask").isArray()) {
    QJsonArray m = x.raw.value("mask").toArray(), r; for (int i = m.size() - 1; i >= 0; i--) r.append(m[i]); x.raw.insert("mask", r);
  }
  for (auto& [aid, a] : sc_.anchors) { QJsonObject at = a.raw.value("at").toObject(); if (!at.isEmpty() && set.count(at.value("c").toInt())) { at.insert("t", 1 - at.value("t").toDouble()); a.raw.insert("at", at); } }
  sc_.reindex();
}

// the handles of these curves' anchors automatic again, smoothness 1
void Network::recalcSilent(const std::vector<int>& cs) {
  std::set<int> ids; for (int id : cs) if (const Item* c = sc_.item(id)) ids.insert(c->anchors.begin(), c->anchors.end());
  for (auto& [id, c] : sc_.items) if (c.type == ItemType::Curve) for (int a : ids) c.ovr.erase(a);
  for (int a : ids) sc_.anchor(a)->smooth = 1;
}

// a new anchor on segment seg of curve c — and of every curve welded along that very segment
void Network::insertOnSegment(int cid, int seg, int a) {
  const Item* c = sc_.item(cid); int p = c->anchors[seg], q = c->anchors[seg + 1];
  for (const Item* hh : curves()) {
    Item* h = sc_.item(hh->id); int i = idxOf(h->anchors, p), j = idxOf(h->anchors, q);
    if (i < 0 || j < 0 || std::abs(i - j) != 1) continue;
    h->anchors.insert(h->anchors.begin() + std::max(i, j), a);
  }
  sc_.reindex();
}

// a new anchor at Bézier parameter u of segment seg — the shape kept (de Casteljau; the handles around it frozen)
int Network::insertKeepShape(int cid, int seg, double u) {
  sc_.reindex();
  const Item* c = sc_.item(cid); int p = c->anchors[seg], q = c->anchors[seg + 1];
  std::vector<int> users;
  for (const Item* h : curves()) { int i = idxOf(h->anchors, p), j = idxOf(h->anchors, q); if (i >= 0 && j >= 0 && std::abs(i - j) == 1) users.push_back(h->id); }
  Vec3 at = bez(curveSegs(sc_, *c)[seg], u);
  struct PlanK { int h, k; BezSeg s; double uu; Handles H0, H1; };
  std::vector<PlanK> plans;
  for (int hid : users) {
    const Item* h = sc_.item(hid); int i = idxOf(h->anchors, p), j = idxOf(h->anchors, q), k = std::min(i, j);
    plans.push_back({hid, k, curveSegs(sc_, *h)[k], i < j ? u : 1 - u, curveHandles(sc_, *h, k), curveHandles(sc_, *h, k + 1)});
  }
  Anchor& na = sc_.addAnchor(at); int a = na.id;
  for (auto& pl : plans) {
    Item* h = sc_.item(pl.h);
    auto l = [&](const Vec3& x, const Vec3& y) { return x.lerp(y, pl.uu); };
    Vec3 q0 = l(pl.s[0], pl.s[1]), q1 = l(pl.s[1], pl.s[2]), q2 = l(pl.s[2], pl.s[3]), r0 = l(q0, q1), r1 = l(q1, q2);
    int A0 = h->anchors[pl.k], A1 = h->anchors[pl.k + 1];
    h->anchors.insert(h->anchors.begin() + pl.k + 1, a);
    auto set = [&](int an, bool in, const Vec3& v) { const Anchor* x = sc_.anchor(an); double sm = x->smooth; if (sm < 1e-3) return; auto& o = h->ovr[an]; (in ? o.in : o.out) = (v - x->pos) / sm; };
    if (pl.k > 0) set(A0, true, pl.H0.hin); set(A0, false, q0);
    set(a, true, r0); set(a, false, r1);
    set(A1, true, q2); if (pl.k + 2 < int(h->anchors.size()) - 1) set(A1, false, pl.H1.hout);
  }
  sc_.reindex();
  return a;
}

}  // namespace hm
