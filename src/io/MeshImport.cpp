#include "io/MeshImport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <charconv>
#include <functional>
#include <map>
#include <set>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ufbx.h>
#include <unordered_map>

namespace hm {

namespace {

inline const char* skipWs(const char* p, const char* e) { while (p < e && (*p == ' ' || *p == '\t')) p++; return p; }
inline const char* parseDouble(const char* p, const char* e, double& out) {
  p = skipWs(p, e);
  auto r = std::from_chars(p, e, out);
  if (r.ec != std::errc()) { out = 0; while (p < e && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++; return p; }
  return r.ptr;
}
double f32(double x) { return double(float(x)); }                 // (the HTML version kept positions as Float32)

// a polygon list → the imported-mesh base, as the HTML version makes it (imeshBaseParsed): only the vertices the polygons
// use (in the order the corners first use them), positions welded where they fall into one cell of size × 1e-6
struct Raw { std::vector<Vec3> v; std::vector<std::array<float, 2>> vt; std::vector<int> fo{0}, fv, ft, fm, fg; std::vector<QString> mats, groups, mtllib;
             std::vector<std::array<float, 3>> vn;                 // (a normal per corner — parallel to fv — or none)
             std::vector<std::array<float, 3>> vc;                 // (polypaint: a colour per raw vertex — parallel to v — or none)
             std::vector<std::array<int, 2>> hard; std::vector<PolyMesh::Crease> crease; };   // (raw vertex indices)
std::shared_ptr<PolyMesh> baseOf(Raw& o, bool exact, QString* error) {
  const int nf = int(o.fo.size()) - 1;
  if (o.ft.size() != o.fv.size()) o.ft.assign(o.fv.size(), -1);
  if (int(o.fm.size()) != nf) o.fm.assign(size_t(std::max(nf, 0)), -1);
  if (int(o.fg.size()) != nf || o.groups.empty()) { o.fg.assign(size_t(std::max(nf, 0)), 0); o.groups = {"Polygroup 1"}; }
  else {                                                     // (a group with no polygons goes)
    std::vector<int> cnt(o.groups.size(), 0); for (int g : o.fg) cnt[size_t(g)]++;
    bool empty = false; for (int c : cnt) if (!c) empty = true;
    if (empty && nf) { std::vector<int> map(cnt.size(), -1); std::vector<QString> keep; for (size_t g = 0; g < cnt.size(); g++) if (cnt[g]) { map[g] = int(keep.size()); keep.push_back(o.groups[g]); }
      for (int& g : o.fg) g = map[size_t(g)]; o.groups = keep; }
  }
  if (nf <= 0) { if (error) *error = QStringLiteral("No polygons in the file"); return nullptr; }
  std::vector<Vec3> P; std::vector<int> fv = o.fv;
  std::vector<int> used, remap;                              // (raw vertex → used vertex → welded)
  if (exact) P = o.v;
  else {
    used.assign(o.v.size(), -1); int n0 = 0;
    for (int& i : fv) { if (used[size_t(i)] < 0) used[size_t(i)] = n0++; i = used[size_t(i)]; }
    P.resize(size_t(n0)); for (size_t i = 0; i < used.size(); i++) if (used[i] >= 0) P[size_t(used[i])] = o.v[i];
  }
  Box3 box; for (auto& p : P) box.expand(p);
  double size = box.size().length(); if (!(size > 0)) size = 1;
  auto m = std::make_shared<PolyMesh>();
  if (exact) m->pos = P;
  else {                                                     // weldTyped
    const double q = size * 1e-6; std::unordered_map<std::string, int> cell; remap.assign(P.size(), 0);
    for (size_t i = 0; i < P.size(); i++) {
      long long x = std::llround((P[i].x - box.min.x) / q), y = std::llround((P[i].y - box.min.y) / q), z = std::llround((P[i].z - box.min.z) / q);
      std::string k(reinterpret_cast<const char*>(&x), 8); k.append(reinterpret_cast<const char*>(&y), 8); k.append(reinterpret_cast<const char*>(&z), 8);
      auto it = cell.find(k); if (it != cell.end()) { remap[i] = it->second; continue; }
      int id = int(m->pos.size()); cell.emplace(std::move(k), id); m->pos.push_back(P[i]); remap[i] = id;
    }
    for (int& i : fv) i = remap[size_t(i)];
  }
  m->faceVerts = fv; m->faceStart = o.fo; m->faceGroup = o.fg; m->groupNames = o.groups;
  m->faceMat = o.fm; m->matNames = o.mats; m->mtllib = o.mtllib;
  if (!o.vt.empty()) { m->cornerUV.resize(fv.size()); for (size_t c = 0; c < fv.size(); c++) { int t = o.ft[c]; m->cornerUV[c] = t >= 0 && size_t(t) < o.vt.size() ? o.vt[size_t(t)] : std::array<float, 2>{0, 0}; } }
  if (o.vn.size() == fv.size()) m->cornerNormal = o.vn;
  if (o.vc.size() == o.v.size() && !o.vc.empty()) {             // (polypaint: each raw vertex's colour onto the vertex it became)
    m->vcol.assign(m->pos.size(), {1, 1, 1});
    for (size_t i = 0; i < o.v.size(); i++) { int t = exact ? int(i) : (used[i] >= 0 ? remap[size_t(used[i])] : -1); if (t >= 0 && size_t(t) < m->vcol.size()) m->vcol[size_t(t)] = o.vc[i]; } }
  // hard edges and creases: their raw vertices → the mesh's (an edge whose ends fell into one point goes)
  auto mapV = [&](int i) { if (exact) return i; if (i < 0 || size_t(i) >= used.size() || used[size_t(i)] < 0) return -1; return remap[size_t(used[size_t(i)])]; };
  for (auto& e : o.hard) { int a = mapV(e[0]), b = mapV(e[1]); if (a >= 0 && b >= 0 && a != b) m->hardEdges.push_back({a, b}); }
  for (auto& c : o.crease) { int a = mapV(c.a), b = mapV(c.b); if (a >= 0 && b >= 0 && a != b && c.w > 0) m->creases.push_back({a, b, c.w}); }
  return m;
}

}  // namespace

// OBJ (parseOBJFast): v / vt / f (negative indices too; repeated corners in a row dropped, a polygon of fewer than 3
// different corners dropped), usemtl / mtllib, g / o — the polygroups
std::shared_ptr<PolyMesh> parseObj(const QByteArray& text, QString* error, bool flat) {
  Raw o; std::unordered_map<std::string, int> gix; int cur = -1, gcur = -1;
  std::vector<std::array<float, 3>> vcs; bool anyVc = false; std::vector<std::array<float, 3>> mrgb;   // (polypaint: "v x y z r g b", ZBrush'"'"'s "#MRGB" blocks)
  std::vector<std::array<float, 3>> vns; std::vector<int> fn, fsg; int sg = 1; bool anyS = false;   // (vn, a corner's vn, the faces' smoothing groups)
  auto groupOf = [&](const std::string& n) { auto it = gix.find(n); if (it != gix.end()) return it->second; int k = int(o.groups.size()); o.groups.push_back(QString::fromStdString(n)); gix.emplace(n, k); return k; };
  const char* p = text.constData(); const char* end = p + text.size();
  auto rest = [](const char* s, const char* le) { s = skipWs(s, le); const char* t = le; while (t > s && (t[-1] == '\r' || t[-1] == ' ' || t[-1] == '\t')) t--; return std::string(s, t); };
  while (p < end) {
    const char* le = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p))); if (!le) le = end;
    const char* q = skipWs(p, le);
    if (q + 1 < le && q[0] == 'v' && (q[1] == ' ' || q[1] == '\t')) { Vec3 x; q = parseDouble(q + 1, le, x.x); q = parseDouble(q, le, x.y); q = parseDouble(q, le, x.z); o.v.push_back({f32(x.x), f32(x.y), f32(x.z)});
      std::array<float, 3> c{1, 1, 1}; const char* r0 = skipWs(q, le);   // (polypaint: "v x y z r g b" — 0…1, or 0…255)
      int ntok = 0; for (const char* t = r0; t < le;) { while (t < le && (*t == ' ' || *t == '\t' || *t == '\r')) t++; if (t >= le || *t == '#') break; ntok++; while (t < le && *t > 32) t++; }
      if (ntok >= 3) { double cr = -1, cg = -1, cb = -1; r0 = parseDouble(r0, le, cr); r0 = parseDouble(r0, le, cg); parseDouble(r0, le, cb);
        if (cr >= 0 && cg >= 0 && cb >= 0) { const double k = cr > 1 || cg > 1 || cb > 1 ? 1.0 / 255 : 1; c = {float(cr * k), float(cg * k), float(cb * k)}; anyVc = true; } }
      vcs.push_back(c); }
    else if (le - q >= 6 && std::strncmp(q, "#MRGB", 5) == 0) { const char* h = skipWs(q + 5, le);   // (ZBrush's polypaint: MMRRGGBB per vertex, in order)
      auto hx = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; };
      while (h + 8 <= le) { int b[8]; bool ok = true; for (int i = 0; i < 8; i++) { b[i] = hx(h[i]); if (b[i] < 0) ok = false; } if (!ok) break;
        mrgb.push_back({float(b[2] * 16 + b[3]) / 255, float(b[4] * 16 + b[5]) / 255, float(b[6] * 16 + b[7]) / 255}); h += 8; } }
    else if (q + 1 < le && q[0] == 'v' && q[1] == 'n') { double a = 0, b = 0, c = 0; q = parseDouble(q + 2, le, a); q = parseDouble(q, le, b); parseDouble(q, le, c); vns.push_back({float(a), float(b), float(c)}); }
    else if (q + 1 < le && q[0] == 's' && (q[1] == ' ' || q[1] == '\t')) { std::string v = rest(q + 1, le); anyS = true; sg = v == "off" ? 0 : std::atoi(v.c_str()); }
    else if (q + 1 < le && q[0] == 'v' && q[1] == 't') { double a = 0, b = 0; q = parseDouble(q + 2, le, a); parseDouble(q, le, b); o.vt.push_back({float(a), float(b)}); }
    else if (q + 1 < le && q[0] == 'f' && (q[1] == ' ' || q[1] == '\t')) {
      const int nv = int(o.v.size()), nt = int(o.vt.size()), nn = int(vns.size()); const size_t s = o.fv.size(); q += 1;
      while (true) {
        q = skipWs(q, le); if (q >= le || *q == '\r' || *q == '#') break;
        char* e2; long a = std::strtol(q, &e2, 10); q = e2; long t = 0;
        long n = 0;
        if (q < le && *q == '/') { q++; if (q < le && *q != '/') { t = std::strtol(q, &e2, 10); q = e2; } if (q < le && *q == '/') { q++; n = std::strtol(q, &e2, 10); q = e2; } }
        o.fv.push_back(a < 0 ? nv + int(a) : int(a) - 1); o.ft.push_back(t == 0 ? -1 : t < 0 ? nt + int(t) : int(t) - 1); fn.push_back(n == 0 ? -1 : n < 0 ? nn + int(n) : int(n) - 1);
        while (q < le && *q > 32) q++;
      }
      size_t w = s; for (size_t k = s; k < o.fv.size(); k++) { int a = o.fv[k]; if (w > s && o.fv[w - 1] == a) continue; o.fv[w] = a; o.ft[w] = o.ft[k]; fn[w] = fn[k]; w++; }
      while (w - s > 1 && o.fv[w - 1] == o.fv[s]) w--;
      int dist = 0; for (size_t k = s; k < w && dist < 3; k++) { bool seen = false; for (size_t j = s; j < k; j++) if (o.fv[j] == o.fv[k]) { seen = true; break; } if (!seen) dist++; }
      bool bad = false; for (size_t k = s; k < w; k++) if (o.fv[k] < 0 || o.fv[k] >= nv) bad = true;
      o.fv.resize(w); o.ft.resize(w); fn.resize(w);
      if (dist >= 3 && !bad) { o.fo.push_back(int(o.fv.size())); o.fm.push_back(cur); o.fg.push_back(gcur < 0 ? (gcur = groupOf("default")) : gcur); fsg.push_back(sg); }
      else { o.fv.resize(s); o.ft.resize(s); fn.resize(s); }
    }
    else if (le - q >= 6 && std::strncmp(q, "usemtl", 6) == 0) { QString n = QString::fromStdString(rest(q + 6, le)); int k = int(std::find(o.mats.begin(), o.mats.end(), n) - o.mats.begin()); if (k == int(o.mats.size())) { k = int(o.mats.size()); o.mats.push_back(n); } cur = k; }
    else if (le - q >= 6 && std::strncmp(q, "mtllib", 6) == 0) o.mtllib.push_back(QString::fromStdString(rest(q + 6, le)));
    else if (q + 1 < le && (q[0] == 'o' || q[0] == 'g') && (q[1] == ' ' || q[1] == '\t')) { std::string n = rest(q + 1, le); gcur = groupOf(n.empty() ? "default" : n); }
    p = le + 1;
  }
  if (mrgb.size() == o.v.size() && !mrgb.empty()) o.vc = mrgb; else if (anyVc && vcs.size() == o.v.size()) o.vc = vcs;   // (polypaint, when the file has it)
  if (flat) o.groups.clear();
  // the file's normals per corner (a corner without one: none — the normals are made there)
  bool anyN = false; for (int n : fn) if (n >= 0 && n < int(vns.size())) { anyN = true; break; }
  if (anyN) { o.vn.resize(o.fv.size()); for (size_t k = 0; k < o.fv.size(); k++) { int n = fn[k]; o.vn[k] = n >= 0 && n < int(vns.size()) ? vns[size_t(n)] : std::array<float, 3>{0, 0, 0}; } }
  // smoothing groups (s): an edge between faces of different groups, or of a face with none (s off), is hard
  if (anyS) {
    std::map<std::pair<int, int>, std::vector<int>> ef;
    for (size_t f = 0; f + 1 < o.fo.size(); f++) for (int k = o.fo[f]; k < o.fo[f + 1]; k++) { int a = o.fv[size_t(k)], b = o.fv[size_t(k + 1 < o.fo[f + 1] ? k + 1 : o.fo[f])]; ef[{std::min(a, b), std::max(a, b)}].push_back(int(f)); }
    for (auto& [e, fs] : ef) { bool hard = false; for (int f : fs) if (!fsg[size_t(f)]) hard = true; for (size_t i = 1; i < fs.size(); i++) if (fsg[size_t(fs[i])] != fsg[size_t(fs[0])]) hard = true;
      if (hard) o.hard.push_back({e.first, e.second}); }
  }
  return baseOf(o, false, error);
}
std::shared_ptr<PolyMesh> parseObj(const QByteArray& text, QString* error) { return parseObj(text, error, false); }

// FBX through ufbx: every mesh baked into world space (geometry_to_world), its polygons as the file has them; one
// polygroup per node, a material slot per the node's materials, the UVs per corner
std::shared_ptr<PolyMesh> parseFbx(const QByteArray& data, QString* error) {
  ufbx_load_opts opts{}; ufbx_error err{};
  ufbx_scene* scene = ufbx_load_memory(data.constData(), size_t(data.size()), &opts, &err);
  if (!scene) { if (error) *error = QStringLiteral("FBX: ") + QString::fromUtf8(err.description.data, int(err.description.length)); return nullptr; }
  Raw o; std::unordered_map<std::string, int> mids; std::vector<std::array<PolyMesh::Map, 2>> fbxMaps; std::vector<QString> fbxCol; std::vector<char> fbxAlpha;
  for (size_t ni = 0; ni < scene->nodes.count; ni++) {
    ufbx_node* node = scene->nodes.data[ni]; ufbx_mesh* mesh = node->mesh;
    if (!mesh || mesh->num_faces == 0) continue;
    int g = int(o.groups.size()); o.groups.push_back(QString::fromUtf8(node->name.data, int(node->name.length)));
    int base = int(o.v.size()); const ufbx_matrix& M = node->geometry_to_world;
    for (size_t i = 0; i < mesh->vertices.count; i++) { ufbx_vec3 w = ufbx_transform_position(&M, mesh->vertices.data[i]); o.v.push_back({f32(w.x), f32(w.y), f32(w.z)}); }
    bool flip = M.m00 * (M.m11 * M.m22 - M.m12 * M.m21) - M.m01 * (M.m10 * M.m22 - M.m12 * M.m20) + M.m02 * (M.m10 * M.m21 - M.m11 * M.m20) < 0;
    const ufbx_matrix NM = ufbx_matrix_for_normals(&M);
    // (the file's normals per corner, its hard edges and creases)
    const bool normals = mesh->vertex_normal.exists;
    if (normals && o.vn.size() != o.fv.size()) o.vn.resize(o.fv.size(), {0, 0, 0});   // (an earlier node without them)
    for (size_t f = 0; f < mesh->faces.count; f++) {
      ufbx_face fc = mesh->faces.data[f]; if (fc.num_indices < 3) continue;
      std::vector<uint32_t> ks; for (uint32_t k = 0; k < fc.num_indices; k++) ks.push_back(fc.index_begin + k);
      if (flip) std::reverse(ks.begin(), ks.end());
      for (uint32_t k : ks) {
        o.fv.push_back(base + int(mesh->vertex_indices.data[k]));
        if (mesh->vertex_color.exists) { if (o.vc.size() < o.v.size()) o.vc.resize(o.v.size(), {1, 1, 1}); const ufbx_vec4 c = ufbx_get_vertex_vec4(&mesh->vertex_color, k);   // (polypaint: the vertex colours)
          o.vc[size_t(base + int(mesh->vertex_indices.data[k]))] = {float(std::clamp(c.x, 0.0, 1.0)), float(std::clamp(c.y, 0.0, 1.0)), float(std::clamp(c.z, 0.0, 1.0))}; }
        if (normals) { ufbx_vec3 n = ufbx_transform_direction(&NM, ufbx_get_vertex_vec3(&mesh->vertex_normal, k)); double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z); if (l > 0) { n.x /= l; n.y /= l; n.z /= l; }
          o.vn.push_back({float(n.x), float(n.y), float(n.z)}); }
        else if (!o.vn.empty()) o.vn.push_back({0, 0, 0});
        if (mesh->vertex_uv.exists) { ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, k); o.ft.push_back(int(o.vt.size())); o.vt.push_back({float(uv.x), float(uv.y)}); } else o.ft.push_back(-1);
      }
      o.fo.push_back(int(o.fv.size())); o.fg.push_back(g);
      int slot = -1;
      if (mesh->face_material.count > f && mesh->materials.count) { uint32_t mi = mesh->face_material.data[f]; if (mi < mesh->materials.count) { ufbx_material* mat = mesh->materials.data[mi];
        std::string nm(mat->name.data, mat->name.length); auto it = mids.find(nm);
        if (it == mids.end()) {
          it = mids.emplace(nm, int(o.mats.size())).first; o.mats.push_back(QString::fromStdString(nm));
          // its textures: embedded (the file's own) or a file it names (matched against the images picked with it)
          auto mapOf = [](ufbx_texture* t) { PolyMesh::Map m; if (!t) return m;
            ufbx_string fn = t->relative_filename.length ? t->relative_filename : t->filename; m.name = QString::fromUtf8(fn.data, int(fn.length)).section(QRegularExpression("[\\\\/]"), -1);
            if (t->content.size) m.data = QByteArray(static_cast<const char*>(t->content.data), int(t->content.size)); return m; };
          ufbx_texture* ct = mat->pbr.base_color.texture ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture;
          ufbx_texture* at = mat->pbr.opacity.texture ? mat->pbr.opacity.texture : mat->fbx.transparency_color.texture;
          fbxMaps.push_back({mapOf(ct), mapOf(at)});
          // (its colour as FBXLoader gives it — the file's diffuse; see-through: an opacity below 1 or an opacity map)
          ufbx_vec4 dc = mat->fbx.diffuse_color.value_vec4; auto h = [](double x) { return QString("%1").arg(int(std::lround(std::clamp(x, 0.0, 1.0) * 255)), 2, 16, QChar('0')); };
          fbxCol.push_back(mat->fbx.diffuse_color.has_value ? "#" + h(dc.x) + h(dc.y) + h(dc.z) : QString());
          fbxAlpha.push_back(char(at != nullptr || (mat->pbr.opacity.has_value && mat->pbr.opacity.value_real < 1)));
        }
        slot = it->second; } }
      o.fm.push_back(slot);
    }
    for (size_t e = 0; e < mesh->edges.count; e++) {
      const ufbx_edge& E = mesh->edges.data[e]; int a = base + int(mesh->vertex_indices.data[E.a]), b = base + int(mesh->vertex_indices.data[E.b]);
      if (mesh->edge_smoothing.count > e && !mesh->edge_smoothing.data[e]) o.hard.push_back({a, b});
      if (mesh->edge_crease.count > e && mesh->edge_crease.data[e] > 0) o.crease.push_back({a, b, float(std::min<double>(1.0, mesh->edge_crease.data[e]))});
    }
    // (smoothing by polygon only: a face that isn't smooth — its edges hard)
    if (!mesh->edge_smoothing.count && mesh->face_smoothing.count == mesh->faces.count)
      for (size_t f = 0; f < mesh->faces.count; f++) if (!mesh->face_smoothing.data[f]) { ufbx_face fc = mesh->faces.data[f];
        for (uint32_t k = 0; k < fc.num_indices; k++) o.hard.push_back({base + int(mesh->vertex_indices.data[fc.index_begin + k]), base + int(mesh->vertex_indices.data[fc.index_begin + (k + 1) % fc.num_indices])}); }
  }
  if (!o.vc.empty()) o.vc.resize(o.v.size(), {1, 1, 1});
  ufbx_free_scene(scene);
  if (o.fo.size() < 2) { if (error) *error = QStringLiteral("No polygons in the FBX file"); return nullptr; }
  auto m = baseOf(o, false, error); if (m) { m->matMaps = fbxMaps; m->matColor = fbxCol; m->matAlpha = fbxAlpha; }
  return m;
}

// glTF 2.0 / GLB: every mesh primitive (triangles) under its nodes' transforms; one polygroup per mesh, a material slot per
// the glTF's materials, TEXCOORD_0 per corner (V flipped as an OBJ has it), the base colour images the file carries
std::shared_ptr<PolyMesh> parseGltf(const QByteArray& data, bool glb, QString* error) {
  QByteArray json, bin;
  if (glb) {
    if (data.size() < 20 || std::memcmp(data.constData(), "glTF", 4) != 0) { if (error) *error = QStringLiteral("Not a GLB file"); return nullptr; }
    int off = 12;
    while (off + 8 <= data.size()) {
      uint32_t len, type; std::memcpy(&len, data.constData() + off, 4); std::memcpy(&type, data.constData() + off + 4, 4);
      QByteArray chunk = data.mid(off + 8, int(len));
      if (type == 0x4E4F534A) json = chunk; else if (type == 0x004E4942) bin = chunk;
      off += 8 + int((len + 3) & ~3u);
    }
  } else json = data;
  QJsonObject G = QJsonDocument::fromJson(json).object();
  if (G.isEmpty()) { if (error) *error = QStringLiteral("glTF: unreadable"); return nullptr; }
  std::vector<QByteArray> buffers;
  for (const QJsonValue& b : G.value("buffers").toArray()) {
    QString uri = b.toObject().value("uri").toString();
    if (uri.isEmpty()) buffers.push_back(bin);
    else if (uri.startsWith("data:")) buffers.push_back(QByteArray::fromBase64(uri.mid(uri.indexOf(',') + 1).toLatin1()));
    else buffers.push_back({});                                // (an external .bin isn't kept with the project)
  }
  QJsonArray views = G.value("bufferViews").toArray(), accs = G.value("accessors").toArray();
  auto viewBytes = [&](int vi) -> QByteArray { QJsonObject v = views.at(vi).toObject(); int b = v.value("buffer").toInt(); if (b < 0 || size_t(b) >= buffers.size()) return {};
    return buffers[size_t(b)].mid(v.value("byteOffset").toInt(0), v.value("byteLength").toInt()); };
  auto readAcc = [&](int ai, int comps) {
    std::vector<double> out; QJsonObject a = accs.at(ai).toObject(); int count = a.value("count").toInt(), ct = a.value("componentType").toInt(5126);
    if (!a.contains("bufferView")) { out.assign(size_t(count * comps), 0); return out; }
    QJsonObject v = views.at(a.value("bufferView").toInt()).toObject(); QByteArray b = viewBytes(a.value("bufferView").toInt());
    int csz = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : 1, stride = v.value("byteStride").toInt(csz * comps), off = a.value("byteOffset").toInt(0);
    bool norm = a.value("normalized").toBool();
    for (int i = 0; i < count; i++) for (int c = 0; c < comps; c++) {
      int o = off + i * stride + c * csz; double x = 0; if (o + csz > b.size()) { out.push_back(0); continue; }
      const char* q = b.constData() + o;
      switch (ct) { case 5126: { float f; std::memcpy(&f, q, 4); x = f; break; } case 5125: { uint32_t u; std::memcpy(&u, q, 4); x = u; break; }
        case 5123: { uint16_t u; std::memcpy(&u, q, 2); x = norm ? u / 65535.0 : u; break; } case 5122: { int16_t u; std::memcpy(&u, q, 2); x = norm ? std::max(u / 32767.0, -1.0) : u; break; }
        case 5121: { uint8_t u = uint8_t(*q); x = norm ? u / 255.0 : u; break; } case 5120: { int8_t u = int8_t(*q); x = norm ? std::max(u / 127.0, -1.0) : u; break; } }
      out.push_back(x);
    }
    return out;
  };
  Raw o; QJsonArray meshes = G.value("meshes").toArray(), nodes = G.value("nodes").toArray();
  auto m = std::make_shared<PolyMesh>();
  // the images the file carries: a material's base colour → its colour map
  QJsonArray images = G.value("images").toArray(), textures = G.value("textures").toArray(), mats = G.value("materials").toArray();
  auto imageBytes = [&](int ti) -> PolyMesh::Map { QJsonObject t = textures.at(ti).toObject(); QJsonObject im = images.at(t.value("source").toInt(-1)).toObject();
    if (im.contains("bufferView")) return {im.value("name").toString("texture"), viewBytes(im.value("bufferView").toInt())};
    QString uri = im.value("uri").toString(); if (uri.startsWith("data:")) return {"texture", QByteArray::fromBase64(uri.mid(uri.indexOf(',') + 1).toLatin1())};
    return {uri, {}}; };
  for (int i = 0; i < mats.size(); i++) { std::array<PolyMesh::Map, 2> mm; QJsonObject pbr = mats.at(i).toObject().value("pbrMetallicRoughness").toObject();
    if (pbr.value("baseColorTexture").isObject()) mm[0] = imageBytes(pbr.value("baseColorTexture").toObject().value("index").toInt()); m->matMaps.push_back(mm); }
  // the node tree: each mesh under its world matrix
  struct M4 { double m[16]; };
  auto mul = [](const M4& a, const M4& b) { M4 r{}; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + i] * b.m[j * 4 + k]; r.m[j * 4 + i] = s; } return r; };   // (column-major)
  auto local = [&](const QJsonObject& n) { M4 r{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    if (n.value("matrix").isArray()) { QJsonArray a = n.value("matrix").toArray(); for (int i = 0; i < 16; i++) r.m[i] = a.at(i).toDouble(); return r; }
    QJsonArray T = n.value("translation").toArray(), R = n.value("rotation").toArray(), S = n.value("scale").toArray();
    double x = R.size() == 4 ? R[0].toDouble() : 0, y = R.size() == 4 ? R[1].toDouble() : 0, z = R.size() == 4 ? R[2].toDouble() : 0, w = R.size() == 4 ? R[3].toDouble() : 1;
    double sx = S.size() == 3 ? S[0].toDouble() : 1, sy = S.size() == 3 ? S[1].toDouble() : 1, sz = S.size() == 3 ? S[2].toDouble() : 1;
    double rm[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
    for (int c = 0; c < 3; c++) { double s = c == 0 ? sx : c == 1 ? sy : sz; for (int k = 0; k < 3; k++) r.m[c * 4 + k] = rm[c * 3 + k] * s; }
    if (T.size() == 3) { r.m[12] = T[0].toDouble(); r.m[13] = T[1].toDouble(); r.m[14] = T[2].toDouble(); }
    return r; };
  std::function<void(int, const M4&)> walk = [&](int ni, const M4& parent) {
    QJsonObject n = nodes.at(ni).toObject(); M4 W = mul(parent, local(n));
    if (n.contains("mesh")) {
      QJsonObject me = meshes.at(n.value("mesh").toInt()).toObject();
      const QString mname = me.value("name").toString(n.value("name").toString(QString("Mesh %1").arg(o.groups.size() + 1)));
      const int nprim = int(me.value("primitives").toArray().size()); int pi = 0;   // (a mesh of several primitives: a polygroup each — name_1, name_2…, as three.js names them)
      double det = W.m[0] * (W.m[5] * W.m[10] - W.m[9] * W.m[6]) - W.m[4] * (W.m[1] * W.m[10] - W.m[9] * W.m[2]) + W.m[8] * (W.m[1] * W.m[6] - W.m[5] * W.m[2]);
      for (const QJsonValue& pv : me.value("primitives").toArray()) {
        QJsonObject pr = pv.toObject(); pi++; if (pr.value("mode").toInt(4) != 4) continue;
        int g = int(o.groups.size()); o.groups.push_back(nprim > 1 ? QString("%1_%2").arg(mname).arg(pi) : mname);
        QJsonObject at = pr.value("attributes").toObject(); if (!at.contains("POSITION")) continue;
        auto P = readAcc(at.value("POSITION").toInt(), 3); int base = int(o.v.size()), cnt = int(P.size() / 3);
        for (int i = 0; i < cnt; i++) { double x = P[size_t(i * 3)], y = P[size_t(i * 3 + 1)], z = P[size_t(i * 3 + 2)];
          o.v.push_back({f32(W.m[0] * x + W.m[4] * y + W.m[8] * z + W.m[12]), f32(W.m[1] * x + W.m[5] * y + W.m[9] * z + W.m[13]), f32(W.m[2] * x + W.m[6] * y + W.m[10] * z + W.m[14])}); }
        std::vector<double> UV; if (at.contains("TEXCOORD_0")) UV = readAcc(at.value("TEXCOORD_0").toInt(), 2);
        int tb = int(o.vt.size()); for (int i = 0; i < cnt && !UV.empty(); i++) o.vt.push_back({float(UV[size_t(i * 2)]), float(1 - UV[size_t(i * 2 + 1)])});
        std::vector<double> I; if (pr.contains("indices")) I = readAcc(pr.value("indices").toInt(), 1); else for (int i = 0; i < cnt; i++) I.push_back(i);
        // the file's normals (per vertex → per corner), turned by the node's matrix (its inverse transpose)
        std::vector<double> NN; if (at.contains("NORMAL")) NN = readAcc(at.value("NORMAL").toInt(), 3);
        double nm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};             // (row-major: n' = nm · n)
        if (!NN.empty()) {
          double A[3][3]; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) A[r][c] = W.m[c * 4 + r];
          // cofactors: C[r][c] — the inverse transpose times the determinant (the length is normalized anyway)
          for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            nm[r * 3 + c] = A[r1][c1] * A[r2][c2] - A[r1][c2] * A[r2][c1];
          }
          if (det < 0) for (double& x : nm) x = -x;               // (a mirroring matrix: the cofactors point inward)
        }
        if (!NN.empty() && o.vn.size() != o.fv.size()) o.vn.resize(o.fv.size(), {0, 0, 0});
        int mat = pr.value("material").toInt(-1);
        for (size_t t = 0; t + 2 < I.size(); t += 3) {
          int c[3] = {int(I[t]), int(I[t + 1]), int(I[t + 2])}; if (det < 0) std::swap(c[1], c[2]);
          if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2]) continue;
          for (int k : c) { o.fv.push_back(base + k); o.ft.push_back(UV.empty() ? -1 : tb + k);
            if (!NN.empty() && size_t(k * 3 + 2) < NN.size()) { double x = NN[size_t(k * 3)], y = NN[size_t(k * 3 + 1)], z = NN[size_t(k * 3 + 2)];
              double nx = nm[0] * x + nm[1] * y + nm[2] * z, ny = nm[3] * x + nm[4] * y + nm[5] * z, nz = nm[6] * x + nm[7] * y + nm[8] * z, l = std::sqrt(nx * nx + ny * ny + nz * nz); if (l > 0) { nx /= l; ny /= l; nz /= l; }
              o.vn.push_back({float(nx), float(ny), float(nz)}); }
            else if (!o.vn.empty()) o.vn.push_back({0, 0, 0}); }
          o.fo.push_back(int(o.fv.size())); o.fm.push_back(mat); o.fg.push_back(g);
        }
      }
    }
    for (const QJsonValue& c : n.value("children").toArray()) walk(c.toInt(), W);
  };
  M4 I4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
  QJsonArray scenes = G.value("scenes").toArray(); int sc = G.value("scene").toInt(0);
  if (!scenes.isEmpty()) for (const QJsonValue& r : scenes.at(sc).toObject().value("nodes").toArray()) walk(r.toInt(), I4);
  else for (int i = 0; i < nodes.size(); i++) walk(i, I4);
  if (o.fo.size() < 2) { if (error) *error = QStringLiteral("No triangles in the glTF file"); return nullptr; }
  // the sets as the HTML version has them: one per material in the order the meshes first use them (a primitive with
  // none — three.js's default material, one set for all of them: "Material N"); the colour (baseColorFactor, linear → sRGB
  // hex) and see-through (alphaMode BLEND / MASK)
  auto srgbHex = [](double x) { x = std::clamp(x, 0.0, 1.0); double s = x <= 0.0031308 ? x * 12.92 : 1.055 * std::pow(x, 1 / 2.4) - 0.055; return QString("%1").arg(int(std::lround(s * 255)), 2, 16, QChar('0')); };
  std::map<int, int> first; std::vector<int> order;
  for (int& f : o.fm) { auto it = first.find(f); if (it == first.end()) { it = first.emplace(f, int(order.size())).first; order.push_back(f); } f = it->second; }
  std::vector<QString> names; std::vector<std::array<PolyMesh::Map, 2>> maps; std::vector<QString> cols; std::vector<char> alpha;
  for (int k : order) {
    if (k < 0 || k >= mats.size()) { names.push_back(QString("Material %1").arg(names.size() + 1)); maps.push_back({}); cols.push_back("#ffffff"); alpha.push_back(0); continue; }
    QJsonObject M = mats.at(k).toObject(); QJsonArray f = M.value("pbrMetallicRoughness").toObject().value("baseColorFactor").toArray();
    names.push_back(M.value("name").toString(QString("Material %1").arg(names.size() + 1))); maps.push_back(m->matMaps[size_t(k)]);
    cols.push_back(f.size() >= 3 ? "#" + srgbHex(f[0].toDouble()) + srgbHex(f[1].toDouble()) + srgbHex(f[2].toDouble()) : QString("#ffffff"));
    QString am = M.value("alphaMode").toString(); alpha.push_back(char(am == "BLEND" || am == "MASK"));
  }
  o.mats = names;
  auto out = baseOf(o, false, error); if (out) { out->matMaps = maps; out->matColor = cols; out->matAlpha = alpha; }
  return out;
}

// the packed meshes the HTML version writes: 'hmesh' (a decimated mesh — triangles, polygroups) and 'pmesh' (a mesh edited
// in the Topology room — its polygons, polygroups, materials, UVs)
namespace {
bool header(const QByteArray& b, uint32_t magic, QJsonObject& h, int& off) {
  if (b.size() < 8) return false; uint32_t m, hl; std::memcpy(&m, b.constData(), 4); std::memcpy(&hl, b.constData() + 4, 4);
  if (m != magic || 8 + int(hl) > b.size()) return false;
  QByteArray js = b.mid(8, int(hl)); while (!js.isEmpty() && (js.back() == '\0' || js.back() == ' ')) js.chop(1);
  h = QJsonDocument::fromJson(js).object(); off = 8 + int(hl); return true;
}
template <class T> std::vector<T> take(const QByteArray& b, int& off, size_t n) { std::vector<T> v(n); if (off + int(n * 4) <= b.size()) std::memcpy(v.data(), b.constData() + off, n * 4); off += int(n * 4); return v; }
}  // namespace
std::shared_ptr<PolyMesh> parseHMesh(const QByteArray& b, QString* error) {
  QJsonObject h; int off; if (!header(b, 0x48534d48, h, off)) { if (error) *error = QStringLiteral("Not a packed mesh"); return nullptr; }
  size_t n = size_t(h.value("n").toInt()), t = size_t(h.value("t").toInt());
  auto pos = take<float>(b, off, n * 3); auto idx = take<uint32_t>(b, off, t * 3);
  std::vector<uint16_t> tg(t); if (off + int(t * 2) <= b.size()) std::memcpy(tg.data(), b.constData() + off, t * 2);
  auto m = std::make_shared<PolyMesh>();
  for (size_t i = 0; i < n; i++) m->pos.push_back({pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]});
  for (size_t i = 0; i < t; i++) { int f[3] = {int(idx[i * 3]), int(idx[i * 3 + 1]), int(idx[i * 3 + 2])}; m->addFace(f, 3); m->faceGroup.push_back(tg[i]); }
  for (const QJsonValue& g : h.value("groups").toArray()) m->groupNames.push_back(g.toString());
  if (m->groupNames.empty()) m->groupNames = {"Polygroup 1"};
  return m;
}
std::shared_ptr<PolyMesh> parsePMesh(const QByteArray& b, QString* error) {
  QJsonObject h; int off; if (!header(b, 0x48534d50, h, off)) { if (error) *error = QStringLiteral("Not an edited mesh"); return nullptr; }
  size_t n = size_t(h.value("n").toInt()), nf = size_t(h.value("f").toInt()), c = size_t(h.value("c").toInt()), nt = size_t(h.value("nt").toInt());
  auto v = take<float>(b, off, n * 3); auto fo = take<int32_t>(b, off, nf + 1); auto fv = take<int32_t>(b, off, c); auto fg = take<int32_t>(b, off, nf);
  auto fm = take<int32_t>(b, off, nf); auto ft = take<int32_t>(b, off, c); auto vt = take<float>(b, off, nt * 2);
  Raw o; for (size_t i = 0; i < n; i++) o.v.push_back({v[i * 3], v[i * 3 + 1], v[i * 3 + 2]});
  o.fo.assign(fo.begin(), fo.end()); o.fv.assign(fv.begin(), fv.end()); o.fg.assign(fg.begin(), fg.end()); o.fm.assign(fm.begin(), fm.end()); o.ft.assign(ft.begin(), ft.end());
  for (size_t i = 0; i < nt; i++) o.vt.push_back({vt[i * 2], vt[i * 2 + 1]});
  for (const QJsonValue& g : h.value("groups").toArray()) o.groups.push_back(g.toString());
  for (const QJsonValue& g : h.value("mats").toArray()) o.mats.push_back(g.toString());
  // (this version's additions, when the file has them: normals per corner, hard edges, creases)
  size_t nn = size_t(h.value("nn").toInt()), nh = size_t(h.value("nh").toInt()), ncr = size_t(h.value("nc").toInt()), ns = size_t(h.value("ns").toInt());
  if (nn == c) { auto N = take<float>(b, off, nn * 3); for (size_t i = 0; i < nn; i++) o.vn.push_back({N[i * 3], N[i * 3 + 1], N[i * 3 + 2]}); }
  if (nh) { auto H = take<int32_t>(b, off, nh * 2); for (size_t i = 0; i < nh; i++) o.hard.push_back({H[i * 2], H[i * 2 + 1]}); }
  if (ncr) { auto C = take<int32_t>(b, off, ncr * 3); for (size_t i = 0; i < ncr; i++) { float w; std::memcpy(&w, &C[i * 3 + 2], 4); o.crease.push_back({C[i * 3], C[i * 3 + 1], w}); } }
  std::vector<std::array<int, 2>> seams;
  if (ns) { auto S = take<int32_t>(b, off, ns * 2); for (size_t i = 0; i < ns; i++) seams.push_back({S[i * 2], S[i * 2 + 1]}); }
  // (the UV Sets: their polygons, the one each shows, each set's UVs per corner)
  std::vector<QString> usn; for (const QJsonValue& x : h.value("uvsets").toArray()) usn.push_back(x.toString());
  std::vector<uint32_t> fsets; std::vector<int8_t> flive; std::vector<std::vector<std::array<float, 2>>> suv;
  if (!usn.empty() && nt == c) { auto M = take<uint32_t>(b, off, nf); auto L = take<int32_t>(b, off, nf); fsets.assign(M.begin(), M.end()); for (int32_t x : L) flive.push_back(int8_t(x));
    for (size_t s = 0; s < usn.size(); s++) { auto U = take<float>(b, off, c * 2); std::vector<std::array<float, 2>> l; l.reserve(c); for (size_t i = 0; i + 1 < U.size(); i += 2) l.push_back({U[i], U[i + 1]}); suv.push_back(std::move(l)); } }
  const size_t nvc = size_t(h.value("nvc").toInt());              // (polypaint: after the UV Sets)
  if (nvc == n && n) { auto C = take<float>(b, off, n * 3); for (size_t i = 0; i < n; i++) o.vc.push_back({C[i * 3], C[i * 3 + 1], C[i * 3 + 2]}); }
  auto r = baseOf(o, true, error);
  if (r && !usn.empty() && size_t(r->faceCount()) == nf && r->faceVerts.size() == c && fsets.size() == nf && suv.size() == usn.size() && std::all_of(suv.begin(), suv.end(), [c](auto& l) { return l.size() == c; })) {
    r->uvSetNames = usn; r->faceSets = fsets; r->faceLive = flive; r->setUV = std::move(suv); }   // (the polygons as they were — else the sets are dropped)
  if (r && !seams.empty()) { for (auto& s : seams) if (s[0] >= 0 && s[1] >= 0 && size_t(s[0]) < r->pos.size() && size_t(s[1]) < r->pos.size()) r->seams.push_back(s); }
  return r;
}

std::shared_ptr<PolyMesh> importMesh(const QString& ext, const QByteArray& data, QString* error, bool flat) {
  QString e = ext.toLower();
  std::shared_ptr<PolyMesh> m;
  if (e == "obj") m = parseObj(data, error, flat);
  else if (e == "fbx") m = parseFbx(data, error);
  else if (e == "glb" || e == "gltf") m = parseGltf(data, e == "glb", error);
  else if (e == "hmesh") m = parseHMesh(data, error);
  else if (e == "pmesh") m = parsePMesh(data, error);
  else { if (error) *error = QStringLiteral("The .%1 format is not supported").arg(e); return nullptr; }
  if (m && flat && m->groupNames.size() > 1) { std::fill(m->faceGroup.begin(), m->faceGroup.end(), 0); m->groupNames.resize(1); }   // (polygroups not imported)
  if (m && e != "pmesh" && e != "hmesh") uvSetsFromMats(*m);
  return m;
}

// materials whose UV lie over one another in the tile (as a Cinema 4D / 3DCoat model with a UVW tag per material — the head
// and the body each filling the tile) → a UV Set each; materials apart share one. The file has one UV layer: each set
// keeps the UV its own polygons have
void uvSetsFromMats(PolyMesh& m) {
  const int nf = m.faceCount(); if (nf <= 0 || m.cornerUV.size() != m.faceVerts.size() || int(m.faceMat.size()) != nf || m.matNames.size() < 2 || !m.uvSetNames.empty()) return;
  std::map<int, std::array<double, 4>> box; std::map<int, double> area;   // (each material's UV box and area)
  for (int f = 0; f < nf; f++) { const int mt = m.faceMat[size_t(f)]; auto& B = box.try_emplace(mt, std::array<double, 4>{1e30, 1e30, -1e30, -1e30}).first->second; double a = 0;
    const int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
    for (int k = s; k < e; k++) { const auto& u = m.cornerUV[size_t(k)]; const auto& w = m.cornerUV[size_t(k + 1 < e ? k + 1 : s)]; B[0] = std::min(B[0], double(u[0])); B[1] = std::min(B[1], double(u[1])); B[2] = std::max(B[2], double(u[0])); B[3] = std::max(B[3], double(u[1])); a += double(u[0]) * w[1] - double(w[0]) * u[1]; }
    area[mt] += std::abs(a) / 2; }
  if (box.size() < 2) return;
  // (two materials overlap: their boxes share over a quarter of the smaller one, and both are more than a speck)
  auto overlap = [&](int a, int b) { const auto& A = box[a]; const auto& Bb = box[b]; const double w = std::min(A[2], Bb[2]) - std::max(A[0], Bb[0]), h = std::min(A[3], Bb[3]) - std::max(A[1], Bb[1]);
    if (w <= 0 || h <= 0) return false; const double sa = (A[2] - A[0]) * (A[3] - A[1]), sb = (Bb[2] - Bb[0]) * (Bb[3] - Bb[1]); return w * h > 0.25 * std::min(sa, sb) && area[a] > 1e-6 && area[b] > 1e-6; };
  std::vector<std::vector<int>> sets;                                   // (greedy: each material into the first set it doesn't overlap)
  for (auto& [mt, B] : box) { (void)B; bool put = false;
    for (auto& S : sets) { bool clash = false; for (int x : S) if (overlap(mt, x)) clash = true; if (!clash) { S.push_back(mt); put = true; break; } }
    if (!put) sets.push_back({mt}); }
  if (sets.size() < 2 || sets.size() > 15) return;
  std::map<int, int> setOf; for (size_t s = 0; s < sets.size(); s++) for (int mt : sets[s]) setOf[mt] = int(s);
  for (size_t s = 0; s < sets.size(); s++) {                            // (named after its material — the first one's, "_Mat" off)
    const int mt = sets[s][0]; QString n = mt >= 0 && size_t(mt) < m.matNames.size() ? m.matNames[size_t(mt)] : QString("UV Set %1").arg(s + 1);
    n.remove(QRegularExpression("[_ ]?[Mm]at(erial)?$")); if (n.isEmpty()) n = QString("UV Set %1").arg(s + 1); m.uvSetNames.push_back(n); }
  m.faceSets.assign(size_t(nf), 0u); m.faceLive.assign(size_t(nf), 0); m.setUV.assign(sets.size(), m.cornerUV);
  for (int f = 0; f < nf; f++) { const int s = setOf[m.faceMat[size_t(f)]]; m.faceSets[size_t(f)] = 1u << s; m.faceLive[size_t(f)] = int8_t(s); }
}
std::shared_ptr<PolyMesh> importMesh(const QString& ext, const QByteArray& data, QString* error) { return importMesh(ext, data, error, false); }

std::vector<TexSet> texSets(const PolyMesh& m) {
  std::vector<int> used;
  if (m.matNames.empty() || int(m.faceMat.size()) != m.faceCount()) used = {-1};
  else { std::set<int> s(m.faceMat.begin(), m.faceMat.end()); used.assign(s.begin(), s.end()); }
  std::vector<TexSet> out;
  for (int k : used) out.push_back({k < 0 || size_t(k) >= m.matNames.size() ? QStringLiteral("Default") : m.matNames[size_t(k)], k,
                                   k >= 0 && size_t(k) < m.matColor.size() ? m.matColor[size_t(k)] : QString(), k >= 0 && size_t(k) < m.matAlpha.size() && m.matAlpha[size_t(k)]});
  return out;
}

void splitPartsIntoGroups(PolyMesh& m) {
  const int nv = int(m.pos.size()), nf = m.faceCount(); if (nf <= 0) return;
  std::vector<int> up(static_cast<size_t>(nv)); for (int i = 0; i < nv; i++) up[size_t(i)] = i;
  auto find = [&](int i) { while (up[size_t(i)] != i) { up[size_t(i)] = up[size_t(up[size_t(i)])]; i = up[size_t(i)]; } return i; };
  for (int f = 0; f < nf; f++) { int s = m.faceStart[size_t(f)]; for (int k = s + 1; k < m.faceStart[size_t(f) + 1]; k++) { int q = find(m.faceVerts[size_t(k)]), r = find(m.faceVerts[size_t(s)]); if (q != r) up[size_t(q)] = r; } }
  std::vector<int> pid(static_cast<size_t>(nv), -1), pf(static_cast<size_t>(nf)); int np = 0;
  for (int f = 0; f < nf; f++) { int r = find(m.faceVerts[size_t(m.faceStart[size_t(f)])]); if (pid[size_t(r)] < 0) pid[size_t(r)] = np++; pf[size_t(f)] = pid[size_t(r)]; }
  if (np < 2) return;
  if (int(m.faceGroup.size()) != nf) m.faceGroup.assign(size_t(nf), 0);
  if (m.groupNames.empty()) m.groupNames = {"Polygroup 1"};
  std::vector<int> first(static_cast<size_t>(np), -1); for (int f = 0; f < nf; f++) if (first[size_t(pf[size_t(f)])] < 0) first[size_t(pf[size_t(f)])] = f;
  const bool one = m.groupNames.size() < 2; std::unordered_map<int, int> per, seen; std::vector<QString> names;
  for (int p = 0; p < np; p++) per[m.faceGroup[size_t(first[size_t(p)])]]++;
  for (int p = 0; p < np; p++) {
    int g = m.faceGroup[size_t(first[size_t(p)])], k = ++seen[g];
    names.push_back(one ? QString("Part %1").arg(p + 1) : per[g] > 1 ? QString("%1 %2").arg(m.groupNames[size_t(g)]).arg(k) : m.groupNames[size_t(g)]);
  }
  m.faceGroup = pf; m.groupNames = names;
}

void hardFromNormals(PolyMesh& m) {
  if (m.cornerNormal.size() != m.faceVerts.size() || m.cornerNormal.empty()) return;
  // (each edge: the corners of the polygons on it at its two ends)
  struct Side { int ka, kb; }; std::map<std::pair<int, int>, std::vector<Side>> E;
  for (int f = 0; f < m.faceCount(); f++) { const int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
    for (int k = s; k < e; k++) { const int k2 = k + 1 < e ? k + 1 : s; int a = m.faceVerts[size_t(k)], b = m.faceVerts[size_t(k2)];
      if (a < b) E[{a, b}].push_back({k, k2}); else E[{b, a}].push_back({k2, k}); } }
  auto same = [&](int k1, int k2) { const auto& p = m.cornerNormal[size_t(k1)]; const auto& q = m.cornerNormal[size_t(k2)];
    const double d = double(p[0]) * q[0] + double(p[1]) * q[1] + double(p[2]) * q[2], l = std::sqrt((double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2]) * (double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2]));
    return l <= 0 || d / l > 0.9995; };                     // (within ~2°: one normal shared)
  std::set<std::pair<int, int>> H; for (auto& h : m.hardEdges) H.insert({std::min(h[0], h[1]), std::max(h[0], h[1])});
  for (auto& [e, sides] : E) { if (sides.size() != 2) continue; const Side &x = sides[0], &y = sides[1];
    if (!same(x.ka, y.ka) || !same(x.kb, y.kb)) H.insert(e); }
  m.hardEdges.clear(); for (auto& [a, b] : H) m.hardEdges.push_back({a, b});
}
void applySourceOptions(PolyMesh& m, const QJsonObject& src) {
  if (src.value("noUV").toBool()) m.cornerUV.clear();
  if (src.value("noCrease").toBool()) m.creases.clear();
  if (src.value("noPaint").toBool()) m.vcol.clear();                  // (polypaint not imported)
  if (src.value("noUV").toBool()) { m.uvSetNames.clear(); m.faceSets.clear(); m.faceLive.clear(); m.setUV.clear(); }
  if (src.value("noTex").toBool()) for (auto& mm : m.matMaps) for (auto& x : mm) x = {};
  if (src.value("noHard").toBool()) { m.hardEdges.clear(); m.cornerNormal.clear(); }
  else if (src.value("hardN").toBool() && m.hardEdges.empty()) hardFromNormals(m);
}

std::vector<std::vector<Vec3>> importCurves(const QString& ext0, const QByteArray& data) {
  std::vector<std::vector<Vec3>> out; const QString ext = ext0.toLower();
  if (ext == "obj") {                                         // (v … and l i j k …)
    std::vector<Vec3> v;
    for (const QByteArray& line : data.split('\n')) {
      const QByteArray t = line.trimmed();
      if (t.startsWith("v ")) { auto p = t.mid(2).simplified().split(' '); if (p.size() >= 3) v.push_back({p[0].toDouble(), p[1].toDouble(), p[2].toDouble()}); }
      else if (t.startsWith("l ")) { std::vector<Vec3> c;
        for (const QByteArray& w : t.mid(2).simplified().split(' ')) { int i = w.split('/').at(0).toInt(); if (i < 0) i = int(v.size()) + i + 1; if (i >= 1 && i <= int(v.size())) c.push_back(v[size_t(i - 1)]); }
        if (c.size() >= 2) out.push_back(std::move(c)); }
    }
  } else if (ext == "fbx") {
    ufbx_load_opts opts{}; ufbx_error err{};
    ufbx_scene* scene = ufbx_load_memory(data.constData(), size_t(data.size()), &opts, &err); if (!scene) return out;
    auto take = [&](const ufbx_line_curve* lc, const ufbx_matrix& M) {
      for (size_t s = 0; s < lc->segments.count; s++) { const ufbx_line_segment& sg = lc->segments.data[s]; std::vector<Vec3> c;
        for (uint32_t i = 0; i < sg.num_indices; i++) { const uint32_t pi = lc->point_indices.data[sg.index_begin + i]; if (pi >= lc->control_points.count) continue;
          const ufbx_vec3 w = ufbx_transform_position(&M, lc->control_points.data[pi]); c.push_back({w.x, w.y, w.z}); }
        if (c.size() >= 2) out.push_back(std::move(c)); } };
    for (size_t ni = 0; ni < scene->nodes.count; ni++) {
      const ufbx_node* node = scene->nodes.data[ni]; const ufbx_matrix& M = node->geometry_to_world;
      if (node->attrib_type == UFBX_ELEMENT_LINE_CURVE) if (auto* lc = ufbx_as_line_curve(node->attrib)) take(lc, M);
      if (node->attrib_type == UFBX_ELEMENT_NURBS_CURVE) if (auto* nc = ufbx_as_nurbs_curve(node->attrib)) {
        ufbx_tessellate_curve_opts to{}; to.span_subdivision = 8; ufbx_error e2{};
        if (ufbx_line_curve* lc = ufbx_tessellate_nurbs_curve(nc, &to, &e2)) { take(lc, M); ufbx_free_line_curve(lc); } }
    }
    ufbx_free_scene(scene);
  }
  return out;
}

}  // namespace hm
