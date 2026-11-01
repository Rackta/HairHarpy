#include "scene/MeshMods.h"
#include "scene/Edit.h"
#include "scene/Topo.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <set>

namespace hm::meshmods {

namespace {
const double PI = 3.14159265358979323846;
double num(const QJsonObject& p, const char* k, double def) { QJsonValue v = p.value(k); return v.isDouble() ? v.toDouble() : def; }
std::vector<double> arr(const QJsonValue& v) { std::vector<double> m; for (const QJsonValue& x : v.toArray()) m.push_back(x.toDouble()); return m; }
QJsonArray toArr(const std::vector<double>& v) { QJsonArray a; for (double x : v) a.append(x); return a; }
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
// R = Rx · Ry · Rz (THREE's Euler 'XYZ')
void eulerXYZ(double x, double y, double z, double R[3][3]) {
  double a = std::cos(x), b = std::sin(x), c = std::cos(y), d = std::sin(y), e = std::cos(z), f = std::sin(z);
  double ae = a * e, af = a * f, be = b * e, bf = b * f;
  R[0][0] = c * e; R[0][1] = -c * f; R[0][2] = d;
  R[1][0] = af + be * d; R[1][1] = ae - bf * d; R[1][2] = -b * c;
  R[2][0] = bf - ae * d; R[2][1] = be + af * d; R[2][2] = a * c;
}
// the vertex normals of a mesh (area-weighted over its polygons' triangles)
std::vector<Vec3> vertexNormals(const PolyMesh& m) {
  std::vector<Vec3> n(m.pos.size());
  for (int f = 0; f < m.faceCount(); f++) { int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
    for (int k = s + 1; k + 1 < e; k++) { int a = m.faceVerts[size_t(s)], b = m.faceVerts[size_t(k)], c = m.faceVerts[size_t(k + 1)];
      Vec3 fn = (m.pos[size_t(b)] - m.pos[size_t(a)]).cross(m.pos[size_t(c)] - m.pos[size_t(a)]); n[size_t(a)] += fn; n[size_t(b)] += fn; n[size_t(c)] += fn; } }
  for (auto& v : n) v = v.normalized();
  return n;
}
const std::vector<Vec3>& baseNormals(const PolyMesh* b) {
  static std::map<const PolyMesh*, std::pair<size_t, std::vector<Vec3>>> cache;
  auto& e = cache[b]; if (e.first != b->pos.size() || e.second.size() != b->pos.size()) { e.first = b->pos.size(); e.second = vertexNormals(*b); }
  return e.second;
}
}  // namespace

std::vector<int> modsOf(const Scene& sc, int mesh) {
  std::vector<const Item*> v; for (auto& [id, it] : sc.items) if (it.type == ItemType::XMod && it.raw.value("mesh").isDouble() && it.raw.value("mesh").toInt() == mesh) v.push_back(&it);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; });
  std::vector<int> o; for (auto* x : v) o.push_back(x->id); return o;
}
Vec3 baseCenter(const Item& m) { const PolyMesh* b = baseOf(m); return b ? b->bounds().center() : Vec3(); }
double baseSize(const Item& m) { const PolyMesh* b = baseOf(m); if (!b) return 1; double s = b->bounds().size().length(); return s > 0 ? s : 1; }
// (no limit now — any mesh is edited, wired; HM_HEAVY_TRIS / HM_WIRE_TRIS set one back)
std::vector<char> hiddenFaces(const Item& m, const PolyMesh& B) {
  const QJsonArray hg = m.raw.value("hidGroups").toArray(); const QString hf = m.raw.value("hidF").toString();
  if (hg.isEmpty() && hf.isEmpty()) return {};
  std::vector<char> h = hf.isEmpty() ? std::vector<char>(size_t(B.faceCount()), 0) : topo::smDec(hf, B.faceCount());
  if (int(h.size()) != B.faceCount()) h.assign(size_t(B.faceCount()), 0);
  if (!hg.isEmpty()) { std::set<int> gs; for (const QJsonValue& v : hg) gs.insert(v.toInt());
    for (int f = 0; f < B.faceCount() && size_t(f) < B.faceGroup.size(); f++) if (gs.count(B.faceGroup[size_t(f)])) h[size_t(f)] = 1; }
  return h;
}
QByteArray hiddenSig(const Item& m) { return QJsonDocument(m.raw.value("hidGroups").toArray()).toJson(QJsonDocument::Compact) + "|" + m.raw.value("hidF").toString().toUtf8(); }

long long heavyTris() { static const long long v = qEnvironmentVariableIsSet("HM_HEAVY_TRIS") ? qEnvironmentVariable("HM_HEAVY_TRIS").toLongLong() : (1LL << 62); return v; }
bool heavy(const PolyMesh& b) { return (long long)b.faceVerts.size() - 2LL * b.faceCount() >= heavyTris(); }   // (triangles: corners − 2 per polygon)
bool heavy(const Item& m) { const PolyMesh* b = baseOf(m); return b && heavy(*b); }
long long wireTris() { static const long long v = qEnvironmentVariableIsSet("HM_WIRE_TRIS") ? qEnvironmentVariable("HM_WIRE_TRIS").toLongLong() : heavyTris(); return v; }
bool wireless(const Item& m) { const PolyMesh* b = baseOf(m); return b && (long long)b->faceVerts.size() - 2LL * b->faceCount() >= wireTris(); }

Affine xformMatrix(const QJsonObject& p, const Vec3& c0) {
  Vec3 c = p.value("cx").isDouble() ? Vec3(num(p, "cx", 0), num(p, "cy", 0), num(p, "cz", 0)) : c0;
  double R[3][3]; eulerXYZ(num(p, "rx", 0) * PI / 180, num(p, "ry", 0) * PI / 180, num(p, "rz", 0) * PI / 180, R);
  double s[3] = {num(p, "sx", 1), num(p, "sy", 1), num(p, "sz", 1)};
  Affine M; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M.L[i][j] = R[i][j] * s[j];
  M.t = c + Vec3(num(p, "px", 0), num(p, "py", 0), num(p, "pz", 0)) - M.lin(c);
  return M;
}
Affine meshMatrix(const Scene& sc, const Item& m) {
  for (int x : modsOf(sc, m.id)) { const Item* it = sc.item(x); if (it->visible && it->raw.value("kind").toString() == "xform") return xformMatrix(it->raw.value("p").toObject(), baseCenter(m)); }
  return Affine{};
}

namespace {
// a virtual symmetry (a group's raw "vsym": type, axis, count) — the copies' transforms, the original not among them
std::vector<Affine> vsymCopies(const QJsonObject& vs) {
  const QString type = vs.value("type").toString("mirror"); const int a = std::clamp(vs.value("axis").toInt(0), 0, 2), N = std::max(2, vs.value("count").toInt(6));
  auto rot = [&](double ang) { Affine R; const double c = std::cos(ang), s = std::sin(ang); const int i = (a + 1) % 3, j = (a + 2) % 3; R.L[i][i] = c; R.L[i][j] = -s; R.L[j][i] = s; R.L[j][j] = c; return R; };
  auto refl = [&](int axis) { Affine F; F.L[axis][axis] = -1; return F; };
  std::vector<Affine> out;
  // (Mirror by sides: every axis picked — a copy for each mix of them: X and Y — three)
  if (type == "mirror" && vs.value("axes").isArray() && !vs.value("axes").toArray().isEmpty()) {
    std::vector<int> ax; for (const QJsonValue& x : vs.value("axes").toArray()) { const int k = QString("xyz").indexOf(x.toString().left(1)); if (k >= 0 && std::find(ax.begin(), ax.end(), k) == ax.end()) ax.push_back(k); }
    for (int mask = 1; mask < (1 << ax.size()); mask++) { Affine T; for (size_t i = 0; i < ax.size(); i++) if (mask >> i & 1) T = Affine::mul(refl(ax[i]), T); out.push_back(T); }
    return out; }
  if (type == "mirror") out.push_back(refl(a));
  else if (type == "radial") for (int k = 1; k < N; k++) out.push_back(rot(2 * M_PI * k / N));
  else if (type == "radialHalf") for (int k = 1; k < N; k++) out.push_back(rot(M_PI * k / N));
  else { const Affine F = refl((a + 1) % 3); for (int k = 0; k < N; k++) { if (k) out.push_back(rot(2 * M_PI * k / N)); out.push_back(Affine::mul(rot(2 * M_PI * k / N), F)); } }
  return out;
}
QJsonObject vsymOf(const Scene& sc, const Item& m, const Item** group = nullptr) {
  for (const Item* x = &m; x; x = x->parent >= 0 ? sc.item(x->parent) : nullptr) if (x->type == ItemType::Group && x->raw.value("vsym").isObject()) { if (group) *group = x; return x->raw.value("vsym").toObject(); }
  return {};
}
Quat quatOfJ(const QJsonValue& v) { const QJsonArray a = v.toArray(); return a.size() == 4 ? Quat{a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()}.normalized() : Quat{}; }
Affine frameOf(const Quat& q, const Vec3& o) { Affine G; const Vec3 E[3] = {q.rotate({1, 0, 0}), q.rotate({0, 1, 0}), q.rotate({0, 0, 1})};
  for (int i = 0; i < 3; i++) { G.L[0][i] = E[i].x; G.L[1][i] = E[i].y; G.L[2][i] = E[i].z; } G.t = o; return G; }
}  // namespace

// a symmetry group's frame: the group's own point and axes (put on its object's gizmo when it goes into one; they go
// with it when it is moved / turned, and with that object)
Affine vsymFrame(const Scene& sc, const Item& g) {
  // (one put under an object before it had a point of its own: on that object's gizmo, as then)
  if (!g.raw.value("gpivot").isArray()) if (const Item* p = sc.item(g.parent); p && p->type == ItemType::IMesh && (p->base || p->poly)) {
    const Affine M = meshMatrix(sc, *p); Vec3 c = baseCenter(*p); Quat gq;
    const QJsonArray pv = p->raw.value("pivot").toArray(); if (pv.size() == 3) c = {pv[0].toDouble(), pv[1].toDouble(), pv[2].toDouble()};
    for (int x : modsOf(sc, p->id)) if (const Item* it = sc.item(x); it && it->visible && it->raw.value("kind").toString() == "xform") { const QJsonObject q = it->raw.value("p").toObject();
      if (q.value("cx").isDouble()) c = {q.value("cx").toDouble(), q.value("cy").toDouble(), q.value("cz").toDouble()}; gq = quatOfJ(q.value("gq")); }
    const Vec3 z = M.lin({0, 0, 1}), y = M.lin({0, 1, 0}); const Quat own = z.lengthSq() > 0 && y.lengthSq() > 0 ? Quat::lookRotation(-z.normalized(), y.normalized()) : Quat{};
    return frameOf((own * gq).normalized(), M.apply(c)); }
  Quat q = quatOfJ(g.raw.value("q")); for (const Item* p = g.parent >= 0 ? sc.item(g.parent) : nullptr; p; p = p->parent >= 0 ? sc.item(p->parent) : nullptr) q = (quatOfJ(p->raw.value("q")) * q).normalized();
  const QJsonArray a = g.raw.value("gpivot").toArray(); return frameOf(q, a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3());
}
// (Mirror by sides: the axes picked, each with its kept side; the seam's welding)
std::vector<std::pair<int, int>> vsymSides(const QJsonObject& vs) {
  std::vector<std::pair<int, int>> o; if (vs.value("type").toString("mirror") != "mirror") return o;
  for (const QJsonValue& x : vs.value("axes").toArray()) { const QString s = x.toString(); const int k = QString("xyz").indexOf(s.left(1)); if (k >= 0 && s.size() == 2) o.push_back({k, s[1] == '-' ? -1 : 1}); }
  return o;
}
bool vsymKeep(const QJsonObject& vs, const Vec3& l) {
  const double tol = std::max(1e-6, vs.value("weld").toBool(true) ? vs.value("weldDist").toDouble(0.05) : 0.0);
  for (auto [a, s] : vsymSides(vs)) if ((a == 0 ? l.x : a == 1 ? l.y : l.z) * s < -tol) return false;
  return true;
}
Vec3 vsymWeld(const QJsonObject& vs, Vec3 l) {
  if (!vs.value("weld").toBool(true)) return l; const double d = vs.value("weldDist").toDouble(0.05);
  for (auto [a, s] : vsymSides(vs)) { double& c = a == 0 ? l.x : a == 1 ? l.y : l.z; if (std::abs(c) <= d) c = 0; }
  return l;
}
std::vector<Affine> vsymWorld(const Scene& sc, const Item& g) {
  auto Ts = vsymCopies(g.raw.value("vsym").toObject()); const Affine G = vsymFrame(sc, g), Gi = G.inverse();
  for (auto& T : Ts) T = Affine::mul(G, Affine::mul(T, Gi)); return Ts;
}

std::vector<int> refresh(Scene& sc, std::map<int, QByteArray>& sig, const std::function<bool(int)>& weightsShown) {
  std::vector<int> changed; std::set<int> alive;
  // (a symmetry group put under an object before it had a point of its own: given its object's gizmo — where it mirrored)
  for (auto& [id, g] : sc.items) if (g.type == ItemType::Group && g.raw.value("vsym").isObject() && !g.raw.value("gpivot").isArray()) if (const Item* p = sc.item(g.parent); p && p->type == ItemType::IMesh) {
    const Affine G = vsymFrame(sc, g); const Vec3 z = G.lin({0, 0, 1}), y = G.lin({0, 1, 0}); Quat P; for (const Item* a = p; a; a = sc.item(a->parent)) P = (quatOfJ(a->raw.value("q")) * P).normalized();
    const Quat q = (P.conj() * Quat::lookRotation(-z.normalized(), y.normalized())).normalized();
    g.raw.insert("gpivot", QJsonArray{G.t.x, G.t.y, G.t.z}); g.raw.insert("q", QJsonArray{q.x, q.y, q.z, q.w}); }
  for (auto& [id, m] : sc.items) {
    if (m.type != ItemType::IMesh || (!m.poly && !m.base)) continue;
    if (!m.base) m.base = m.poly;
    alive.insert(id);
    std::vector<const Item*> ex; for (int x : modsOf(sc, id)) { const Item* it = sc.item(x); if (it->visible) ex.push_back(it); }
    bool shown = weightsShown && weightsShown(id);
    const Item* vg = nullptr; const QJsonObject vsym = vsymOf(sc, m, &vg);   // (a group's virtual symmetry: its copies shown with the mesh — about its frame)
    std::vector<Affine> vTs; if (vg) vTs = vsymWorld(sc, *vg);
    QByteArray s = QByteArray::number(quintptr(m.base.get())) + (shown ? "S" : "-") + hiddenSig(m) + QJsonDocument(vsym).toJson(QJsonDocument::Compact);
    for (auto& T : vTs) { s += QByteArray::number(T.t.x, 'g', 9) + "," + QByteArray::number(T.t.y, 'g', 9) + "," + QByteArray::number(T.t.z, 'g', 9) + ",";
      for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) s += QByteArray::number(T.L[i][j], 'g', 9) + ","; }
    for (auto* x : ex) s += QJsonDocument(x->raw).toJson(QJsonDocument::Compact);
    auto f = sig.find(id); if (f != sig.end() && f->second == s) continue;
    sig[id] = s; changed.push_back(id);
    const PolyMesh& B = *m.base; const size_t n = B.pos.size();
    const std::vector<char> hidF = hiddenFaces(m, *m.base);
    if (ex.empty() && hidF.empty() && !shown && vsym.isEmpty()) { m.poly = m.base; continue; }
    auto P = std::make_shared<PolyMesh>(); P->pos = B.pos; P->groupNames = B.groupNames;
    if (B.vcol.size() == n) P->vcol = B.vcol;                  // (the polypaint kept — a polygroup hidden, a modifier on)
    // Thickness: the weights push the surface along its normals (its mask holds them back, an older Mask modifier too)
    std::vector<std::vector<double>> masks; for (auto* x : ex) if (x->raw.value("kind").toString() == "mask") masks.push_back(arr(x->raw.value("mask")));
    std::vector<std::vector<double>> holds, thicks; std::vector<double> amts;
    for (auto* x : ex) if (x->raw.value("kind").toString() == "thick" && x->raw.value("mask").isArray()) { thicks.push_back(arr(x->raw.value("mask"))); holds.push_back(arr(x->raw.value("hold"))); amts.push_back(num(x->raw.value("p").toObject(), "amt", 0)); }
    auto weight = [&](size_t t, size_t v) { double w = v < holds[t].size() ? 1 - holds[t][v] : 1; for (auto& mk : masks) if (v < mk.size()) w *= 1 - mk[v]; return w; };
    if (!thicks.empty()) { const auto& N = baseNormals(&B);
      for (size_t t = 0; t < thicks.size(); t++) for (size_t v = 0; v < n && v < thicks[t].size(); v++) { double w = thicks[t][v]; if (!w) continue; P->pos[v] += N[v] * (w * amts[t] * weight(t, v)); } }
    // the weights shown while worked on: mask dark, thickness orange (out) / blue (in)
    if (shown) {
      auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
      const Vec3 base(lin(0x9a / 255.0), lin(0x9a / 255.0), lin(0x9a / 255.0)), fat(1, 0.55, 0.15), thin(0.2, 0.62, 1);
      for (size_t v = 0; v < n; v++) {
        double mv = 0; for (auto& mk : masks) if (v < mk.size()) mv = std::max(mv, mk[v]); for (auto& h : holds) if (v < h.size()) mv = std::max(mv, h[v]);
        double w = 0; for (auto& th : thicks) if (v < th.size()) w += th[v]; w = clamp(w, -1, 1);
        Vec3 c = base.lerp(w >= 0 ? fat : thin, std::min(1.0, std::abs(w) * 1.5)) * (1 - mv * 0.85);
        P->col.push_back({float(c.x / base.x), float(c.y / base.y), float(c.z / base.z)});   // (a tint on the mesh's own grey)
      }
    }
    Affine X = meshMatrix(sc, m); for (auto& p : P->pos) p = X.apply(p);
    // (Mirror by sides: the seam welded onto the planes, the polygons on the other sides not shown — their copies are)
    std::vector<char> cut;
    if (vg && !vsymSides(vsym).empty()) { const Affine G = vsymFrame(sc, *vg), Gi = G.inverse();
      for (auto& p : P->pos) p = G.apply(vsymWeld(vsym, Gi.apply(p)));
      cut.assign(size_t(B.faceCount()), 0);
      for (int fi = 0; fi < B.faceCount(); fi++) { Vec3 c; const int s0 = B.faceStart[size_t(fi)], e0 = B.faceStart[size_t(fi) + 1]; for (int q = s0; q < e0; q++) c += P->pos[size_t(B.faceVerts[size_t(q)])];
        if (e0 > s0 && !vsymKeep(vsym, Gi.apply(c * (1.0 / (e0 - s0))))) cut[size_t(fi)] = 1; } }
    // (the file's normals turned with it: the inverse transpose of its linear part)
    Affine NX = X.inverse(); const bool hasN = B.cornerNormal.size() == B.faceVerts.size();
    auto turnN = [&](const std::array<float, 3>& n) -> std::array<float, 3> { Vec3 v(n[0], n[1], n[2]);
      Vec3 r(NX.L[0][0] * v.x + NX.L[1][0] * v.y + NX.L[2][0] * v.z, NX.L[0][1] * v.x + NX.L[1][1] * v.y + NX.L[2][1] * v.z, NX.L[0][2] * v.x + NX.L[1][2] * v.y + NX.L[2][2] * v.z);
      if (v.lengthSq() == 0) return n; r = r.normalized(); return {float(r.x), float(r.y), float(r.z)}; };
    for (int fi = 0; fi < B.faceCount(); fi++) {
      if (!hidF.empty() && hidF[size_t(fi)]) continue;
      if (!cut.empty() && cut[size_t(fi)]) continue;
      int s0 = B.faceStart[size_t(fi)], e0 = B.faceStart[size_t(fi) + 1]; P->addFace(&B.faceVerts[size_t(s0)], e0 - s0);
      if (size_t(fi) < B.faceGroup.size()) P->faceGroup.push_back(B.faceGroup[size_t(fi)]);
      if (B.cornerUV.size() == B.faceVerts.size()) P->cornerUV.insert(P->cornerUV.end(), B.cornerUV.begin() + s0, B.cornerUV.begin() + e0);   // (its texture: UVs, material slots)
      if (hasN) for (int k = s0; k < e0; k++) P->cornerNormal.push_back(turnN(B.cornerNormal[size_t(k)]));
      if (size_t(fi) < B.faceMat.size()) P->faceMat.push_back(B.faceMat[size_t(fi)]);
    }
    P->matNames = B.matNames; P->matMaps = B.matMaps; P->matColor = B.matColor; P->matAlpha = B.matAlpha; P->flipY = B.flipY;
    P->hardEdges = B.hardEdges; P->creases = B.creases; P->seams = B.seams;
    if (!vsym.isEmpty()) {                                       // (the copies after the mesh's own vertices and polygons: its own indices stay)
      const size_t n0 = P->pos.size(); const int nf0 = P->faceCount(); const bool uv = P->cornerUV.size() == P->faceVerts.size(), cn = P->cornerNormal.size() == P->faceVerts.size() && !P->cornerNormal.empty();
      const auto& Ts = vTs; int k = 0;
      for (const Affine& T : Ts) { k++; const bool flip = T.det() < 0;
        for (size_t v = 0; v < n0; v++) P->pos.push_back(T.apply(P->pos[v]));
        if (P->vcol.size() >= n0) for (size_t v = 0; v < n0; v++) P->vcol.push_back(P->vcol[v]);
        for (int fi = 0; fi < nf0; fi++) { const int s0 = P->faceStart[size_t(fi)], e0 = P->faceStart[size_t(fi) + 1]; std::vector<int> vs; std::vector<std::array<float, 2>> us; std::vector<std::array<float, 3>> ns;
          for (int q = s0; q < e0; q++) { vs.push_back(P->faceVerts[size_t(q)] + int(n0) * k); if (uv) us.push_back(P->cornerUV[size_t(q)]);
            if (cn) { const auto& o = P->cornerNormal[size_t(q)]; Vec3 w = T.lin(Vec3(o[0], o[1], o[2])); w = w.lengthSq() > 0 ? w.normalized() : w; ns.push_back({float(w.x), float(w.y), float(w.z)}); } }
          if (flip) { std::reverse(vs.begin(), vs.end()); std::reverse(us.begin(), us.end()); std::reverse(ns.begin(), ns.end()); }
          P->addFace(vs.data(), int(vs.size())); if (uv) P->cornerUV.insert(P->cornerUV.end(), us.begin(), us.end()); if (cn) P->cornerNormal.insert(P->cornerNormal.end(), ns.begin(), ns.end());
          if (size_t(fi) < P->faceGroup.size()) P->faceGroup.push_back(P->faceGroup[size_t(fi)]); if (size_t(fi) < P->faceMat.size()) P->faceMat.push_back(P->faceMat[size_t(fi)]); } }
    }
    m.poly = P;
  }
  for (auto it = sig.begin(); it != sig.end();) { if (!alive.count(it->first)) it = sig.erase(it); else ++it; }
  return changed;
}

int newMeshExtra(Scene& sc, int mesh, const QString& kind, QString* msg) {
  Item* m = sc.item(mesh); if (!m) return -1;
  const PolyMesh* B = baseOf(*m); if (!B) return -1;
  if (heavy(*m) && kind != "xform") { if (msg) *msg = QObject::tr("%1: Over 300 000 triangles: no wireframe and not editable (it can be moved, rotated, scaled)").arg(m->name); return -1; }
  double size = baseSize(*m); QJsonObject p;
  if (kind == "xform") { p = QJsonObject{{"px", 0}, {"py", 0}, {"pz", 0}, {"rx", 0}, {"ry", 0}, {"rz", 0}, {"sx", 1}, {"sy", 1}, {"sz", 1}};
    QJsonArray pv = m->raw.value("pivot").toArray(); if (pv.size() == 3) { p.insert("cx", pv[0]); p.insert("cy", pv[1]); p.insert("cz", pv[2]); } }
  else if (kind == "thick") p = QJsonObject{{"amt", size * 0.02}, {"amtRef", size * 0.02}};
  QJsonArray zeros; if (kind == "mask" || kind == "thick") for (size_t i = 0; i < B->pos.size(); i++) zeros.append(0);
  QString names[] = {"Transform", "Thickness", "Mask"};
  QString nm = kind == "xform" ? names[0] : kind == "thick" ? names[1] : names[2];
  edit::nextNumber(sc, ItemType::XMod);
  int parent = m->parent; QString mname = m->name;
  Item& x = sc.addItem(ItemType::XMod, parent, QString("%1 (%2)").arg(nm, mname)); int id = x.id;
  x.raw = QJsonObject{{"mesh", mesh}, {"kind", kind}, {"p", p}, {"mask", zeros.isEmpty() ? QJsonValue() : QJsonValue(zeros)}, {"hold", kind == "thick" ? QJsonValue(zeros) : QJsonValue()}};
  // (after the mesh and its other modifiers)
  std::vector<int>& A = parent >= 0 && sc.item(parent) ? sc.item(parent)->children : sc.roots;
  A.erase(std::remove(A.begin(), A.end(), id), A.end());
  int at = -1; std::vector<int> after{mesh}; for (int e : modsOf(sc, mesh)) if (e != id) after.push_back(e);
  for (int a : after) { auto f = std::find(A.begin(), A.end(), a); if (f != A.end()) at = std::max(at, int(f - A.begin())); }
  A.insert(at >= 0 ? A.begin() + at + 1 : A.end(), id);
  return id;
}
int meshThick(Scene& sc, int mesh) { for (int x : modsOf(sc, mesh)) if (sc.item(x)->raw.value("kind").toString() == "thick") return x; return newMeshExtra(sc, mesh, "thick", nullptr); }

void setMeshMatrix(Scene& sc, int mesh, const Affine& W) {
  Item* m = sc.item(mesh); if (!m) return;
  int xid = -1; for (int x : modsOf(sc, mesh)) if (sc.item(x)->raw.value("kind").toString() == "xform") { xid = x; break; }
  if (xid < 0) xid = newMeshExtra(sc, mesh, "xform", nullptr); if (xid < 0) return;
  Item* x = sc.item(xid); x->visible = true;
  // decompose W = T · R · S
  Vec3 col[3]; double s[3];
  for (int j = 0; j < 3; j++) { col[j] = Vec3(W.L[0][j], W.L[1][j], W.L[2][j]); s[j] = col[j].length(); }
  if (W.det() < 0) s[0] = -s[0];
  double R[3][3]; for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) R[i][j] = s[j] ? col[j][i] / s[j] : (i == j);
  double ry = std::asin(clamp(R[0][2], -1, 1)), rx, rz;
  if (std::abs(R[0][2]) < 0.9999999) { rx = std::atan2(-R[1][2], R[2][2]); rz = std::atan2(-R[0][1], R[0][0]); } else { rx = std::atan2(R[2][1], R[1][1]); rz = 0; }
  QJsonObject p = x->raw.value("p").toObject();
  Vec3 c = p.value("cx").isDouble() ? Vec3(num(p, "cx", 0), num(p, "cy", 0), num(p, "cz", 0)) : baseCenter(*m);
  Affine RS; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) RS.L[i][j] = R[i][j] * s[j];
  Vec3 off = W.t - c + RS.lin(c);
  p.insert("px", off.x); p.insert("py", off.y); p.insert("pz", off.z);
  p.insert("rx", rx * 180 / PI); p.insert("ry", ry * 180 / PI); p.insert("rz", rz * 180 / PI);
  p.insert("sx", s[0]); p.insert("sy", s[1]); p.insert("sz", s[2]);
  x->raw.insert("p", p);
}

bool paintWeights(Scene& sc, int xmod, bool hold, const QString& op, double target, double lo, double amt, bool absMode,
                  double brushR, const std::function<double(double)>& brushWeight, const ScreenFn& screen, const Vec3& eye, bool ortho, const Vec3& camDir) {
  Item* x = sc.item(xmod); if (!x) return false; Item* m = sc.item(x->raw.value("mesh").toInt(-1));
  if (!m || !m->poly || heavy(*m) || !sc.isVisible(*m)) return false;
  const char* key = hold ? "hold" : "mask";
  std::vector<double> W = arr(x->raw.value(key)); const size_t n = m->poly->pos.size(); if (W.size() != n) W.resize(n, 0);
  const auto src = W; const auto N = vertexNormals(*m->poly);
  std::vector<std::vector<int>> adj; if (op == "smooth") { adj.resize(n); for (int f = 0; f < m->poly->faceCount(); f++) { int s0 = m->poly->faceStart[size_t(f)], e0 = m->poly->faceStart[size_t(f) + 1];
    for (int k = s0; k < e0; k++) { int a = m->poly->faceVerts[size_t(k)], b = m->poly->faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)]; adj[size_t(a)].push_back(b); adj[size_t(b)].push_back(a); } } }
  bool ch = false;
  for (size_t v = 0; v < n; v++) {
    const Vec3& p = m->poly->pos[v]; double d; if (!screen(p, d) || d >= brushR) continue;
    Vec3 view = ortho ? -camDir : eye - p; if (N[v].dot(view) < 0) continue;              // (the back side stays)
    double bw = brushWeight(d), old = W[v];
    if (op == "smooth") { double sum = src[v]; int k = 1; for (int j : adj[v]) { sum += src[size_t(j)]; k++; } W[v] = old + (sum / k - old) * bw * 0.5; }
    else if (absMode) W[v] = old + (target - old) * bw;
    else if (op == "erase") W[v] = old > 0 ? std::max(0.0, old - bw * amt) : std::min(0.0, old + bw * amt);
    else W[v] = clamp(old + target * bw * amt, lo, 1);
    if (W[v] != old) ch = true;
  }
  if (ch) x->raw.insert(key, toArr(W));
  return ch;
}

}  // namespace hm::meshmods
