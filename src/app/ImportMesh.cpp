#include "app/ImportMesh.h"
#include "io/MeshImport.h"
#include "scene/Convert.h"
#include "scene/MeshCurves.h"
#include "scene/Scene.h"
#include "scene/Topo.h"
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QRadioButton>
#include <QSlider>
#include <QVBoxLayout>
#include <atomic>
#include <thread>
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "app/Extras.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QWidgetAction>
#include <QLineEdit>
#include <algorithm>
#include <map>
#include "scene/MeshMods.h"

namespace hm {

// ---- content of another scale coming into a scene that has something in it (20.11): asked whether to fit it ----
std::function<std::optional<double>(double)> fitScaleHook;
QString fitScaleAsked;

Box3 sceneContentBox(const Scene& sc, const std::set<int>& skip) {
  Box3 b;
  for (const auto& [id, it] : sc.items) { if (skip.count(id) || !sc.isVisible(it)) continue;
    if (it.poly && (it.type == ItemType::IMesh || it.type == ItemType::Mesh || it.type == ItemType::Head)) b.unite(it.poly->bounds());
    if (it.type == ItemType::Curve) for (int a : it.anchors) if (const Anchor* an = sc.anchor(a)) b.expand(an->pos); }
  return b;
}
double boxBig(const Box3& b) { if (b.empty()) return 0; const Vec3 s = b.size(); return std::max({s.x, s.y, s.z}); }

std::optional<double> askFitScale(QWidget* parent, const QString& what, double times, double optimal) {
  const bool bigger = times >= 1; const double t = bigger ? times : 1 / times;
  fitScaleAsked = QString("%1: ×%2 %3, offered ×%4").arg(what).arg(QString::number(t, 'g', 3)).arg(bigger ? "bigger" : "smaller").arg(QString::number(optimal, 'g', 4));
  if (fitScaleHook) return fitScaleHook(optimal);
  // (the look of the Inspector's Report — 20.11, the author: "make this window like the inspector's report window": Mara, the
  // title, the text, the buttons; framed blue, no title bar; it waits for the answer, Esc — kept as it is)
  QDialog d(parent, Qt::Dialog | Qt::FramelessWindowHint); d.setObjectName("fitAsk"); d.setWindowTitle(QObject::tr("Another scale"));
  d.setAttribute(Qt::WA_TranslucentBackground);
  auto* fr = new QFrame; fr->setObjectName("fitAskF");
  fr->setStyleSheet("#fitAskF{background:#111725;border:2px solid #5a8dee;border-radius:16px}"
                    "#fitAskF QLineEdit{background:#0b0f19;border:1px solid #3a4568;border-radius:6px;color:#fff;padding:4px 8px;font-size:13px}"
                    "#fitAskF QPushButton{background:#1f2638;border:1px solid #3a4568;color:#dde;border-radius:6px;padding:5px 12px;min-width:68px}"
                    "#fitAskF QPushButton[blue=\"true\"]{background:#3b6fd8;border-color:#5a8dee;color:#fff;min-width:84px}");
  auto* outer = new QVBoxLayout(&d); outer->setContentsMargins(0, 0, 0, 0); outer->addWidget(fr);
  auto* h = new QHBoxLayout(fr); h->setContentsMargins(14, 14, 14, 14); h->setSpacing(16);
  auto* mara = new QLabel; mara->setFixedSize(150, 150); mara->setScaledContents(true); mara->setPixmap(ui::image("mara")); h->addWidget(mara, 0, Qt::AlignVCenter);
  extras::attachMara(mara);
  auto* v = new QVBoxLayout; v->setSpacing(8); h->addLayout(v, 1);
  auto* title = new QLabel(QObject::tr("Another scale")); title->setStyleSheet("font-weight:600;font-size:14px;color:#fff"); v->addWidget(title);
  const QString tt = QLocale().toString(t, 'f', t < 10 ? 1 : 0);
  auto* l = new QLabel(QObject::tr("%1 is about %2 times %3 than what the scene has. Fit it to the project?").arg(what.toHtmlEscaped()).arg(tt).arg(bigger ? QObject::tr("bigger") : QObject::tr("smaller")));
  l->setStyleSheet("color:#c8cbd8;font-size:12px"); l->setWordWrap(true); v->addWidget(l);
  auto* row = new QHBoxLayout; row->setSpacing(6);
  auto* e = new QLineEdit(QString::number(optimal, 'g', 4));
  e->setToolTip(QObject::tr("The scale it gets — the fitted one, or your own: a number (0.002) or a fraction (1/500)"));
  auto* back = new QPushButton("↺"); back->setStyleSheet("min-width:0;padding:5px 8px"); back->setToolTip(QObject::tr("Back to the fitted value ×%1").arg(QString::number(optimal, 'g', 4)));
  QObject::connect(back, &QPushButton::clicked, &d, [e, optimal] { e->setText(QString::number(optimal, 'g', 4)); });
  auto* sl = new QLabel(QObject::tr("Scale ×")); sl->setStyleSheet("color:#c8cbd8;font-size:12px");
  row->addWidget(sl); row->addWidget(e, 1); row->addWidget(back); v->addLayout(row);
  v->addStretch(1);
  auto* bar = new QHBoxLayout; bar->setSpacing(6); bar->addStretch(1);
  auto* keep = new QPushButton(QObject::tr("Keep its size")); auto* fit = new QPushButton(QObject::tr("Fit")); fit->setProperty("blue", true); fit->setDefault(true);
  bar->addWidget(keep); bar->addWidget(fit); v->addLayout(bar);
  fr->setFixedWidth(620);
  if (parent) { d.adjustSize(); const QRect g = parent->window()->geometry(); d.move(g.center().x() - d.width() / 2, g.center().y() - d.height() / 2); }   // (in the middle of the window — 20.11, the author: not in the corner)
  std::optional<double> r;
  auto parse = [](QString s) -> double { s = s.trimmed().replace(',', '.'); bool ok = false;
    if (const int k = int(s.indexOf('/')); k > 0) { const double a = QLocale::c().toDouble(s.left(k).trimmed(), &ok); if (!ok) return 0; const double b = QLocale::c().toDouble(s.mid(k + 1).trimmed(), &ok); return ok && b != 0 ? a / b : 0; }
    const double x = QLocale::c().toDouble(s, &ok); return ok ? x : 0; };
  QObject::connect(fit, &QPushButton::clicked, &d, [&] { const double k = parse(e->text()); if (!(k > 0) || !std::isfinite(k)) { e->setFocus(); e->selectAll(); return; } r = k; d.accept(); });
  QObject::connect(keep, &QPushButton::clicked, &d, &QDialog::reject);
  e->selectAll(); e->setFocus();
  d.exec();
  return r;
}

void scaleMeshes(Scene& s, const std::vector<int>& ids, double k, const Vec3& from, const Vec3& to) {
  Affine S; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) S.L[i][j] = i == j ? k : 0; S.t = to - from * k;
  for (int id : ids) { const Item* it = s.item(id); if (!it || !(it->type == ItemType::IMesh || it->type == ItemType::Mesh) || !(it->base || it->poly)) continue;
    meshmods::setMeshMatrix(s, id, Affine::mul(S, meshmods::meshMatrix(s, *it))); }
}

std::optional<double> offerFitScale(Viewport* vp, const Box3& sceneBox, const Box3& content, const QString& what, std::optional<double> optimal) {
  const double a = boxBig(sceneBox), b = boxBig(content); if (!(a > 0) || !(b > 0)) return std::nullopt;
  const double times = b / a; if (times < 4 && times > 0.25) return std::nullopt;   // (about the scene's size: nothing to ask)
  (void)vp; return askFitScale(QApplication::activeWindow(), what, times, optimal ? *optimal : 1 / times);
}

namespace {

const QStringList MODEL_EXTS = {"obj", "glb", "gltf", "fbx"};
const QRegularExpression IMG_RE("\\.(png|jpe?g|webp|bmp|gif|avif|tga)$", QRegularExpression::CaseInsensitiveOption);
QString extOf(const QString& f) { return QFileInfo(f).suffix().toLower(); }
QByteArray readAll(const QString& f) { QFile q(f); return q.open(QIODevice::ReadOnly) ? q.readAll() : QByteArray(); }
QString baseName(const QString& p) { return p.section(QRegularExpression("[\\\\/]"), -1); }
QString mimeOf(const QString& name) {
  QString e = QFileInfo(name).suffix().toLower();
  return e == "jpg" || e == "jpeg" ? "image/jpeg" : e == "webp" ? "image/webp" : e == "bmp" ? "image/bmp" : e == "gif" ? "image/gif" : e == "avif" ? "image/avif" : e == "tga" ? "image/x-tga" : "image/png";
}
QJsonObject keptImage(const QString& path) { return {{"name", QFileInfo(path).fileName()}, {"data", "data:" + mimeOf(path) + ";base64," + QString::fromLatin1(readAll(path).toBase64())}}; }

// the file as a mesh source (srcOfFile): OBJ / GLTF as text, GLB / FBX as base64. A glTF's buffers that are files of their
// own (picked with it) go into it as data: the project keeps the whole model
QJsonObject sourceOf(const QString& path, const QString& ext, const std::map<QString, QString>& extra) {
  QByteArray b = readAll(path);
  if (ext == "glb" || ext == "fbx") return {{"ext", ext}, {"b64", QString::fromLatin1(b.toBase64())}};
  if (ext == "gltf") {
    QJsonObject G = QJsonDocument::fromJson(b).object(); QJsonArray bufs = G.value("buffers").toArray(); bool changed = false;
    for (int i = 0; i < bufs.size(); i++) {
      QJsonObject o = bufs[i].toObject(); QString uri = o.value("uri").toString();
      if (uri.isEmpty() || uri.startsWith("data:")) continue;
      QString local = QFileInfo(path).dir().filePath(uri); auto it = extra.find(baseName(uri).toLower());
      QString f = QFile::exists(local) ? local : it != extra.end() ? it->second : QString(); if (f.isEmpty()) continue;
      o.insert("uri", "data:application/octet-stream;base64," + QString::fromLatin1(readAll(f).toBase64())); bufs[i] = o; changed = true;
    }
    if (changed) { G.insert("buffers", bufs); b = QJsonDocument(G).toJson(QJsonDocument::Compact); }
  }
  return {{"ext", ext}, {"text", QString::fromUtf8(b)}};
}

// OBJ's material library: newmtl → { color (Kd), map (map_Kd), alphaMap (map_d) }
struct Mtl { QString color; QString map, alphaMap; };
std::map<QString, Mtl> parseMTL(const QString& text) {
  std::map<QString, Mtl> out; Mtl* cur = nullptr;
  auto file = [](const QString& rest) { QStringList p = rest.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts); QString last;
    for (const QString& s : p) if (!s.startsWith('-') && !QRegularExpression("^[-\\d.]+$").match(s).hasMatch()) last = s; return last.isEmpty() ? rest : last; };
  for (const QString& line : text.split('\n')) {
    QString t = line.trimmed(); int sp = t.indexOf(' '); QString k = (sp < 0 ? t : t.left(sp)).toLower(), rest = sp < 0 ? QString() : t.mid(sp + 1).trimmed();
    if (k == "newmtl") { cur = &out[rest]; continue; }
    if (!cur) continue;
    if (k == "kd") { QStringList c = rest.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts); bool ok = c.size() >= 3; double v[3] = {0, 0, 0};
      for (int i = 0; i < 3 && ok; i++) v[i] = c[i].toDouble(&ok);
      if (ok) cur->color = QString("#%1%2%3").arg(int(std::lround(std::clamp(v[0], 0.0, 1.0) * 255)), 2, 16, QChar('0')).arg(int(std::lround(std::clamp(v[1], 0.0, 1.0) * 255)), 2, 16, QChar('0')).arg(int(std::lround(std::clamp(v[2], 0.0, 1.0) * 255)), 2, 16, QChar('0')); }
    else if (k == "map_kd") cur->map = file(rest);
    else if (k == "map_d") cur->alphaMap = file(rest);
  }
  return out;
}

// the look of a freshly imported mesh (importLook): textures from the picked files (the file's references, OBJ's MTL) become
// its own maps (kept in the project), the MTL's colours its set colours; textures shown when there are any
QJsonObject importLook(const PolyMesh& B, const std::map<QString, QString>& extra) {
  auto sets = texSets(B); std::vector<QJsonObject> maps(sets.size()), refs(sets.size()); std::vector<QString> colors(sets.size());
  auto pick = [&](size_t i, const char* slot, const QString& f) { maps[i].insert(slot, keptImage(f)); };
  auto own = [&](size_t i, int slot) -> const PolyMesh::Map* { int m = sets[i].mat; return m >= 0 && size_t(m) < B.matMaps.size() ? &B.matMaps[size_t(m)][size_t(slot)] : nullptr; };
  // (the file's references to images of its own: those picked with it)
  for (size_t i = 0; i < sets.size(); i++) for (int s = 0; s < 2; s++) {
    const PolyMesh::Map* o = own(i, s); if (!o || o->name.isEmpty() || !o->data.isEmpty()) continue;
    auto it = extra.find(baseName(o->name).toLower()); if (it != extra.end()) pick(i, s ? "alphaMap" : "map", it->second);
  }
  std::map<QString, Mtl> lib;
  for (auto& [k, f] : extra) if (extOf(f) == "mtl") for (auto& [n, m] : parseMTL(QString::fromUtf8(readAll(f)))) lib[n] = m;
  for (size_t i = 0; i < sets.size(); i++) {
    auto it = lib.find(sets[i].name); if (it == lib.end()) continue;
    if (!it->second.color.isEmpty()) colors[i] = it->second.color;
    for (auto [slot, path] : {std::pair<const char*, QString>{"map", it->second.map}, {"alphaMap", it->second.alphaMap}}) {
      if (path.isEmpty()) continue; refs[i].insert(slot, baseName(path)); auto f = extra.find(baseName(path).toLower()); if (f != extra.end()) pick(i, slot, f->second);
    }
  }
  // a single set and a single picked image no reference matched: that image is its base colour
  QStringList imgs; for (auto& [k, f] : extra) if (IMG_RE.match(f).hasMatch()) imgs << f;
  bool ownMap0 = own(0, 0) && !own(0, 0)->data.isEmpty();
  if (sets.size() == 1 && imgs.size() == 1 && !maps[0].contains("map") && !ownMap0) pick(0, "map", imgs[0]);
  QJsonArray js; bool tex = false;
  for (size_t i = 0; i < sets.size(); i++) {
    QJsonObject s{{"name", sets[i].name}, {"on", true}, {"maps", maps[i]}}; if (!colors[i].isEmpty()) s.insert("color", colors[i]); if (!refs[i].isEmpty()) s.insert("ref", refs[i]);
    js.append(s);
    if (!maps[i].isEmpty() || (own(i, 0) && !own(i, 0)->data.isEmpty()) || (own(i, 1) && !own(i, 1)->data.isEmpty())) tex = true;
  }
  return {{"tex", tex}, {"flat", false}, {"sets", js}};
}

// an OBJ with only these polygons (objSubset): the vertices they use renumbered, a UV per corner when the mesh has them
QString objSubset(const PolyMesh& B, const std::vector<int>& faces, const QString& name) {
  QString out = "o " + name + "\n"; std::map<int, int> vmap; QString vs, ts, fs; int nt = 0;
  auto num = [](double x) { return QString::number(x, 'g', 9); };
  for (int f : faces) {
    QString line = "f";
    for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) {
      int v = B.faceVerts[size_t(k)]; auto it = vmap.find(v);
      if (it == vmap.end()) { it = vmap.emplace(v, int(vmap.size()) + 1).first; const Vec3& p = B.pos[size_t(v)]; vs += "v " + num(p.x) + " " + num(p.y) + " " + num(p.z) + "\n"; }
      line += " " + QString::number(it->second);
      if (!B.cornerUV.empty()) { ts += "vt " + num(B.cornerUV[size_t(k)][0]) + " " + num(B.cornerUV[size_t(k)][1]) + "\n"; line += "/" + QString::number(++nt); }
    }
    fs += line + "\n";
  }
  // (the materials kept: usemtl per run of polygons)
  if (!B.matNames.empty() && B.faceMat.size() == size_t(B.faceCount())) {
    fs.clear(); int cur = -2;
    int t = 0;
    for (int f : faces) {
      int m = B.faceMat[size_t(f)]; if (m != cur) { cur = m; if (m >= 0) fs += "usemtl " + B.matNames[size_t(m)] + "\n"; }
      QString line = "f";
      for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) { line += " " + QString::number(vmap[B.faceVerts[size_t(k)]]); if (!B.cornerUV.empty()) line += "/" + QString::number(++t); }
      fs += line + "\n";
    }
  }
  QString lib; for (const QString& l : B.mtllib) lib += "mtllib " + l + "\n";
  return lib + out + vs + ts + fs;
}

// where an imported mesh goes: straight into Meshes (20.11: no Imported folder any more — an older project's one is kept as it is)
int importGroup(Scene& sc) { return meshesGroup(sc); }

Item& newIMesh(Scene& sc, int parent, const QString& name, const QJsonObject& src, std::shared_ptr<PolyMesh> poly) {
  Item& it = sc.addItem(ItemType::IMesh, parent, name);
  it.raw = QJsonObject{{"src", src}}; it.poly = std::move(poly);
  return it;
}

// the import window's choices kept in the mesh's source (applied again whenever the project is read)
void optFlags(QJsonObject& src, const TopoImport& o) {
  if (!o.uvs) src.insert("noUV", true);
  if (!o.textures) src.insert("noTex", true);
  if (!o.polypaint) src.insert("noPaint", true);
  if (!o.creases) src.insert("noCrease", true);
  if (!o.hard) src.insert("noHard", true); else src.insert("hardN", true);
}
QByteArray bytesOf(const QJsonObject& src) { return src.contains("text") ? src.value("text").toString().toUtf8() : QByteArray::fromBase64(src.value("b64").toString().toLatin1()); }
// the file's curves as this program's curves (in a group "<file> curves") → how many
int importFileCurves(Scene& s, const QString& ext, const QByteArray& bytes, const QString& base) {
  auto lines = importCurves(ext, bytes); if (lines.empty()) return 0;
  return int(meshc::polylinesToCurves(s, lines, base + " curves").size());
}

void importFilesImpl(const QStringList& files, bool asHead, Viewport* vp, Scene* sc, const TopoImport* opt);
}  // namespace

void importFiles(const QStringList& files, bool asHead, Viewport* vp, Scene* sc) { importFilesImpl(files, asHead, vp, sc, nullptr); }
void importFilesWith(const QStringList& files, const TopoImport& o, Viewport* vp, Scene* sc) { importFilesImpl(files, o.head, vp, sc, &o); }

namespace {
void importFilesImpl(const QStringList& files, bool asHead, Viewport* vp, Scene* sc, const TopoImport* opt) {
  asHead = false;                                               // (no head mark any more — a mesh is imported as a mesh; Head Collision takes copies)
  std::map<QString, QString> extraAll; for (const QString& f : files) extraAll[QFileInfo(f).fileName().toLower()] = f;
  QStringList models; for (const QString& f : files) if (MODEL_EXTS.contains(extOf(f))) models << f;
  if (models.isEmpty()) { ui::toast(QObject::tr("Pick the model (.obj / .glb / .gltf / .fbx) — with its texture images and .mtl if you like")); return; }
  if (asHead) models = models.mid(0, 1);
  const bool split = !asHead && (opt ? opt->groups && opt->objects : QSettings().value("import/splitGroups", false).toBool());
  int curvesMade = 0;
  ui::BusyScope bz(QObject::tr("Importing"));
  // (read first: a file that can't be read leaves the scene as it was)
  struct Part { QString name; QJsonObject src; std::shared_ptr<PolyMesh> poly; QJsonObject look; };
  struct File { QString base, ext; QByteArray bytes; std::vector<Part> parts; bool split = false; };
  std::vector<File> read;
  for (const QString& f : models) {
    const QString ext = extOf(f); auto extra = extraAll; extra.erase(QFileInfo(f).fileName().toLower());
    QJsonObject src = sourceOf(f, ext, extra); QString err;
    if (opt) { if (!opt->groups) src.insert("flat", true); optFlags(src, *opt); }
    QByteArray bytes = bytesOf(src);
    auto B = importMesh(ext, bytes, &err, opt && !opt->groups);
    if (!B || B->pos.empty()) {                              // (no geometry: a file of curves — they become this program's curves)
      int n = 0; vp->runEdit([&](Scene& s, Selection&, const CurveCache&) -> QString { n = importFileCurves(s, ext, bytes, QFileInfo(f).completeBaseName());
                  return n ? QObject::tr("%1 curve%2 from %3").arg(n).arg(n == 1 ? "" : "s").arg(QFileInfo(f).completeBaseName()) : QString(); });
      if (n) { curvesMade += n; continue; }
      ui::toast(QObject::tr("Could not load the file: %1").arg(err.isEmpty() ? QObject::tr("no meshes or curves in it") : err)); continue; }
    applySourceOptions(*B, src);
    File F; F.base = QFileInfo(f).completeBaseName(); F.ext = ext; if (opt && opt->curves) F.bytes = bytes;
    if (split && B->groupNames.size() >= 2) {                // every polygroup an OBJ source of its own (each face kept once)
      std::vector<std::vector<int>> by(B->groupNames.size());
      for (int k = 0; k < B->faceCount(); k++) by[size_t(B->faceGroup[size_t(k)])].push_back(k);
      for (size_t g = 0; g < by.size(); g++) {
        if (by[g].empty()) continue;
        QJsonObject s{{"ext", "obj"}, {"text", objSubset(*B, by[g], B->groupNames[g])}}; if (opt) optFlags(s, *opt);
        auto P = importMesh("obj", s.value("text").toString().toUtf8(), &err); if (!P) continue;
        // (the part numbers its materials in its own order: the file's maps / colours / see-through taken by name)
        P->matMaps.assign(P->matNames.size(), {}); P->matColor.assign(P->matNames.size(), QString()); P->matAlpha.assign(P->matNames.size(), 0);
        for (size_t i = 0; i < P->matNames.size(); i++) {
          auto f = std::find(B->matNames.begin(), B->matNames.end(), P->matNames[i]); if (f == B->matNames.end()) continue; const size_t j = size_t(f - B->matNames.begin());
          if (j < B->matMaps.size()) P->matMaps[i] = B->matMaps[j];
          if (j < B->matColor.size()) P->matColor[i] = B->matColor[j];
          if (j < B->matAlpha.size()) P->matAlpha[i] = B->matAlpha[j];
        }
        P->flipY = B->flipY;
        F.parts.push_back({B->groupNames[g], s, P, {}});
      }
      F.split = F.parts.size() >= 2;
    }
    if (!F.split) { F.parts.clear(); F.parts.push_back({asHead ? QFileInfo(f).fileName() : F.base, src, B, {}}); }
    for (Part& p : F.parts) if (!p.poly->cornerUV.empty() && (!opt || opt->textures)) p.look = importLook(*p.poly, extra);
    if (F.split) for (Part& p : F.parts) {                    // (a part's source is OBJ text: the file's own textures / colours / see-through kept in its look)
      if (p.look.isEmpty()) continue;
      const std::vector<TexSet> ts = texSets(*p.poly); QJsonArray S = p.look.value("sets").toArray();
      for (int i = 0; i < S.size() && size_t(i) < ts.size(); i++) {
        const int mat = ts[size_t(i)].mat; QJsonObject s = S[i].toObject(), maps = s.value("maps").toObject();
        for (int slot = 0; slot < 2; slot++) {
          const char* k = slot ? "alphaMap" : "map"; if (maps.contains(k) || mat < 0 || size_t(mat) >= p.poly->matMaps.size()) continue;
          const PolyMesh::Map& own = p.poly->matMaps[size_t(mat)][size_t(slot)]; if (own.data.isEmpty()) continue;
          const QString e = QFileInfo(own.name).suffix().toLower();
          const QString mime = e == "jpg" || e == "jpeg" ? "image/jpeg" : e == "webp" ? "image/webp" : e == "bmp" ? "image/bmp" : e == "tga" ? "image/x-tga" : "image/png";
          maps.insert(k, QJsonObject{{"name", own.name.isEmpty() ? QString("texture") : own.name}, {"data", "data:" + mime + ";base64," + QString::fromLatin1(own.data.toBase64())}});
        }
        if (!maps.value("map").isObject() && !s.contains("color") && !ts[size_t(i)].color.isEmpty()) s.insert("color", ts[size_t(i)].color);
        if (!s.contains("alpha") && ts[size_t(i)].alpha) s.insert("alpha", true);
        s.insert("maps", maps); S[i] = s;
      }
      p.look.insert("sets", S);
    }
    read.push_back(std::move(F));
  }
  if (read.empty()) { if (curvesMade) ui::toast(QObject::tr("%1 curve%2 from the file — made this program's curves").arg(curvesMade).arg(curvesMade == 1 ? "" : "s")); return; }
  const bool noGroups = split && !read[0].split;
  Box3 box; int made = 0; std::vector<int> ids; bool fitHead = false, empty = true;
  const Box3 before = sceneContentBox(*sc);                    // (what the scene had: the model of another scale is asked about)
  vp->runEdit([&](Scene& s, Selection& sel, const CurveCache&) -> QString {
    for (auto& [id, it] : s.items) { bool svc = it.type == ItemType::Group && (it.raw.value("meshesGroup").toBool() || it.raw.value("importGroup").toBool()); if (!svc) { empty = false; break; } }
    int g = importGroup(s); s.item(g)->expanded = true;
    for (File& F : read) {
      int parent = g;
      if (F.split) { Item& sub = s.addItem(ItemType::Group, g, F.base);   // (the parts in a group named after the file)
        sub.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "hide"}, {"conv", QJsonValue()}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                              {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
        sub.expanded = true; parent = sub.id; }
      for (Part& p : F.parts) {
        Item& it = newIMesh(s, parent, p.name, p.src, p.poly);
        if (!p.look.isEmpty()) it.raw.insert("look", p.look);
        box.unite(p.poly->bounds()); ids.push_back(it.id); made++;
      }
      if (opt && opt->curves) curvesMade += importFileCurves(s, F.ext, F.bytes, F.base);
    }
    fitHead = true;                                           // (20.11: the view always goes to what came in — a model of another scale than the scene stood the camera inside it)
    sel.anchors.clear(); sel.items = ids;
    if (asHead) {
      const Part& p = read[0].parts[0]; int n = int(texSets(*p.poly).size());
      bool tex = p.look.value("tex").toBool();
      return n > 1 || tex ? QObject::tr("Head: %1 texture set%2 — double-click ▦ in the outliner to set the maps").arg(n).arg(n == 1 ? "" : "s") : QString();
    }
    const Part& p0 = read[0].parts[0];
    return read[0].split ? QObject::tr("Imported %1: %2 polygroups as separate meshes").arg(read[0].base).arg(read[0].parts.size())
         : noGroups ? QObject::tr("No polygroups in %1 — imported as one mesh (%2 vertices)").arg(read[0].base).arg(p0.poly->pos.size())
         : QObject::tr("Imported: %1 (%2 vertices) — Move / Rotate / Scale write a Transform modifier").arg(p0.name).arg(p0.poly->pos.size());
  });
  if (!made) return;
  if (!asHead) if (auto k = offerFitScale(vp, before, box, read[0].base)) {   // (fitted: about the file's origin, through each mesh's Transform)
    vp->runEdit([&](Scene& s, Selection&, const CurveCache&) { scaleMeshes(s, ids, *k, Vec3(), Vec3()); return QObject::tr("%1 scaled ×%2 to the project").arg(read[0].base).arg(QString::number(*k, 'g', 4)); });
    Box3 nb; nb.expand(box.min * *k); nb.expand(box.max * *k); nb.unite(before); box = nb; }   // (in view with what was there)
  if (fitHead) {
    Box3 hb; if (asHead) { for (auto& [id, it] : sc->items) if (it.headTag && it.poly) hb.unite(it.poly->bounds()); } else hb = box;
    vp->fitToBox(hb.empty() ? box : hb);
    // an empty scene: the loop step from the head's height, as in the Wolfcut example (15.52 of 539.6 = 2.876 %)
    if (asHead && empty) { double h = (hb.empty() ? box : hb).size().y; if (h > 0) vp->setConv("step", h * 0.02876, true); }
  }
  if (curvesMade) ui::toast(QObject::tr("%1 curve%2 from the file — made this program's curves").arg(curvesMade).arg(curvesMade == 1 ? "" : "s"));
}
}  // namespace

QString receiveMesh(const QString& file, const QString& name, const QString& from, Viewport* vp, Scene* sc) {
  const QString ext = extOf(file); const QJsonObject src = sourceOf(file, ext, {}); QString err;
  auto P = importMesh(ext, bytesOf(src), &err);
  if (!P || P->pos.empty()) return {};
  applySourceOptions(*P, src);
  int id = -1; bool fresh = false; const bool firstMesh = [&] { for (auto& [k, it] : sc->items) if (it.type == ItemType::IMesh) return false; return true; }();
  vp->runEdit([&](Scene& s, Selection& sel, const CurveCache&) -> QString {
    for (auto& [k, it] : s.items) if (it.type == ItemType::IMesh && it.raw.value("from").toString() == from && it.raw.value("fromName").toString() == name) { id = k; break; }
    if (id >= 0) {                                             // (sent before: the geometry replaced, the rest kept)
      Item& it = *s.item(id); it.raw.insert("src", src);
      if (it.base) it.base = P; it.poly = P;
    } else {
      Item& it = newIMesh(s, importGroup(s), name, src, P); it.raw.insert("from", from); it.raw.insert("fromName", name); id = it.id; fresh = true;
      s.item(it.parent)->expanded = true;
    }
    s.revision++;
    sel.anchors.clear(); sel.items = {id};
    return fresh ? QObject::tr("From %1: %2 (%3 vertices%4)").arg(from, name).arg(P->pos.size()).arg(P->vcol.empty() ? QString() : QObject::tr(", polypaint"))
                 : QObject::tr("From %1: %2 updated (%3 vertices%4)").arg(from, name).arg(P->pos.size()).arg(P->vcol.empty() ? QString() : QObject::tr(", polypaint"));
  });
  if (firstMesh) vp->fitToBox(P->bounds());
  return fresh ? "new" : "updated";
}

void importMenu(QWidget* anchor, Viewport* vp, Scene* sc) {
  QMenu m(anchor);
  QAction* h = nullptr;                                         // (no head import any more: the head's stand-in is Head Collision)
  QAction* ms = m.addAction(QObject::tr("◆ Mesh → Imported"));
  ms->setToolTip(QObject::tr("An extra mesh (hat, accessory, body part...): moves, rotates, scales; Mask and Thickness modifiers"));
  auto* wa = new QWidgetAction(&m); auto* cb = new QCheckBox(QObject::tr("Polygroups as separate meshes"));
  cb->setToolTip(QObject::tr("Mesh import: every polygroup of the file becomes a mesh of its own (OBJ — its groups / objects, g / o; GLB / GLTF / FBX — its meshes), in a group named after the file"));
  cb->setChecked(QSettings().value("import/splitGroups", false).toBool()); cb->setStyleSheet("QCheckBox{padding:3px 8px;color:#ccc}");
  QObject::connect(cb, &QCheckBox::toggled, [](bool on) { QSettings().setValue("import/splitGroups", on); });
  wa->setDefaultWidget(cb); m.addAction(wa); m.setToolTipsVisible(true);
  QAction* got = m.exec(anchor->mapToGlobal(QPoint(0, anchor->height() + 2)));
  if (!got || got != ms) return;
  QSettings st;
  QStringList fs = QFileDialog::getOpenFileNames(anchor, QObject::tr("Import a mesh"), st.value("importDir").toString(),
                                                 QObject::tr("Models and their textures (*.obj *.glb *.gltf *.fbx *.mtl *.bin *.png *.jpg *.jpeg *.webp *.bmp *.gif *.tga);;All files (*)"));
  if (fs.isEmpty()) return;
  st.setValue("importDir", QFileInfo(fs[0]).absolutePath());
  importFiles(fs, false, vp, sc); (void)h;
}

// ---------- the Topology room's import ----------
namespace {
// Mara's report: a file with curves only dropped into the Topology room — they can only go to the Hair room (true — switch and import)
bool maraCurvesAsk(const QString& name) {
  QDialog d(QApplication::activeWindow(), Qt::Dialog | Qt::FramelessWindowHint); d.setObjectName("maraAsk");
  d.setStyleSheet("#maraAsk{background:#111725;border:2px solid #5a8dee;border-radius:16px}"
                  "#maraAsk QPushButton{background:#1f2638;border:1px solid #3a4568;color:#dde;border-radius:6px;padding:5px 12px;min-width:90px}"
                  "#maraAsk QPushButton[blue=\"true\"]{background:#3b6fd8;border-color:#5a8dee;color:#fff}");
  auto* h = new QHBoxLayout(&d); h->setContentsMargins(14, 14, 14, 14); h->setSpacing(16);
  auto* mara = new QLabel; mara->setFixedSize(150, 150); mara->setScaledContents(true); mara->setPixmap(ui::image("mara")); extras::attachMara(mara); h->addWidget(mara, 0, Qt::AlignVCenter);
  auto* mid = new QVBoxLayout; mid->setSpacing(8);
  auto* title = new QLabel(QObject::tr("Inspector's Report: curves found")); title->setStyleSheet("font-weight:600;font-size:14px;color:#fff"); title->setWordWrap(true);
  auto* text = new QLabel(QObject::tr("%1 holds curves that can only be imported into the Hair room. Skip importing them, or load them into the Hair room?").arg(name));
  text->setStyleSheet("color:#c8cbd8;font-size:12px"); text->setWordWrap(true);
  auto* bar = new QHBoxLayout; bar->setSpacing(8); bar->addStretch(1);
  auto* skip = new QPushButton(QObject::tr("Skip")); auto* go = new QPushButton(QObject::tr("Load into the Hair room")); go->setProperty("blue", true); go->setDefault(true);
  bar->addWidget(skip); bar->addWidget(go);
  mid->addWidget(title); mid->addWidget(text); mid->addStretch(1); mid->addLayout(bar); h->addLayout(mid, 1);
  d.setFixedWidth(560);
  QObject::connect(go, &QPushButton::clicked, &d, &QDialog::accept); QObject::connect(skip, &QPushButton::clicked, &d, &QDialog::reject);
  return d.exec() == QDialog::Accepted;
}

// the decimation question: → a share of the triangles (0…1), 2 — as is, −1 — cancelled
double decimateAsk(QWidget* parent, const QString& name, int npoly, int ntri, bool canAsIs) {
  QDialog d(parent); d.setWindowTitle(QObject::tr("Decimate %1?").arg(name)); d.setMinimumWidth(440);
  auto* v = new QVBoxLayout(&d);
  auto* t = new QLabel(QObject::tr("A heavy model: %1 polygons (%2 triangles). It can be loaded lighter — decimated per polygroup, the polygroup seams kept; topologically identical parts (mirrored eyes, ears, boots…) are decimated the same way and stay identical.")
                         .arg(QLocale().toString(npoly)).arg(QLocale().toString(ntri)));
  t->setWordWrap(true); t->setStyleSheet("color:#aaa"); v->addWidget(t);
  auto* row = new QHBoxLayout; auto* l = new QLabel(QObject::tr("Keep")); auto* s = new QSlider(Qt::Horizontal); s->setRange(1, 100);
  s->setValue(std::clamp(int(std::lround(1e6 / ntri * 100)), 1, 100)); auto* val = new QLabel; val->setMinimumWidth(150); val->setAlignment(Qt::AlignRight);
  auto show = [=] { val->setText(QObject::tr("%1% → %2 tris").arg(s->value()).arg(QLocale().toString(qlonglong(std::llround(double(ntri) * s->value() / 100))))); }; show();
  QObject::connect(s, &QSlider::valueChanged, &d, show);
  row->addWidget(l); row->addWidget(s, 1); row->addWidget(val); v->addLayout(row);
  auto* n = new QLabel(QObject::tr("As is: every polygon kept — while the camera moves a lighter copy (made in the background) is shown.")); n->setStyleSheet("color:#888;font-size:11px"); n->setWordWrap(true); v->addWidget(n);
  auto* bar = new QHBoxLayout; bar->addStretch(1);
  auto* as = new QPushButton(QObject::tr("Import as is")); as->setEnabled(canAsIs); as->setToolTip(canAsIs ? QObject::tr("Every polygon (slow to draw and to edit)") : QObject::tr("Too big to load whole"));
  auto* cn = new QPushButton(QObject::tr("Cancel")); auto* go = new QPushButton(QObject::tr("Decimate")); go->setProperty("primary", true); go->setDefault(true);
  bar->addWidget(as); bar->addWidget(cn); bar->addWidget(go); v->addLayout(bar);
  double r = -1;
  QObject::connect(go, &QPushButton::clicked, &d, [&] { r = s->value() / 100.0; d.accept(); });
  QObject::connect(as, &QPushButton::clicked, &d, [&] { r = 2; d.accept(); });
  QObject::connect(cn, &QPushButton::clicked, &d, &QDialog::reject);
  d.setWindowModality(Qt::WindowModal);
  d.show(); d.raise(); d.activateWindow();
  d.exec();
  return r;
}
}  // namespace

void topoImportFiles(const QStringList& files, const TopoImport& o, Viewport* vp, Scene* sc) {
  std::map<QString, QString> extraAll; for (const QString& f : files) extraAll[QFileInfo(f).fileName().toLower()] = f;
  QStringList models; for (const QString& f : files) if (MODEL_EXTS.contains(extOf(f))) models << f;
  if (models.isEmpty()) { ui::toast(QObject::tr("Pick the model (.obj / .glb / .gltf / .fbx) — with its texture images and .mtl if you like")); return; }
  const bool objects = (o.groups || o.parts) && o.objects;
  for (const QString& f : models) {
    const QString ext = extOf(f), base = QFileInfo(f).completeBaseName(); auto extra = extraAll; extra.erase(QFileInfo(f).fileName().toLower());
    QJsonObject src; std::shared_ptr<PolyMesh> B; QByteArray bytes;
    { ui::BusyScope bz(QObject::tr("Importing %1").arg(QFileInfo(f).fileName()));
      src = sourceOf(f, ext, extra); if (!o.groups) src.insert("flat", true); if (o.parts) src.insert("split", true); optFlags(src, o);
      QString err; bytes = bytesOf(src);
      B = importMesh(ext, bytes, &err, !o.groups); if (B && o.parts) splitPartsIntoGroups(*B); if (B) applySourceOptions(*B, src);
      if (!B || B->pos.empty()) {
        if (!importCurves(ext, bytes).empty()) {
          if (o.quiet || maraCurvesAsk(QFileInfo(f).fileName())) { vp->switchRoom("hair"); int n = 0; vp->runEdit([&](Scene& s, Selection&, const CurveCache&) -> QString { n = importFileCurves(s, ext, bytes, base);
                            return n ? QObject::tr("%1 curve%2 from %3").arg(n).arg(n == 1 ? "" : "s").arg(base) : QString(); }); }
          continue; }   // (the Topology room has no curves of its own)
        ui::toast(QObject::tr("Could not load the file%1").arg(err.isEmpty() ? QString() : ": " + err)); continue; } }
    const int nf = B->faceCount(), ntri = topo::triCount(*B);
    double ratio = 2;
    if (ntri > 1000000 && !o.quiet && !qEnvironmentVariableIsSet("HM_IMPORT_ASIS")) { ratio = decimateAsk(QApplication::activeWindow(), QFileInfo(f).fileName(), nf, ntri, true); if (ratio < 0) return; }
    struct Made { QString name; QJsonObject src; std::shared_ptr<PolyMesh> poly; QJsonObject look; bool live = false; };
    std::vector<Made> made; QString msg;
    if (ratio < 1) {
      topo::Dec dd; std::atomic<double> prog{0}; std::atomic<bool> done{false};
      { ui::BusyScope bz(QObject::tr("Decimating %1").arg(QFileInfo(f).fileName()));
        std::thread th([&] { dd = topo::decimateByGroups(*B, ratio, [&](double p) { prog = p; }); done = true; });
        while (!done) { bz.progress(prog); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50); std::this_thread::sleep_for(std::chrono::milliseconds(30)); }
        th.join(); }
      if (objects && dd.groups.size() > 1) for (size_t gi = 0; gi < dd.groups.size(); gi++) { auto P = std::make_shared<PolyMesh>(topo::decSubset(dd, int(gi), dd.groups[gi])); if (P->faceCount()) made.push_back({dd.groups[gi], {}, P, {}, true}); }
      else made.push_back({base, QJsonObject{{"ext", "hmesh"}, {"b64", QString::fromLatin1(topo::packHMesh(dd).toBase64())}}, std::make_shared<PolyMesh>(topo::decMesh(dd)), {}, false});
      msg = QObject::tr("Imported %1: %2 → %3 triangles").arg(base).arg(QLocale().toString(ntri)).arg(QLocale().toString(qlonglong(dd.idx.size() / 3)))
          + (dd.twins ? QObject::tr(" · %1 identical part%2 decimated the same way").arg(dd.twins).arg(dd.twins == 1 ? "" : "s") : QString());
    } else if (objects && B->groupNames.size() > 1) {               // (each polygroup a mesh of its own, cut from the read mesh)
      for (size_t gi = 0; gi < B->groupNames.size(); gi++) {
        std::vector<char> keep(static_cast<size_t>(nf)); for (int k = 0; k < nf; k++) keep[size_t(k)] = B->faceGroup[size_t(k)] == int(gi);
        auto P = std::make_shared<PolyMesh>(topo::subset(*B, keep)); std::fill(P->faceGroup.begin(), P->faceGroup.end(), 0); P->groupNames = {B->groupNames[gi]};
        if (!P->faceCount()) continue;
        Made m{B->groupNames[gi], {}, P, {}, true}; if (!P->cornerUV.empty() && o.textures) m.look = importLook(*P, extra); made.push_back(m);
      }
    } else { Made m{base, src, B, {}, false}; if (!B->cornerUV.empty() && o.textures) { m.look = importLook(*B, extra); } made.push_back(m); }
    if (made.empty()) { ui::toast(QObject::tr("No geometry in the file")); continue; }
    Box3 box; std::vector<int> ids; const Box3 before = sceneContentBox(*sc);   // (what the scene had: a model of another scale is asked about)
    vp->runEdit([&](Scene& s, Selection& sel, const CurveCache&) -> QString {
      int g = importGroup(s); s.item(g)->expanded = true; int parent = g;
      if (objects && made.size() > 1) { Item& sub = s.addItem(ItemType::Group, g, base);
        sub.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "hide"}, {"conv", QJsonValue()}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                              {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
        sub.expanded = true; parent = sub.id; }
      for (Made& m : made) {
        Item& it = newIMesh(s, parent, m.name, m.live ? QJsonObject{{"ext", "pmesh"}, {"live", true}} : m.src, m.poly);
        it.base = m.poly;
        if (!m.look.isEmpty()) it.raw.insert("look", m.look);
        it.raw.insert("pivot", QJsonArray{0, 0, 0});          // (the file's origin: the mesh turns and scales about it)
        if (!m.poly->cornerNormal.empty()) it.raw.insert("smoothN", true);   // (the file's own normals: seen with smooth shading)
        box.unite(m.poly->bounds()); ids.push_back(it.id);
      }
      if (o.curves) if (int n = importFileCurves(s, ext, bytes, base)) msg = (msg.isEmpty() ? QObject::tr("Imported %1").arg(base) : msg) + QObject::tr(" · %1 curve%2").arg(n).arg(n == 1 ? "" : "s");
      sel.anchors.clear(); sel.items = ids;
      if (!msg.isEmpty()) return msg;
      const PolyMesh& p0 = *made[0].poly;
      return made.size() > 1 ? QObject::tr("Imported %1: %2 meshes").arg(base).arg(made.size())
                             : QObject::tr("Imported %1 (%2 vertices, %3 polygroup%4)%5").arg(base).arg(QLocale().toString(qlonglong(p0.pos.size()))).arg(p0.groupNames.size()).arg(p0.groupNames.size() == 1 ? "" : "s").arg(o.head ? QObject::tr(" — as the head") : QString());
    });
    if (!o.head) if (auto k = offerFitScale(vp, before, box, base)) {   // (fitted: about the file's origin, through each mesh's Transform)
      vp->runEdit([&](Scene& s, Selection&, const CurveCache&) { scaleMeshes(s, ids, *k, Vec3(), Vec3()); return QObject::tr("%1 scaled ×%2 to the project").arg(base).arg(QString::number(*k, 'g', 4)); });
      Box3 nb; nb.expand(box.min * *k); nb.expand(box.max * *k); nb.unite(before); box = nb; }   // (in view with what was there)
    vp->fitToBox(box);                                         // (20.11: always onto what came in — not only into an empty scene: a model of another scale stood the camera inside it)
  }
}

void importAuto(const QStringList& files, Viewport* vp, Scene* sc) {
  QStringList models, extras; for (const QString& f : files) (MODEL_EXTS.contains(extOf(f)) ? models : extras) << f;
  if (models.isEmpty()) { ui::toast(QObject::tr("Models: .obj, .glb, .gltf or .fbx")); return; }
  TopoImport o; o.head = false; o.groups = true; o.objects = false; o.parts = false;
  o.uvs = o.textures = o.creases = o.hard = o.polypaint = true; o.curves = true; o.quiet = true;   // (what the file has comes with it)
  for (const QString& m : models) {
    if (sc->room != "hair") topoImportFiles(QStringList{m} + extras, o, vp, sc);
    else importFilesWith(QStringList{m} + extras, o, vp, sc);
  }
}

void topoImportMenu(QWidget* anchor, Viewport* vp, Scene* sc) {
  QSettings st; QJsonObject T = QJsonDocument::fromJson(st.value("topo/import").toByteArray()).object();
  TopoImport o; o.groups = T.value("groups").toBool(true); o.objects = T.value("objects").toBool(false); o.parts = T.value("parts").toBool(false);
  o.head = false;
  QMenu m(anchor); m.setToolTipsVisible(true);
  auto* box = new QWidget; auto* v = new QVBoxLayout(box); v->setContentsMargins(6, 6, 6, 6); v->setSpacing(4);
  auto* gb = new QCheckBox(QObject::tr("Import polygroups")); gb->setChecked(o.groups); gb->setToolTip(QObject::tr("Keep the file's polygroups (OBJ g / o, the meshes of a glTF / FBX file); off — one mesh, one group"));
  auto* r1 = new QRadioButton(QObject::tr("as polygroups")); auto* r2 = new QRadioButton(QObject::tr("as separate objects"));
  r1->setToolTip(QObject::tr("One mesh, its polygroups listed in its settings")); r2->setToolTip(QObject::tr("Every polygroup a mesh of its own, in a group named after the file"));
  (o.objects ? r2 : r1)->setChecked(true); r1->setStyleSheet("margin-left:16px"); r2->setStyleSheet("margin-left:16px");
  auto* pb = new QCheckBox(QObject::tr("Split separate parts into polygroups")); pb->setChecked(o.parts);
  pb->setToolTip(QObject::tr("Every separate (unconnected) part of the model becomes a polygroup of its own — for a model made of pieces that has no polygroups (eyes, teeth, buttons…)"));
  auto sync = [=] { bool on = gb->isChecked() || pb->isChecked(); r1->setEnabled(on); r2->setEnabled(on); }; sync();
  QObject::connect(gb, &QCheckBox::toggled, box, sync); QObject::connect(pb, &QCheckBox::toggled, box, sync);
  auto* go = new QPushButton(QObject::tr("◆ Import model…")); go->setProperty("primary", true); go->setToolTip(QObject::tr("OBJ / GLB / GLTF / FBX — with its textures and .mtl if you like"));
  for (QWidget* w : std::initializer_list<QWidget*>{gb, r1, r2, pb, go}) v->addWidget(w);
  auto* wa = new QWidgetAction(&m); wa->setDefaultWidget(box); m.addAction(wa);
  bool pick = false; QObject::connect(go, &QPushButton::clicked, &m, [&] { pick = true; m.close(); });
  m.exec(anchor->mapToGlobal(QPoint(0, anchor->height() + 2)));
  o.head = false; o.groups = gb->isChecked(); o.objects = r2->isChecked(); o.parts = pb->isChecked();
  st.setValue("topo/import", QJsonDocument(QJsonObject{{"groups", o.groups}, {"objects", o.objects}, {"parts", o.parts}}).toJson(QJsonDocument::Compact));
  if (!pick) return;
  QStringList fs = QFileDialog::getOpenFileNames(anchor, QObject::tr("Import a model"), st.value("importDir").toString(),
                                                 QObject::tr("Models and their textures (*.obj *.glb *.gltf *.fbx *.mtl *.bin *.png *.jpg *.jpeg *.webp *.bmp *.gif *.tga);;All files (*)"));
  if (fs.isEmpty()) return;
  st.setValue("importDir", QFileInfo(fs[0]).absolutePath());
  topoImportFiles(fs, o, vp, sc);
}

}  // namespace hm
