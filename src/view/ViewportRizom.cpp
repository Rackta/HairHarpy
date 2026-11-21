// The UV sent to RizomUV: the active mesh written as an OBJ and a Lua script RizomUV runs at its start (its UV Mats as
// tiles, the stacked shells as groups Pack keeps stacked, the pins, the texture's padding); (its UV kept where it has one — a corner's UV point shared
// where the UV runs on, so RizomUV sees the same shells), RizomUV started with it; when it saves the file again the UV
// comes back onto the mesh (one undo step) — as often as it is saved. RizomUV is looked for in "Rizom Lab" at the root
// of every drive and in Program Files (the newest), else asked for once (Shift + the button — asked again).
#include "scene/Uv.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <map>
#include <functional>
#include <set>
#include <tuple>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
// RizomUV's program: the one kept, else the newest in a "Rizom Lab" folder (a drive's root, Program Files)
QString findRizom() {
  const QString kept = QSettings().value("rizom/exe").toString(); if (!kept.isEmpty() && QFileInfo::exists(kept)) return kept;
  QString best; QDateTime bt;
  for (const QFileInfo& drive : QDir::drives()) for (const QString& sub : {QString("Rizom Lab"), QString("Program Files/Rizom Lab"), QString("Program Files (x86)/Rizom Lab")}) {
    QDir d(drive.absolutePath() + sub); if (!d.exists()) continue;
    for (const QFileInfo& ver : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      const QFileInfo f(ver.absoluteFilePath() + "/rizomuv.exe"); if (f.exists() && (best.isEmpty() || f.lastModified() > bt)) { best = f.absoluteFilePath(); bt = f.lastModified(); } } }
  if (!best.isEmpty()) QSettings().setValue("rizom/exe", best);
  return best;
}
}  // namespace

QString Viewport::uvToRizom(bool ask, bool launch) {   // (launch off: the files written, RizomUV not started — checks)
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it) { ui::toast(tr("Select the mesh whose UV goes to RizomUV")); return "!"; }
  
  QString exe = ask ? QString() : findRizom();
  if (exe.isEmpty()) {
    QWidget* mw = nullptr; for (QWidget* w : QApplication::topLevelWidgets()) if (w->inherits("QMainWindow")) mw = w;
    exe = QFileDialog::getOpenFileName(mw, tr("Where is RizomUV? (rizomuv.exe)"), "C:/", tr("RizomUV (rizomuv.exe);;Programs (*.exe)"));
    if (exe.isEmpty()) return "!";
    QSettings().setValue("rizom/exe", exe);
  }
  // ---- the OBJ: its vertices, its UV points (one per vertex and UV — shared where the UV runs on), its polygons. Each UV
  // Mat (a texture of its own, packed into its own 0…1) in a RizomUV tile of its own: its U moved along by the tile ----
  // (several meshes picked: all of them in one OBJ, their points, polygons and seams side by side — each comes back its own part)
  std::vector<int> meshIds; for (int id : tselTargets()) { const Item* x = scene_->item(id); if (x && uv::has(*baseOf(*x))) meshIds.push_back(id); }
  if (meshIds.size() < 2) { meshIds.clear(); meshIds.push_back(m); }
  auto Jp = std::make_shared<PolyMesh>(); rzParts_.clear(); uv::Pins pinsAll; bool anyMat = false; for (int id : meshIds) anyMat = anyMat || !baseOf(*scene_->item(id))->faceMat.empty();
  for (int id : meshIds) { const PolyMesh& Bm = *baseOf(*scene_->item(id)); const int v0 = int(Jp->pos.size()), c0 = int(Jp->faceVerts.size()), f0 = Jp->faceCount();
    rzParts_.push_back({id, f0, Bm.faceCount()});
    Jp->pos.insert(Jp->pos.end(), Bm.pos.begin(), Bm.pos.end()); if (Jp->faceStart.empty()) Jp->faceStart.push_back(0);
    for (int f = 0; f < Bm.faceCount(); f++) Jp->faceStart.push_back(c0 + Bm.faceStart[size_t(f) + 1]);
    for (int v : Bm.faceVerts) Jp->faceVerts.push_back(v0 + v);
    if (uv::has(Bm)) Jp->cornerUV.insert(Jp->cornerUV.end(), Bm.cornerUV.begin(), Bm.cornerUV.end());
    for (auto& s : Bm.seams) Jp->seams.push_back({v0 + s[0], v0 + s[1]});
    if (anyMat) for (int f = 0; f < Bm.faceCount(); f++) Jp->faceMat.push_back(Bm.faceMat.empty() ? -1 : Bm.faceMat[size_t(f)]);
    for (const auto& p : uvPinsOf(id)) pinsAll.insert({v0 + p.first, v0 + p.second}); }
  if (meshIds.size() > 1) Jp->matNames = baseOf(*it)->matNames;
  const PolyMesh& B = meshIds.size() > 1 ? *Jp : *baseOf(*it); const bool uv = uv::has(B); const int nf = B.faceCount();
  std::map<int, int> tileOfMat; for (int fi = 0; fi < nf; fi++) tileOfMat.emplace(B.faceMat.empty() ? -1 : B.faceMat[size_t(fi)], 0);
  { int t = 0; for (auto& [mat, ti] : tileOfMat) ti = t++; }
  const int tiles = int(tileOfMat.size());
  rzTile_.assign(size_t(nf), 0); if (tiles > 1) for (int fi = 0; fi < nf; fi++) rzTile_[size_t(fi)] = tileOfMat[B.faceMat.empty() ? -1 : B.faceMat[size_t(fi)]];
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/HairHarpy"; QDir().mkpath(dir);
  QString safe = it->name; safe.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_"); if (meshIds.size() > 1) safe += QString("_plus%1").arg(meshIds.size() - 1);
  const QString path = dir + "/" + (safe.isEmpty() ? QString("mesh") : safe) + "_rizom.obj";
  QFile f(path); if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { ui::toast(tr("Can't write %1").arg(path)); return "!"; }
  std::vector<int> vtOf(B.faceVerts.size(), -1);
  { QTextStream o(&f); o.setRealNumberPrecision(9);
    o << "# HairHarpy to RizomUV: save in RizomUV (the same file) - the UV comes back\n";
    { Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30); for (auto& p : B.pos) { lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)); hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)); }   // (about the origin: RizomUV frames the 3D view on it — a model far off was not seen; only the UV comes back)
      const Vec3 c = B.pos.empty() ? Vec3(0, 0, 0) : (lo + hi) * 0.5; for (auto& p : B.pos) o << "v " << (p.x - c.x) << " " << (p.y - c.y) << " " << (p.z - c.z) << "\n"; }
    if (uv) { std::map<std::tuple<int, float, float>, int> vt; int n = 0;
      for (int fi = 0; fi < nf; fi++) { const float du = float(rzTile_[size_t(fi)]);
        for (int k = B.faceStart[size_t(fi)]; k < B.faceStart[size_t(fi) + 1]; k++) { const auto& u = B.cornerUV[size_t(k)]; auto r = vt.emplace(std::make_tuple(B.faceVerts[size_t(k)], u[0] + du, u[1]), n);
          if (r.second) { o << "vt " << (u[0] + du) << " " << u[1] << "\n"; n++; } vtOf[size_t(k)] = r.first->second; } } }
    for (int fi = 0; fi < nf; fi++) { o << "f";
      for (int k = B.faceStart[size_t(fi)]; k < B.faceStart[size_t(fi) + 1]; k++) { o << " " << (B.faceVerts[size_t(k)] + 1); if (uv) o << "/" << (vtOf[size_t(k)] + 1); }
      o << "\n"; } }
  f.close();
  // ---- the script RizomUV runs at its start (-cfi): the OBJ loaded; the tiles (a UV Mat each); the stacks — shells laid
  // over each other here (a copy over its twin, a mirror image over its pair) — each a group of its own in its tile, so
  // RizomUV's Pack keeps them stacked; the pins pinned; the padding of the texture its packing's ----
  int nStacks = 0, nPins = 0;
  const QString lua = path.left(path.size() - 4) + ".lua";
  { QFile sf(lua); if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { ui::toast(tr("Can't write %1").arg(lua)); return "!"; }
    QTextStream s(&sf); auto q = [](QString p) { return "\"" + p.replace('\\', '/').replace('"', "\\\"") + "\""; };
    auto ids = [](const std::vector<int>& v) { QStringList l; for (int x : v) l << QString::number(x); return "{" + l.join(", ") + "}"; };
    s << "-- HairHarpy: " << it->name << " sent to RizomUV\n";
    // (each step on its own: one RizomUV refuses doesn't stop the rest; what each did — in <name>_rizom_log.txt)
    s << "local log = io.open(" << q(path.left(path.size() - 4) + "_log.txt") << ", \"w\")\n";
    s << "local step = 0\n";
    s << "local function try(f) step = step + 1; local ok, err = pcall(f); local line = \"step \" .. step .. (ok and \" ok\" or (\" FAILED: \" .. tostring(err))); if not ok then print(\"HairHarpy: \" .. line) end; if log then log:write(line .. \"\\n\"); log:flush() end end\n";
    s << "try(function() ZomLoad({File={Path=" << q(path) << ", ImportGroups=true, XYZUVW=true, UVWProps=true}, NormalizeUVW=false}) end)\n";
    if (tiles > 1) {
      s << "try(function() ZomIslandGroups({Mode=\"SetMultiTileLayout\", WorkingSet=\"Visible\", TileRows=1, TileColumns=" << tiles << "}) end)\n";
      s << "try(function() ZomIslandGroups({Mode=\"DistributeInTilesByBBox\", WorkingSet=\"Visible\"}) end)\n";
      // (no DefineGroup into the tiles: RizomUV moves an island so put into the middle of its tile — every shell piled
      // up there; the islands kept by their boxes instead, each where we laid it)
    }
    if (uv) {
      // (the stacks: shells of one UV Mat with as many polygons and as much 3D area, their UV boxes over each other)
      auto T = meshIds.size() > 1 ? topo::ttopo(Jp) : topo::ttopo(baseSp(*it)); const uv::Points P = uv::points(B, *T); const uv::Shells S = uv::shells(B, *T, P);
      struct Sh { int first = -1, n = 0, tile = 0; double a3 = 0, u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300; };
      std::vector<Sh> sh(size_t(S.count));
      for (int fi = 0; fi < nf; fi++) { Sh& h = sh[size_t(S.of[size_t(fi)])]; if (h.first < 0) h.first = fi; h.n++; h.tile = rzTile_[size_t(fi)]; h.a3 += uv::faceArea3(B, fi);
        for (int k = B.faceStart[size_t(fi)]; k < B.faceStart[size_t(fi) + 1]; k++) { const auto& u = B.cornerUV[size_t(k)]; h.u0 = std::min(h.u0, double(u[0])); h.v0 = std::min(h.v0, double(u[1])); h.u1 = std::max(h.u1, double(u[0])); h.v1 = std::max(h.v1, double(u[1])); } }
      std::vector<int> grp(size_t(S.count)); for (int i = 0; i < S.count; i++) grp[size_t(i)] = i;
      std::function<int(int)> root = [&](int x) { return grp[size_t(x)] == x ? x : grp[size_t(x)] = root(grp[size_t(x)]); };
      for (int a = 0; a < S.count; a++) for (int b = a + 1; b < S.count; b++) {
        const Sh &A = sh[size_t(a)], &Bb = sh[size_t(b)]; if (A.tile != Bb.tile || A.n != Bb.n || std::abs(A.a3 - Bb.a3) > 0.02 * std::max(A.a3, Bb.a3)) continue;
        const double iw = std::min(A.u1, Bb.u1) - std::max(A.u0, Bb.u0), ih = std::min(A.v1, Bb.v1) - std::max(A.v0, Bb.v0); if (iw <= 0 || ih <= 0) continue;
        const double ia = iw * ih, ua = (A.u1 - A.u0) * (A.v1 - A.v0) + (Bb.u1 - Bb.u0) * (Bb.v1 - Bb.v0) - ia;
        if (ua > 0 && ia / ua > 0.8) grp[size_t(root(a))] = root(b); }
      std::map<int, std::vector<int>> stacks; for (int i = 0; i < S.count; i++) if (sh[size_t(i)].first >= 0) stacks[root(i)].push_back(i);
      for (auto& [r, members] : stacks) { if (members.size() < 2) continue; std::vector<int> polys; for (int i : members) polys.push_back(sh[size_t(i)].first);
        s << "try(function() ZomIslandGroups({Mode=\"DefineGroup\", WorkingSet=\"Visible\", IslandByPolygonIDs=" << ids(polys) << ", GroupPath=\"RootGroup.Children.Tile_" << sh[size_t(members[0])].tile << "_0.Children.Stack" << nStacks << "\", AutoDelete=true}) end)\n"; nStacks++; }
      // (the pins: the UV points at the pinned edges' ends — as the OBJ numbers them)
      const uv::Pins pins = meshIds.size() > 1 ? pinsAll : uvPinsOf(m); std::set<int> pv;
      for (int fi = 0; fi < nf && !pins.empty(); fi++) for (int k = B.faceStart[size_t(fi)]; k < B.faceStart[size_t(fi) + 1]; k++) { const int nk = k + 1 < B.faceStart[size_t(fi) + 1] ? k + 1 : B.faceStart[size_t(fi)];
        const int a = B.faceVerts[size_t(k)], b = B.faceVerts[size_t(nk)]; if (uv::pointPinned(pins, a, fi)) pv.insert(vtOf[size_t(k)]); if (pins.count({std::min(a, b), std::max(a, b)})) { pv.insert(vtOf[size_t(k)]); pv.insert(vtOf[size_t(nk)]); } }
      if (!pv.empty()) { nPins = int(pv.size());
        s << "try(function() ZomSelect({PrimType=\"Vertex\", WorkingSet=\"Visible\", ResetBefore=true, Select=true, IDs=" << ids({pv.begin(), pv.end()}) << ", List=true, InterpretIDsUsingOriginalUVWPolys=true}) end)\n";
        s << "try(function() ZomConstrain({PrimType=\"Vertex\", WorkingSet=\"Visible\", Mode=\"Pin\"}) end)\n";
        s << "try(function() ZomSelect({PrimType=\"Vertex\", WorkingSet=\"Visible\", ResetBefore=true, DeSelect=true, All=true}) end)\n"; }
    }
    { const UvTex t = uvTex(m); const double M = std::max(t.w, t.h);   // (the texture's padding: between the islands, half of it off the tiles' edges)
      // (its root group's Pack settings — what RizomUV's Pack panel shows and uses: the map's size, the padding, the margin)
      for (auto [key, val] : {std::pair<const char*, double>{"MapResolution", M}, {"PaddingSize", t.pad / M}, {"MarginSize", t.pad / 2 / M}})
        s << "try(function() ZomSet({Path=\"Lib.Mesh.RootGroup.Properties.Pack." << key << "\", Value=" << (QString(key) == "MapResolution" ? QString::number(int(val)) : QString::number(val, 'g', 10)) << "}) end)\n"; }
    s << "if log then log:write(\"done\\n\"); log:close() end\n";
  }
  // ---- RizomUV started with the script; its saves of the OBJ watched ----
  if (launch && !QProcess::startDetached(exe, {"-cfi", QDir::toNativeSeparators(lua)}, QFileInfo(exe).absolutePath())) { ui::toast(tr("RizomUV didn't start: %1 (Shift + the button — pick it again)").arg(exe)); return "!"; }
  if (!rzWatch_) { rzWatch_ = new QFileSystemWatcher(this);
    connect(rzWatch_, &QFileSystemWatcher::fileChanged, this, [this](const QString& p) {   // (saved: read a moment later — the file whole; watched again — a save may replace it)
      // (a drag going on — the UV or the vertices being moved: read once it is over, not under it)
      auto read = [this, p](const auto& self) -> void {
        if (drag_ != Drag::None) { QTimer::singleShot(300, this, [self] { self(self); }); return; }
        if (QFileInfo::exists(p) && !rzWatch_->files().contains(p)) rzWatch_->addPath(p); uvFromRizom(p); };
      QTimer::singleShot(500, this, [read] { read(read); }); }); }
  for (const QString& p : rzWatch_->files()) rzWatch_->removePath(p);
  rzWatch_->addPath(path); rzMesh_ = m; rzStamp_ = QFileInfo(path).lastModified().toMSecsSinceEpoch();
  QString msg = tr("Sent to RizomUV: %1 — unwrap there and Save (the same file): the UV comes back here").arg(meshIds.size() > 1 ? tr("%1 meshes").arg(meshIds.size()) : it->name);
  QStringList extra; if (tiles > 1) extra << tr("%1 UV Mats — a tile each").arg(tiles); if (nStacks) extra << tr("%1 stack%2 kept stacked").arg(nStacks).arg(nStacks == 1 ? "" : "s"); if (nPins) extra << tr("%1 pinned point%2").arg(nPins).arg(nPins == 1 ? "" : "s");
  if (!extra.isEmpty()) msg += " · " + extra.join(" · ");
  ui::toast(msg); return msg;
}

// RizomUV saved the file: its UV onto the mesh (the same polygons in the same order — else nothing is changed)
QString Viewport::uvFromRizom(const QString& path) {
  const qint64 st = QFileInfo(path).lastModified().toMSecsSinceEpoch(); if (st == rzStamp_) return {}; rzStamp_ = st;
  Item* it = rzMesh_ >= 0 ? scene_->item(rzMesh_) : nullptr; if (!it) return "!";
  QFile f(path); if (!f.open(QIODevice::ReadOnly)) return "!";
  std::vector<std::array<float, 2>> vt; std::vector<std::vector<std::pair<int, int>>> faces;
  QTextStream in(&f);
  while (!in.atEnd()) { const QString line = in.readLine().trimmed();
    if (line.startsWith("vt ")) { const auto p = line.mid(3).split(' ', Qt::SkipEmptyParts); if (p.size() >= 2) vt.push_back({p[0].toFloat(), p[1].toFloat()}); }
    else if (line.startsWith("f ")) { std::vector<std::pair<int, int>> fc; for (const QString& c : line.mid(2).split(' ', Qt::SkipEmptyParts)) { const auto q = c.split('/'); fc.push_back({q[0].toInt() - 1, q.size() > 1 && !q[1].isEmpty() ? q[1].toInt() - 1 : -1}); } faces.push_back(fc); } }
  // (the meshes sent: each its part of the polygons, its points after the ones before it)
  std::vector<std::array<int, 3>> parts = rzParts_; if (parts.empty()) parts.push_back({rzMesh_, 0, baseOf(*it)->faceCount()});
  int total = 0; for (const auto& p : parts) { const Item* x = scene_->item(p[0]); if (!x || baseOf(*x)->faceCount() != p[2]) { ui::toast(tr("A mesh sent to RizomUV changed since — the UV not taken")); return "!"; } total += p[2]; }
  if (int(faces.size()) != total || vt.empty()) { ui::toast(tr("RizomUV's file doesn't match the meshes (%1 polygons there, %2 here) — the UV not taken").arg(faces.size()).arg(total)); return "!"; }
  std::vector<std::pair<int, std::shared_ptr<PolyMesh>>> out; int vOff = 0;
  for (const auto& part : parts) { Item* x = scene_->item(part[0]); const PolyMesh& B = *baseOf(*x);
    auto nb = std::make_shared<PolyMesh>(B); nb->cornerUV.assign(B.faceVerts.size(), {0, 0});
    for (int fi = 0; fi < B.faceCount(); fi++) { const int gi = part[1] + fi;
      const int s = B.faceStart[size_t(fi)], n = B.faceStart[size_t(fi) + 1] - s; const auto& fc = faces[size_t(gi)];
      if (int(fc.size()) != n) { ui::toast(tr("RizomUV's polygon %1 differs from the mesh's — the UV not taken").arg(gi + 1)); return "!"; }
      // (its corners as RizomUV has them — turned round if it starts elsewhere)
      int off = -1; for (int r = 0; r < n && off < 0; r++) { bool same = true; for (int k = 0; k < n && same; k++) same = fc[size_t((k + r) % n)].first == vOff + B.faceVerts[size_t(s + k)]; if (same) off = r; }
      if (off < 0) { ui::toast(tr("RizomUV's polygon %1 has other vertices — the UV not taken").arg(gi + 1)); return "!"; }
      const float du = size_t(gi) < rzTile_.size() ? float(rzTile_[size_t(gi)]) : 0.f;   // (its UV Mat's tile: back into 0…1)
      for (int k = 0; k < n; k++) { const int t = fc[size_t((k + off) % n)].second; if (t >= 0 && t < int(vt.size())) nb->cornerUV[size_t(s + k)] = {vt[size_t(t)][0] - du, vt[size_t(t)][1]}; } }
    vOff += int(B.pos.size()); out.push_back({part[0], std::move(nb)}); }
  for (auto& [id, nb] : out) { topo::shareTopo(nb, baseSp(*scene_->item(id))); topoWrite(id, nb); }
  uvStamp_++; uvRev_++; scene_->revision++; commit(true); emit topoState();
  const QString msg = out.size() > 1 ? tr("The UV came back from RizomUV: %1 meshes").arg(out.size()) : tr("The UV came back from RizomUV: %1").arg(it->name); ui::toast(msg); return msg;
}

}  // namespace hm
