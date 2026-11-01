// FBX export: binary FBX 7.4 (what Maya, 3ds Max, Blender, Unreal, Unity read). Per mesh a Model + Geometry: the polygons
// as they are (quads stay quads), Edges, the normals per polygon corner (the file's own custom normals when the mesh
// has them, else smooth ones split at the hard edges), the smoothing (per edge — soft / hard — or 3ds Max smoothing
// groups per polygon), edge creases, the UVs and the material slots. Y up, centimetres (UnitScaleFactor 1).
#include "io/Export.h"
#include <QColor>
#include <QDateTime>
#include <QSet>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

namespace hm {

namespace {

// ---------- the binary node tree ----------
struct Prop { char t; QByteArray d; };      // the type code and its little-endian payload
struct Node {
  QByteArray name; std::vector<Prop> props; std::vector<Node> kids;
  Node(const char* n, std::vector<Prop> p = {}, std::vector<Node> k = {}) : name(n), props(std::move(p)), kids(std::move(k)) {}
};
template <class T> QByteArray le(T v) { QByteArray b(int(sizeof(T)), Qt::Uninitialized); std::memcpy(b.data(), &v, sizeof(T)); return b; }
Prop pI(int32_t v) { return {'I', le(v)}; }
Prop pL(int64_t v) { return {'L', le(v)}; }
Prop pD(double v) { return {'D', le(v)}; }
Prop pC(bool v) { return {'C', QByteArray(1, v ? 1 : 0)}; }
Prop pS(const QByteArray& s) { return {'S', le(uint32_t(s.size())) + s}; }
Prop pR(const QByteArray& s) { return {'R', le(uint32_t(s.size())) + s}; }
template <class T> Prop arr(char t, const std::vector<T>& v) {
  QByteArray b = le(uint32_t(v.size())) + le(uint32_t(0)) + le(uint32_t(v.size() * sizeof(T)));
  b.append(reinterpret_cast<const char*>(v.data()), qsizetype(v.size() * sizeof(T))); return {t, b};
}
Prop pAi(const std::vector<int32_t>& v) { return arr('i', v); }
Prop pAd(const std::vector<double>& v) { return arr('d', v); }

const int SENTINEL = 13;                     // the null record closing a node's children (7.4: 32-bit offsets)
const uint32_t VERSION = 7400;
const unsigned char FILE_ID[16] = {0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2, 0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1};
const unsigned char FOOT_ID[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
const unsigned char FOOT_MAGIC[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};

// (a node's children end with a null record, and so does a node with neither properties nor children — unless it's
// the last of its siblings: the layout the FBX SDK writes)
void write(QByteArray& out, const Node& n, bool last) {
  const qsizetype at = out.size(); QByteArray props; for (auto& p : n.props) { props.append(p.t); props.append(p.d); }
  out.append(le(uint32_t(0))); out.append(le(uint32_t(n.props.size()))); out.append(le(uint32_t(props.size())));
  out.append(char(n.name.size())); out.append(n.name); out.append(props);
  for (size_t i = 0; i < n.kids.size(); i++) write(out, n.kids[i], i + 1 == n.kids.size());
  if (!n.kids.empty() || (n.props.empty() && !last)) out.append(QByteArray(SENTINEL, '\0'));
  const uint32_t end = uint32_t(out.size()); std::memcpy(out.data() + at, &end, 4);
}
QByteArray encode(const std::vector<Node>& top) {
  QByteArray out("Kaydara FBX Binary  \0\x1a\0", 23); out.append(le(VERSION));
  for (size_t i = 0; i < top.size(); i++) write(out, top[i], i + 1 == top.size());
  out.append(QByteArray(SENTINEL, '\0'));
  out.append(reinterpret_cast<const char*>(FOOT_ID), 16); out.append(QByteArray(4, '\0'));
  qsizetype pad = ((out.size() + 15) & ~qsizetype(15)) - out.size(); if (pad == 0) pad = 16;
  out.append(QByteArray(pad, '\0')); out.append(le(VERSION)); out.append(QByteArray(120, '\0'));
  out.append(reinterpret_cast<const char*>(FOOT_MAGIC), 16);
  return out;
}
Node p70(const char* name, const char* type, const char* label, const char* flags, std::vector<Prop> vals = {}) {
  std::vector<Prop> p{pS(name), pS(type), pS(label), pS(flags)}; for (auto& v : vals) p.push_back(v); return Node("P", p);
}
QByteArray cls(const QString& name, const char* c) { return name.toUtf8() + QByteArray("\0\x01", 2) + c; }
Node layerInfo(const char* type, int ver, const char* mapping, const char* ref, std::vector<Node> data) {
  std::vector<Node> k{Node("Version", {pI(ver)}), Node("Name", {pS("")}), Node("MappingInformationType", {pS(mapping)}), Node("ReferenceInformationType", {pS(ref)})};
  for (auto& d : data) k.push_back(std::move(d));
  return Node(type, {pI(0)}, std::move(k));
}

// ---------- the mesh's data ----------
inline uint64_t ekey(int a, int b) { if (a > b) std::swap(a, b); return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }
struct Dsu { std::vector<int> p; explicit Dsu(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
  int find(int x) { while (p[size_t(x)] != x) x = p[size_t(x)] = p[size_t(p[size_t(x)])]; return x; }
  void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[size_t(a)] = b; } };

struct Built { std::vector<Node> objects; std::vector<Node> conns; int models = 0, geoms = 0, mats = 0; };

void buildMesh(Built& B, const ExportMesh& em, double sc, bool groups, int64_t& nextId, QSet<QString>& used) {
  const PolyMesh& m = *em.mesh; const int nf = m.faceCount(); if (nf <= 0) return;
  const size_t nc = m.faceVerts.size();
  // the edges: each once, as the corner it starts from (FBX: an index into PolygonVertexIndex)
  std::vector<int> faceOf(nc); for (int f = 0; f < nf; f++) for (int k = m.faceStart[size_t(f)]; k < m.faceStart[size_t(f) + 1]; k++) faceOf[size_t(k)] = f;
  auto nextC = [&](int k) { int f = faceOf[size_t(k)]; return k + 1 < m.faceStart[size_t(f) + 1] ? k + 1 : m.faceStart[size_t(f)]; };
  std::unordered_map<uint64_t, int> edgeId; std::vector<int32_t> edges; std::vector<int> cornerEdge(nc);
  edgeId.reserve(nc);
  for (size_t k = 0; k < nc; k++) { uint64_t key = ekey(m.faceVerts[k], m.faceVerts[size_t(nextC(int(k)))]);
    auto r = edgeId.emplace(key, int(edges.size())); if (r.second) edges.push_back(int32_t(k)); cornerEdge[k] = r.first->second; }
  std::vector<char> hard(edges.size(), 0); std::vector<double> crease(edges.size(), 0); bool anyCrease = false;
  for (auto& e : m.hardEdges) { auto it = edgeId.find(ekey(e[0], e[1])); if (it != edgeId.end()) hard[size_t(it->second)] = 1; }
  for (auto& c : m.creases) { auto it = edgeId.find(ekey(c.a, c.b)); if (it != edgeId.end() && c.w > 0) { crease[size_t(it->second)] = std::min(1.0, double(c.w)); anyCrease = true; } }
  // (the edges along a border or shared by more than two polygons — by the polygons each edge has)
  std::vector<std::array<int, 2>> ef(edges.size(), {-1, -1}); std::vector<int> ecount(edges.size(), 0);
  for (size_t k = 0; k < nc; k++) { int e = cornerEdge[k]; if (ecount[size_t(e)] < 2) ef[size_t(e)][size_t(ecount[size_t(e)])] = faceOf[k]; ecount[size_t(e)]++; }

  // the normals: the file's own, else smooth — the corners round a point joined across the soft edges
  std::vector<double> normals; normals.reserve(nc * 3);
  if (m.cornerNormal.size() == nc) for (auto& n : m.cornerNormal) { normals.push_back(n[0]); normals.push_back(n[1]); normals.push_back(n[2]); }
  else {
    std::vector<Vec3> fn(static_cast<size_t>(nf));
    for (int f = 0; f < nf; f++) { Vec3 n; int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
      for (int k = s; k < e; k++) { const Vec3& a = m.pos[size_t(m.faceVerts[size_t(k)])]; const Vec3& b = m.pos[size_t(m.faceVerts[size_t(k + 1 < e ? k + 1 : s)])];
        n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
      fn[size_t(f)] = n; }
    Dsu u(nc);
    if (m.hardEdges.empty()) { std::vector<int> first(m.pos.size(), -1); for (size_t k = 0; k < nc; k++) { int v = m.faceVerts[k]; if (first[size_t(v)] < 0) first[size_t(v)] = int(k); else u.unite(int(k), first[size_t(v)]); } }
    else {
      // across a soft edge shared by two polygons: the corner at each of its ends joined with the other polygon's corner there
      std::unordered_map<uint64_t, int> at; at.reserve(nc);       // (polygon, point) → corner
      for (size_t k = 0; k < nc; k++) at.emplace((uint64_t(uint32_t(faceOf[k])) << 32) | uint32_t(m.faceVerts[k]), int(k));
      for (size_t k = 0; k < nc; k++) { int e = cornerEdge[k]; if (hard[size_t(e)] || ecount[size_t(e)] != 2) continue;
        int other = ef[size_t(e)][0] == faceOf[k] ? ef[size_t(e)][1] : ef[size_t(e)][0]; if (other < 0 || other == faceOf[k]) continue;
        for (int c : {int(k), nextC(int(k))}) { auto it = at.find((uint64_t(uint32_t(other)) << 32) | uint32_t(m.faceVerts[size_t(c)])); if (it != at.end()) u.unite(c, it->second); } }
    }
    std::vector<Vec3> sum(nc); for (size_t k = 0; k < nc; k++) sum[size_t(u.find(int(k)))] += fn[size_t(faceOf[k])];
    for (size_t k = 0; k < nc; k++) { Vec3 n = sum[size_t(u.find(int(k)))]; n = n.lengthSq() > 0 ? n.normalized() : Vec3(0, 1, 0); normals.push_back(n.x); normals.push_back(n.y); normals.push_back(n.z); }
  }

  std::vector<double> verts; verts.reserve(m.pos.size() * 3); for (auto& p : m.pos) { verts.push_back(p.x * sc); verts.push_back(p.y * sc); verts.push_back(p.z * sc); }
  std::vector<int32_t> pvi(nc); for (int f = 0; f < nf; f++) { int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
    for (int k = s; k < e; k++) pvi[size_t(k)] = k + 1 == e ? -(m.faceVerts[size_t(k)] + 1) : m.faceVerts[size_t(k)]; }

  std::vector<Node> geo{Node("Vertices", {pAd(verts)}), Node("PolygonVertexIndex", {pAi(pvi)}), Node("Edges", {pAi(edges)}), Node("GeometryVersion", {pI(124)})};
  std::vector<const char*> layers;
  geo.push_back(layerInfo("LayerElementNormal", 101, "ByPolygonVertex", "Direct", {Node("Normals", {pAd(normals)})})); layers.push_back("LayerElementNormal");
  if (groups) {
    // 3ds Max smoothing groups: the regions joined by soft edges, a bit each, neighbours across a hard edge apart
    Dsu u(static_cast<size_t>(nf)); for (size_t e = 0; e < edges.size(); e++) if (!hard[e] && ecount[e] == 2 && ef[e][1] >= 0) u.unite(ef[e][0], ef[e][1]);
    std::map<int, std::set<int>> nb; for (size_t e = 0; e < edges.size(); e++) if (hard[e] && ef[e][1] >= 0) { int a = u.find(ef[e][0]), b = u.find(ef[e][1]); if (a != b) { nb[a].insert(b); nb[b].insert(a); } }
    std::unordered_map<int, int32_t> bit; std::vector<int32_t> sg(static_cast<size_t>(nf));
    for (int f = 0; f < nf; f++) { int r = u.find(f); auto it = bit.find(r);
      if (it == bit.end()) { uint32_t taken = 0; for (int o : nb[r]) { auto j = bit.find(o); if (j != bit.end()) taken |= uint32_t(j->second); }
        int b = 0; while (b < 31 && (taken >> b & 1u)) b++; it = bit.emplace(r, int32_t(1u << b)).first; }
      sg[size_t(f)] = it->second; }
    bool allSoft = true; for (char h : hard) if (h) { allSoft = false; break; } if (allSoft) std::fill(sg.begin(), sg.end(), 1);
    geo.push_back(layerInfo("LayerElementSmoothing", 102, "ByPolygon", "Direct", {Node("Smoothing", {pAi(sg)})}));
  } else {
    std::vector<int32_t> s(edges.size()); for (size_t e = 0; e < edges.size(); e++) s[e] = hard[e] ? 0 : 1;
    geo.push_back(layerInfo("LayerElementSmoothing", 102, "ByEdge", "Direct", {Node("Smoothing", {pAi(s)})}));
  }
  layers.push_back("LayerElementSmoothing");
  if (anyCrease) { geo.push_back(layerInfo("LayerElementEdgeCrease", 101, "ByEdge", "Direct", {Node("EdgeCrease", {pAd(crease)})})); layers.push_back("LayerElementEdgeCrease"); }
  if (m.cornerUV.size() == nc) {
    // (the UVs shared where they are the same, as an index)
    std::map<std::pair<float, float>, int> uvAt; std::vector<double> uv; std::vector<int32_t> uvi(nc);
    for (size_t k = 0; k < nc; k++) { auto r = uvAt.emplace(std::make_pair(m.cornerUV[k][0], m.cornerUV[k][1]), int(uvAt.size()));
      if (r.second) { uv.push_back(m.cornerUV[k][0]); uv.push_back(m.cornerUV[k][1]); } uvi[k] = r.first->second; }
    Node l = layerInfo("LayerElementUV", 101, "ByPolygonVertex", "IndexToDirect", {Node("UV", {pAd(uv)}), Node("UVIndex", {pAi(uvi)})});
    l.kids[1] = Node("Name", {pS("UVMap")}); geo.push_back(std::move(l)); layers.push_back("LayerElementUV");
  }
  const int nm = std::max(1, int(m.matNames.size())); const bool perFace = int(m.faceMat.size()) == nf && m.matNames.size() > 1;
  if (perFace) { std::vector<int32_t> fm(static_cast<size_t>(nf)); for (int f = 0; f < nf; f++) fm[size_t(f)] = std::clamp(m.faceMat[size_t(f)], 0, nm - 1);
    geo.push_back(layerInfo("LayerElementMaterial", 101, "ByPolygon", "IndexToDirect", {Node("Materials", {pAi(fm)})})); }
  else geo.push_back(layerInfo("LayerElementMaterial", 101, "AllSame", "IndexToDirect", {Node("Materials", {pAi({0})})}));
  layers.push_back("LayerElementMaterial");
  std::vector<Node> layer{Node("Version", {pI(100)})};
  for (const char* t : layers) layer.push_back(Node("LayerElement", {}, {Node("Type", {pS(t)}), Node("TypedIndex", {pI(0)})}));
  geo.push_back(Node("Layer", {pI(0)}, std::move(layer)));

  QString name = em.name.trimmed().isEmpty() ? QStringLiteral("Mesh") : em.name.trimmed(); { QString n0 = name; for (int i = 2; used.contains(name); i++) name = n0 + "_" + QString::number(i); used.insert(name); }
  const int64_t model = ++nextId, geom = ++nextId;
  B.objects.push_back(Node("Geometry", {pL(geom), pS(cls(name, "Geometry")), pS("Mesh")}, std::move(geo)));
  B.objects.push_back(Node("Model", {pL(model), pS(cls(name, "Model")), pS("Mesh")}, {
    Node("Version", {pI(232)}),
    Node("Properties70", {}, {p70("Lcl Translation", "Lcl Translation", "", "A", {pD(0), pD(0), pD(0)}), p70("Lcl Rotation", "Lcl Rotation", "", "A", {pD(0), pD(0), pD(0)}),
                              p70("Lcl Scaling", "Lcl Scaling", "", "A", {pD(1), pD(1), pD(1)}), p70("DefaultAttributeIndex", "int", "Integer", "", {pI(0)}), p70("InheritType", "enum", "", "", {pI(1)})}),
    Node("MultiLayer", {pI(0)}), Node("MultiTake", {pI(0)}), Node("Shading", {pC(true)}), Node("Culling", {pS("CullingOff")})}));
  B.conns.push_back(Node("C", {pS("OO"), pL(model), pL(0)})); B.conns.push_back(Node("C", {pS("OO"), pL(geom), pL(model)}));
  for (int i = 0; i < nm; i++) {
    QString mn = size_t(i) < m.matNames.size() && !m.matNames[size_t(i)].isEmpty() ? m.matNames[size_t(i)] : name + "_mat";
    QColor c(size_t(i) < m.matColor.size() && !m.matColor[size_t(i)].isEmpty() ? m.matColor[size_t(i)] : QStringLiteral("#9a9a9a")); if (!c.isValid()) c = QColor(0x9a, 0x9a, 0x9a);
    const int64_t mat = ++nextId;
    B.objects.push_back(Node("Material", {pL(mat), pS(cls(mn, "Material")), pS("")}, {
      Node("Version", {pI(102)}), Node("ShadingModel", {pS("phong")}), Node("MultiLayer", {pI(0)}),
      Node("Properties70", {}, {p70("DiffuseColor", "Color", "", "A", {pD(c.redF()), pD(c.greenF()), pD(c.blueF())}), p70("DiffuseFactor", "Number", "", "A", {pD(1)})})}));
    B.conns.push_back(Node("C", {pS("OO"), pL(mat), pL(model)})); B.mats++;
  }
  B.models++; B.geoms++;
}

}  // namespace

QByteArray exportFbx(const std::vector<ExportMesh>& list, double sc, bool smoothGroups) {
  if (!(sc > 0)) sc = 1;
  Built B; int64_t nextId = 1000000; QSet<QString> used;
  for (auto& em : list) if (em.mesh) buildMesh(B, em, sc, smoothGroups, nextId, used);
  const QByteArray creator = "HairHarpy";
  const QDateTime now = QDateTime::currentDateTime();
  auto app = [&](const char* pre) { QByteArray p(pre); return std::vector<Node>{
    p70(pre, "Compound", "", ""), p70((p + "|ApplicationVendor").constData(), "KString", "", "", {pS("Catarackta")}),
    p70((p + "|ApplicationName").constData(), "KString", "", "", {pS(creator)}), p70((p + "|ApplicationVersion").constData(), "KString", "", "", {pS("2.0")}),
    p70((p + "|DateTime_GMT").constData(), "DateTime", "", "", {pS("01/01/1970 00:00:00.000")})}; };
  std::vector<Node> info{p70("DocumentUrl", "KString", "Url", "", {pS("/hair.fbx")}), p70("SrcDocumentUrl", "KString", "Url", "", {pS("/hair.fbx")})};
  for (auto& n : app("Original")) info.push_back(n); info.push_back(p70("Original|FileName", "KString", "", "", {pS("/hair.fbx")})); for (auto& n : app("LastSaved")) info.push_back(n);
  std::vector<Node> meta{Node("Version", {pI(100)})}; for (const char* k : {"Title", "Subject", "Author", "Keywords", "Revision", "Comment"}) meta.push_back(Node(k, {pS("")}));
  auto ip = [](const char* n, int v) { return p70(n, "int", "Integer", "", {pI(v)}); };
  auto dp = [](const char* n, double v) { return p70(n, "double", "Number", "", {pD(v)}); };
  std::vector<Node> top{
    Node("FBXHeaderExtension", {}, {
      Node("FBXHeaderVersion", {pI(1003)}), Node("FBXVersion", {pI(int32_t(VERSION))}), Node("EncryptionType", {pI(0)}),
      Node("CreationTimeStamp", {}, {Node("Version", {pI(1000)}), Node("Year", {pI(now.date().year())}), Node("Month", {pI(now.date().month())}), Node("Day", {pI(now.date().day())}),
                                     Node("Hour", {pI(now.time().hour())}), Node("Minute", {pI(now.time().minute())}), Node("Second", {pI(now.time().second())}), Node("Millisecond", {pI(now.time().msec())})}),
      Node("Creator", {pS(creator)}),
      Node("SceneInfo", {pS(QByteArray("GlobalInfo\0\x01SceneInfo", 22)), pS("UserData")}, {Node("Type", {pS("UserData")}), Node("Version", {pI(100)}), Node("MetaData", {}, meta), Node("Properties70", {}, info)})}),
    // (FileId, CreationTime and the footer go together: the constants Blender writes, which the FBX SDK accepts)
    Node("FileId", {pR(QByteArray(reinterpret_cast<const char*>(FILE_ID), 16))}),
    Node("CreationTime", {pS("1970-01-01 10:00:00:000")}),
    Node("Creator", {pS(creator)}),
    Node("GlobalSettings", {}, {Node("Version", {pI(1000)}), Node("Properties70", {}, {
      ip("UpAxis", 1), ip("UpAxisSign", 1), ip("FrontAxis", 2), ip("FrontAxisSign", 1), ip("CoordAxis", 0), ip("CoordAxisSign", 1), ip("OriginalUpAxis", 1), ip("OriginalUpAxisSign", 1),
      dp("UnitScaleFactor", 1), dp("OriginalUnitScaleFactor", 1), p70("AmbientColor", "ColorRGB", "Color", "", {pD(0), pD(0), pD(0)}),
      p70("DefaultCamera", "KString", "", "", {pS("Producer Perspective")}), p70("TimeMode", "enum", "", "", {pI(11)}),
      p70("TimeSpanStart", "KTime", "Time", "", {pL(0)}), p70("TimeSpanStop", "KTime", "Time", "", {pL(46186158000LL)}), dp("CustomFrameRate", 24)})}),
    Node("Documents", {}, {Node("Count", {pI(1)}), Node("Document", {pL(++nextId), pS(""), pS("Scene")}, {
      Node("Properties70", {}, {p70("SourceObject", "object", "", ""), p70("ActiveAnimStackName", "KString", "", "", {pS("")})}), Node("RootNode", {pL(0)})})}),
    Node("References"),
    Node("Definitions", {}, {Node("Version", {pI(100)}), Node("Count", {pI(1 + B.models + B.geoms + B.mats)}),
      Node("ObjectType", {pS("GlobalSettings")}, {Node("Count", {pI(1)})}), Node("ObjectType", {pS("Model")}, {Node("Count", {pI(B.models)})}),
      Node("ObjectType", {pS("Geometry")}, {Node("Count", {pI(B.geoms)})}), Node("ObjectType", {pS("Material")}, {Node("Count", {pI(B.mats)})})}),
    Node("Objects", {}, std::move(B.objects)),
    Node("Connections", {}, std::move(B.conns)),
    Node("Takes", {}, {Node("Current", {pS("")})})};
  return encode(top);
}

}  // namespace hm
