#include "io/Export.h"
#include "view/SceneGeometry.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace hm {

namespace {
// +v.toFixed(6): six decimals, then as JavaScript prints the number (no trailing zeros, no "-0")
QString fx(double v) {
  double r = std::round(v * 1e6) / 1e6; if (r == 0) return "0";
  QString s = QString::number(r, 'f', 6);
  while (s.endsWith('0')) s.chop(1); if (s.endsWith('.')) s.chop(1);
  return s;
}
std::vector<std::array<int, 3>> tris(const PolyMesh& m, int f) {
  std::vector<std::array<int, 3>> t; int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
  for (int i = s + 1; i < e - 1; i++) t.push_back({m.faceVerts[size_t(s)], m.faceVerts[size_t(i)], m.faceVerts[size_t(i + 1)]});
  return t;
}
QString objName(QString n) { return n.replace(QRegularExpression("\\s+"), "_"); }
}  // namespace

QString exportExt(const QString& fmt) { return fmt; }
QString exportLabel(const QString& fmt) {
  return fmt == "obj" ? QStringLiteral("OBJ — quads") : fmt == "gltf" ? QStringLiteral("glTF — triangles") : fmt == "ply" ? QStringLiteral("PLY — quads") : fmt == "fbx" ? QStringLiteral("FBX — quads, normals, smoothing, creases") : QStringLiteral("STL — triangles");
}

std::vector<ExportMesh> exportMeshes(const Scene& sc, const std::vector<int>& sel, const QString& what) {
  std::vector<const Item*> out; std::set<int> seen;
  const bool topo = sc.room == "topo";
  auto isMesh = [&](const Item& it) { return it.poly && (it.type == ItemType::Mesh || (topo && (it.type == ItemType::IMesh || it.type == ItemType::Head))); };
  auto take = [&](const Item* it) { if (it && isMesh(*it) && seen.insert(it->id).second) out.push_back(it); };
  if (what == "all") {
    std::vector<const Item*> all; for (auto& [id, it] : sc.items) if (isMesh(it) && (topo ? sc.isVisible(it) : it.visible)) all.push_back(&it);
    std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return a->order < b->order; });
    for (auto* it : all) take(it);
  } else {
    std::function<void(int)> add = [&](int id) { const Item* it = sc.item(id); if (!it) return; take(it); if (it->type == ItemType::Group) for (int c : it->children) add(c); };
    for (int id : sel) add(id);
  }
  std::vector<ExportMesh> r; for (auto* it : out) r.push_back({it->name, it->poly.get()});
  return r;
}

QByteArray exportText(const QString& fmt, const std::vector<ExportMesh>& list, double sc, bool fbxGroups) {
  if (!(sc > 0)) sc = 1;
  if (fmt == "fbx") return exportFbx(list, sc, fbxGroups);
  if (fmt == "obj") {
    // the normals as Maya keeps them (fbxGroups off): a vn per corner, split at the hard edges; as 3ds Max (on): smoothing
    // groups — s N per region the soft edges join (a hard edge parts two regions)
    QString out = "# HairHarpy export\n"; int off = 1, offT = 1, offN = 1;
    for (auto& em : list) {
      const PolyMesh& m = *em.mesh; const int nf = m.faceCount(); const size_t nc = m.faceVerts.size();
      out += "o " + objName(em.name) + "\n";
      for (auto& p : m.pos) out += "v " + fx(p.x * sc) + " " + fx(p.y * sc) + " " + fx(p.z * sc) + "\n";
      const bool uv = m.cornerUV.size() == nc;
      std::vector<int> ti(uv ? nc : 0), ni;
      if (uv) { std::map<std::pair<float, float>, int> at;
        for (size_t k = 0; k < nc; k++) { auto r = at.emplace(std::make_pair(m.cornerUV[k][0], m.cornerUV[k][1]), int(at.size()));
          if (r.second) out += "vt " + fx(m.cornerUV[k][0]) + " " + fx(m.cornerUV[k][1]) + "\n"; ti[k] = r.first->second; }
      }
      std::vector<int> sg;
      if (!fbxGroups) {
        std::vector<Vec3> cn; if (m.cornerNormal.size() == nc) for (auto& n : m.cornerNormal) cn.push_back(Vec3(n[0], n[1], n[2])); else cn = hardCornerNormals(m);
        if (cn.size() == nc) { ni.resize(nc); std::map<std::array<double, 3>, int> at;
          for (size_t k = 0; k < nc; k++) { const Vec3 n = cn[k].lengthSq() > 0 ? cn[k].normalized() : Vec3(0, 1, 0);
            std::array<double, 3> key{std::round(n.x * 1e5), std::round(n.y * 1e5), std::round(n.z * 1e5)};
            auto r = at.emplace(key, int(at.size())); if (r.second) out += "vn " + fx(n.x) + " " + fx(n.y) + " " + fx(n.z) + "\n"; ni[k] = r.first->second; } }
      } else {
        // (the regions: polygons joined across the soft edges they share)
        std::vector<int> par(static_cast<size_t>(nf)); for (int f = 0; f < nf; f++) par[size_t(f)] = f;
        std::function<int(int)> find = [&](int x) { while (par[size_t(x)] != x) { par[size_t(x)] = par[size_t(par[size_t(x)])]; x = par[size_t(x)]; } return x; };
        std::set<std::pair<int, int>> hard; for (auto& h : m.hardEdges) hard.insert({std::min(h[0], h[1]), std::max(h[0], h[1])});
        std::map<std::pair<int, int>, int> firstFace;
        for (int f = 0; f < nf; f++) { int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
          for (int k = s; k < e; k++) { int a = m.faceVerts[size_t(k)], b = m.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; std::pair<int, int> key{std::min(a, b), std::max(a, b)};
            if (hard.count(key)) continue; auto r = firstFace.emplace(key, f); if (!r.second) { int x = find(f), y = find(r.first->second); if (x != y) par[size_t(x)] = y; } } }
        std::map<int, int> num; sg.resize(static_cast<size_t>(nf));
        for (int f = 0; f < nf; f++) { auto r = num.emplace(find(f), int(num.size()) + 1); sg[size_t(f)] = r.first->second; }
      }
      int curS = -1;
      for (int f = 0; f < nf; f++) {
        if (!sg.empty() && sg[size_t(f)] != curS) { curS = sg[size_t(f)]; out += "s " + QString::number(curS) + "\n"; }
        out += "f";
        for (int k = m.faceStart[size_t(f)]; k < m.faceStart[size_t(f) + 1]; k++) {
          out += " " + QString::number(m.faceVerts[size_t(k)] + off);
          if (uv || !ni.empty()) out += "/" + (uv ? QString::number(ti[size_t(k)] + offT) : QString());
          if (!ni.empty()) out += "/" + QString::number(ni[size_t(k)] + offN);
        }
        out += "\n";
      }
      off += int(m.pos.size());
      if (uv) offT += *std::max_element(ti.begin(), ti.end()) + 1;
      if (!ni.empty()) offN += *std::max_element(ni.begin(), ni.end()) + 1;
    }
    return out.toUtf8();
  }
  if (fmt == "ply") {                                          // one mesh (PLY has no objects)
    size_t nv = 0, nf = 0; for (auto& m : list) { nv += m.mesh->pos.size(); nf += size_t(m.mesh->faceCount()); }
    QStringList out{"ply", "format ascii 1.0", "comment HairHarpy export", QString("element vertex %1").arg(nv), "property float x", "property float y", "property float z",
                    QString("element face %1").arg(nf), "property list uchar int vertex_indices", "end_header"};
    for (auto& m : list) for (auto& p : m.mesh->pos) out << fx(p.x * sc) + " " + fx(p.y * sc) + " " + fx(p.z * sc);
    int off = 0;
    for (auto& m : list) { for (int f = 0; f < m.mesh->faceCount(); f++) { int s = m.mesh->faceStart[size_t(f)], e = m.mesh->faceStart[size_t(f) + 1]; QString l = QString::number(e - s);
        for (int k = s; k < e; k++) l += " " + QString::number(m.mesh->faceVerts[size_t(k)] + off); out << l; } off += int(m.mesh->pos.size()); }
    return (out.join("\n") + "\n").toUtf8();
  }
  if (fmt == "stl") {
    QStringList out{"solid hair"};
    for (auto& m : list) for (int f = 0; f < m.mesh->faceCount(); f++) for (auto& t : tris(*m.mesh, f)) {
      const Vec3 &A = m.mesh->pos[size_t(t[0])], &B = m.mesh->pos[size_t(t[1])], &C = m.mesh->pos[size_t(t[2])]; Vec3 n = (B - A).cross(C - A).normalized();
      out << "facet normal " + fx(n.x) + " " + fx(n.y) + " " + fx(n.z) << "outer loop";
      for (const Vec3* p : {&A, &B, &C}) out << "vertex " + fx(p->x * sc) + " " + fx(p->y * sc) + " " + fx(p->z * sc);
      out << "endloop" << "endfacet";
    }
    out << "endsolid hair"; return (out.join("\n") + "\n").toUtf8();
  }
  // glTF 2.0: a node + mesh per hair mesh — positions, smooth normals, uint32 triangles, one buffer as a data: URI
  QByteArray bin; QJsonArray views, acc, meshes, nodes;
  auto put = [&](const void* data, int bytes, int target) {
    QJsonObject v{{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes}, {"target", target}}; views.append(v);
    bin.append(static_cast<const char*>(data), bytes); while (bin.size() % 4) bin.append('\0');
    return views.size() - 1;
  };
  for (auto& m : list) {
    const auto& P = m.mesh->pos; std::vector<Vec3> N(P.size()); std::vector<uint32_t> idx;
    for (int f = 0; f < m.mesh->faceCount(); f++) for (auto& t : tris(*m.mesh, f)) {
      Vec3 n = (P[size_t(t[1])] - P[size_t(t[0])]).cross(P[size_t(t[2])] - P[size_t(t[0])]); for (int k : t) { N[size_t(k)] += n; idx.push_back(uint32_t(k)); } }
    std::vector<float> pos(P.size() * 3), nor(P.size() * 3); double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (size_t i = 0; i < P.size(); i++) {
      double c[3] = {P[i].x * sc, P[i].y * sc, P[i].z * sc};
      for (int k = 0; k < 3; k++) { float v = float(c[k]); pos[i * 3 + size_t(k)] = v; lo[k] = std::min(lo[k], double(v)); hi[k] = std::max(hi[k], double(v)); }
      Vec3 n = N[i].length() > 0 ? N[i].normalized() : Vec3(0, 1, 0); nor[i * 3] = float(n.x); nor[i * 3 + 1] = float(n.y); nor[i * 3 + 2] = float(n.z);
    }
    int vp = put(pos.data(), int(pos.size() * 4), 34962), vn = put(nor.data(), int(nor.size() * 4), 34962), vi = put(idx.data(), int(idx.size() * 4), 34963), a0 = acc.size();
    acc.append(QJsonObject{{"bufferView", vp}, {"componentType", 5126}, {"count", int(P.size())}, {"type", "VEC3"}, {"min", QJsonArray{lo[0], lo[1], lo[2]}}, {"max", QJsonArray{hi[0], hi[1], hi[2]}}});
    acc.append(QJsonObject{{"bufferView", vn}, {"componentType", 5126}, {"count", int(P.size())}, {"type", "VEC3"}});
    acc.append(QJsonObject{{"bufferView", vi}, {"componentType", 5125}, {"count", int(idx.size())}, {"type", "SCALAR"}});
    meshes.append(QJsonObject{{"name", m.name}, {"primitives", QJsonArray{QJsonObject{{"attributes", QJsonObject{{"POSITION", a0}, {"NORMAL", a0 + 1}}}, {"indices", a0 + 2}}}}});
    nodes.append(QJsonObject{{"name", m.name}, {"mesh", meshes.size() - 1}});
  }
  QJsonArray sn; for (int i = 0; i < nodes.size(); i++) sn.append(i);
  QJsonObject G{{"asset", QJsonObject{{"version", "2.0"}, {"generator", "HairHarpy"}}}, {"scene", 0}, {"scenes", QJsonArray{QJsonObject{{"nodes", sn}}}},
                {"nodes", nodes}, {"meshes", meshes}, {"accessors", acc}, {"bufferViews", views},
                {"buffers", QJsonArray{QJsonObject{{"byteLength", bin.size()}, {"uri", "data:application/octet-stream;base64," + QString::fromLatin1(bin.toBase64())}}}}};
  return QJsonDocument(G).toJson(QJsonDocument::Compact);
}

QByteArray bridgePayload(const std::vector<ExportMesh>& list, double sc, bool replace) {
  if (!(sc > 0)) sc = 1;
  QByteArray o = "{\"app\":\"hmc\",\"v\":1,\"replace\":" + QByteArray(replace ? "true" : "false") + ",\"up\":\"y\",\"objects\":[";
  for (size_t i = 0; i < list.size(); i++) {
    const PolyMesh& m = *list[i].mesh; if (i) o += ",";
    o += "{\"name\":" + QJsonDocument(QJsonArray{list[i].name}).toJson(QJsonDocument::Compact).mid(1).chopped(1) + ",\"pos\":[";
    for (size_t k = 0; k < m.pos.size(); k++) { if (k) o += ","; o += (fx(m.pos[k].x * sc) + "," + fx(m.pos[k].y * sc) + "," + fx(m.pos[k].z * sc)).toLatin1(); }
    o += "],\"faces\":[";
    for (int f = 0; f < m.faceCount(); f++) { if (f) o += ","; o += "["; for (int k = m.faceStart[size_t(f)]; k < m.faceStart[size_t(f) + 1]; k++) { if (k > m.faceStart[size_t(f)]) o += ","; o += QByteArray::number(m.faceVerts[size_t(k)]); } o += "]"; }
    o += "]}";
  }
  return o + "]}";
}

}  // namespace hm
