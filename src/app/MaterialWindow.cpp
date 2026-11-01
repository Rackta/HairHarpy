#include "app/MaterialWindow.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>
#include <optional>

namespace hm {

namespace {
struct Mat { QString name; QColor color{0x9a, 0x9a, 0x9a}; double rough = 0.7, metal = 0, spec = 1; };
const Mat BUILTIN[] = {{"Sculpt clay", QColor("#a99a86"), 0.55, 0, 0.35}, {"Clay", QColor("#b9a99a"), 0.85, 0, 0.2}, {"Skin", QColor("#e6b8a2"), 0.55, 0, 0.45},
                       {"Plastic", QColor("#d4d8de"), 0.32, 0, 0.8}, {"Rubber", QColor("#3b3b3e"), 0.92, 0, 0.1}, {"Metal", QColor("#c9ccd1"), 0.3, 1, 1},
                       {"Gold", QColor("#e3b65a"), 0.26, 1, 1}, {"Jade", QColor("#5ea476"), 0.4, 0, 0.7}, {"Wax", QColor("#b4423c"), 0.6, 0, 0.4}};

QJsonObject toJson(const Mat& m) { return {{"color", m.color.name()}, {"roughness", m.rough}, {"metalness", m.metal}, {"specular", m.spec}}; }
Mat fromJson(const QJsonObject& o, const QString& name = {}) {
  Mat m; m.name = name.isEmpty() ? o.value("name").toString() : name; if (QColor c(o.value("color").toString()); c.isValid()) m.color = c;
  m.rough = std::clamp(o.value("roughness").toDouble(m.rough), 0.0, 1.0); m.metal = std::clamp(o.value("metalness").toDouble(m.metal), 0.0, 1.0);
  m.spec = std::clamp(o.value("specular").toDouble(m.spec), 0.0, 1.0); return m;
}
std::vector<Mat> userPresets() {
  std::vector<Mat> out; for (const QJsonValue& v : QJsonDocument::fromJson(QSettings().value("materials/user").toByteArray()).array()) out.push_back(fromJson(v.toObject()));
  return out;
}
void saveUserPresets(const std::vector<Mat>& L) {
  QJsonArray a; for (auto& m : L) { QJsonObject o = toJson(m); o.insert("name", m.name); a.append(o); }
  QSettings().setValue("materials/user", QJsonDocument(a).toJson(QJsonDocument::Compact));
}
// (a linear colour of a renderer's file → the screen's)
double toSrgb(double x) { x = std::clamp(x, 0.0, 1.0); return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055; }
QColor rgbF(double r, double g, double b, bool linear) { if (linear) { r = toSrgb(r); g = toSrgb(g); b = toSrgb(b); } return QColor::fromRgbF(float(std::clamp(r, 0.0, 1.0)), float(std::clamp(g, 0.0, 1.0)), float(std::clamp(b, 0.0, 1.0))); }

// ---- a material read from a file: this program's / another's ----
const char* FORMATS =
    "Imported (the colour, roughness, metalness and specular are read — texture maps are not):\n"
    "• This program's presets — .hmmat\n"
    "• Wavefront .mtl — with an OBJ from Blender, Maya, 3ds Max, Cinema 4D, Substance Painter… (Kd, Ns, Ks; Pr / Pm if there)\n"
    "• glTF .gltf — Blender, Substance, Sketchfab, three.js… (pbrMetallicRoughness, KHR_materials_specular)\n"
    "• Unity .mat — Standard / URP / HDRP (_Color / _BaseColor, _Metallic, _Glossiness / _Smoothness)\n"
    "• Godot .tres / .material — StandardMaterial3D (albedo_color, metallic, roughness, metallic_specular)\n"
    "• three.js material .json (color, roughness, metalness)\n"
    "A file with several materials: the first one.";
std::optional<Mat> importFile(const QString& path, QString* err) {
  QFile f(path); if (!f.open(QIODevice::ReadOnly)) { *err = QObject::tr("Can't read the file"); return std::nullopt; }
  const QByteArray bytes = f.readAll(); const QString text = QString::fromUtf8(bytes); const QString ext = QFileInfo(path).suffix().toLower();
  Mat m; m.name = QFileInfo(path).completeBaseName(); bool any = false;
  auto num = [&](const QString& re) -> std::optional<double> { auto x = QRegularExpression(re, QRegularExpression::MultilineOption).match(text); if (!x.hasMatch()) return std::nullopt; bool ok = false; double d = x.captured(1).toDouble(&ok); return ok ? std::optional<double>(d) : std::nullopt; };
  if (ext == "mtl") {                                            // Wavefront: the first newmtl
    QString body = text; int a = text.indexOf("newmtl"); if (a >= 0) { int b = text.indexOf("newmtl", a + 6); body = text.mid(a, b > 0 ? b - a : -1); m.name = body.section('\n', 0, 0).mid(6).trimmed(); }
    auto rx = [&](const QString& key) { return QRegularExpression("^\\s*" + key + "\\s+([-0-9.eE]+)(?:\\s+([-0-9.eE]+)\\s+([-0-9.eE]+))?", QRegularExpression::MultilineOption).match(body); };
    if (auto k = rx("Kd"); k.hasMatch()) { double r = k.captured(1).toDouble(), g = k.captured(2).isEmpty() ? r : k.captured(2).toDouble(), b = k.captured(3).isEmpty() ? r : k.captured(3).toDouble(); m.color = rgbF(r, g, b, false); any = true; }
    if (auto k = rx("Pr"); k.hasMatch()) { m.rough = std::clamp(k.captured(1).toDouble(), 0.0, 1.0); any = true; }
    else if (auto n = rx("Ns"); n.hasMatch()) { m.rough = std::clamp(1 - std::sqrt(std::clamp(n.captured(1).toDouble(), 0.0, 1000.0) / 1000.0), 0.0, 1.0); any = true; }   // (shininess 0..1000 → rough)
    if (auto k = rx("Pm"); k.hasMatch()) { m.metal = std::clamp(k.captured(1).toDouble(), 0.0, 1.0); any = true; }
    if (auto k = rx("Ks"); k.hasMatch()) { double r = k.captured(1).toDouble(), g = k.captured(2).isEmpty() ? r : k.captured(2).toDouble(), b = k.captured(3).isEmpty() ? r : k.captured(3).toDouble(); m.spec = std::clamp((r + g + b) / 3, 0.0, 1.0); any = true; }
  } else if (ext == "gltf" || ext == "json" || ext == "hmmat") {
    const QJsonObject o = QJsonDocument::fromJson(bytes).object();
    if (o.value("type").toString() == "material" || o.contains("roughness") && o.value("color").isString()) { m = fromJson(o, o.value("name").toString(m.name)); any = true; }   // (this program's)
    else if (o.value("materials").isArray() && !o.value("materials").toArray().isEmpty()) {   // glTF
      const QJsonObject g = o.value("materials").toArray().at(0).toObject(); m.name = g.value("name").toString(m.name);
      const QJsonObject pbr = g.value("pbrMetallicRoughness").toObject(); const QJsonArray c = pbr.value("baseColorFactor").toArray();
      if (c.size() >= 3) m.color = rgbF(c[0].toDouble(), c[1].toDouble(), c[2].toDouble(), true);
      m.metal = std::clamp(pbr.value("metallicFactor").toDouble(1), 0.0, 1.0); m.rough = std::clamp(pbr.value("roughnessFactor").toDouble(1), 0.0, 1.0);
      const QJsonObject sp = g.value("extensions").toObject().value("KHR_materials_specular").toObject(); if (sp.contains("specularFactor")) m.spec = std::clamp(sp.value("specularFactor").toDouble(1), 0.0, 1.0);
      any = true;
    } else if (o.contains("color") || o.value("metadata").toObject().value("type").toString() == "Material") {   // three.js
      const QJsonValue c = o.value("color"); if (c.isDouble()) { const int v = c.toInt(); m.color = QColor((v >> 16) & 255, (v >> 8) & 255, v & 255); }
      m.rough = std::clamp(o.value("roughness").toDouble(m.rough), 0.0, 1.0); m.metal = std::clamp(o.value("metalness").toDouble(m.metal), 0.0, 1.0); m.name = o.value("name").toString(m.name); any = true;
    }
  } else if (ext == "mat") {                                     // Unity (YAML)
    if (auto x = QRegularExpression("m_Name:\\s*(.+)").match(text); x.hasMatch()) m.name = x.captured(1).trimmed();
    auto colorOf = [&](const QString& key) -> std::optional<QColor> { auto x = QRegularExpression("-\\s*" + key + ":\\s*\\{r:\\s*([-0-9.eE]+),\\s*g:\\s*([-0-9.eE]+),\\s*b:\\s*([-0-9.eE]+)").match(text);
      return x.hasMatch() ? std::optional<QColor>(rgbF(x.captured(1).toDouble(), x.captured(2).toDouble(), x.captured(3).toDouble(), false)) : std::nullopt; };
    if (auto c = colorOf("_BaseColor")) { m.color = *c; any = true; } else if (auto c2 = colorOf("_Color")) { m.color = *c2; any = true; }
    if (auto v = num("-\\s*_Metallic:\\s*([-0-9.eE]+)")) { m.metal = std::clamp(*v, 0.0, 1.0); any = true; }
    if (auto v = num("-\\s*_Smoothness:\\s*([-0-9.eE]+)")) { m.rough = std::clamp(1 - *v, 0.0, 1.0); any = true; }
    else if (auto v2 = num("-\\s*_Glossiness:\\s*([-0-9.eE]+)")) { m.rough = std::clamp(1 - *v2, 0.0, 1.0); any = true; }
  } else if (ext == "tres" || ext == "material") {              // Godot
    if (auto x = QRegularExpression("albedo_color\\s*=\\s*Color\\(\\s*([-0-9.eE]+),\\s*([-0-9.eE]+),\\s*([-0-9.eE]+)").match(text); x.hasMatch()) { m.color = rgbF(x.captured(1).toDouble(), x.captured(2).toDouble(), x.captured(3).toDouble(), false); any = true; }
    if (auto v = num("^metallic\\s*=\\s*([-0-9.eE]+)")) { m.metal = std::clamp(*v, 0.0, 1.0); any = true; }
    if (auto v = num("^roughness\\s*=\\s*([-0-9.eE]+)")) { m.rough = std::clamp(*v, 0.0, 1.0); any = true; }
    if (auto v = num("^metallic_specular\\s*=\\s*([-0-9.eE]+)")) { m.spec = std::clamp(*v, 0.0, 1.0); any = true; }
    if (auto x = QRegularExpression("resource_name\\s*=\\s*\"([^\"]*)\"").match(text); x.hasMatch()) m.name = x.captured(1);
  }
  if (!any) { *err = QObject::tr("No material found in it (the formats it reads: the ? button)"); return std::nullopt; }
  return m;
}

// ---- a row: the name left, its control right (as the reference's list) ----
class MatWindow : public QWidget {
public:
  MatWindow(Viewport* vp, int id) : QWidget(nullptr, Qt::Tool), vp_(vp), id_(id) {
    for (QWidget* w : QApplication::topLevelWidgets()) if (w->inherits("QMainWindow")) { setParent(w, Qt::Tool); break; }
    setAttribute(Qt::WA_DeleteOnClose); setObjectName("matPop"); setMinimumSize(300, 230);
    setStyleSheet("#matPop{background:#262626} #matPop *{font-size:11px} QLabel{color:#cfcfcf} #rowName{color:#bdbdbd} #hint{color:#8a8a8a;font-size:10px}"
                  " QPushButton{padding:1px 6px;min-height:16px} QComboBox{min-height:16px;padding:0 4px}"
                  " QDoubleSpinBox{background:#1b1b1b;border:1px solid #3a3a3a;color:#e8e8e8;padding:0 2px;min-width:48px;max-height:16px}");
    const Item* it = vp->scene()->item(id); name_ = it ? it->name : QString();
    setWindowTitle(QObject::tr("Material — %1").arg(name_));
    // what was (Cancel / the window closed puts it back)
    if (it) { was_ = it->raw; cur_ = it->raw.value("mat").isObject() ? fromJson(it->raw.value("mat").toObject(), it->raw.value("matPreset").toString()) : BUILTIN[1]; }
    auto* v = new QVBoxLayout(this); v->setContentsMargins(7, 6, 7, 7); v->setSpacing(3);
    // ---- the presets ----
    auto* pr = new QHBoxLayout; pr->setSpacing(4);
    auto* pl = new QLabel(QObject::tr("Preset")); pl->setObjectName("rowName"); pl->setFixedWidth(64); pr->addWidget(pl);
    preset_ = new QComboBox; pr->addWidget(preset_, 1);
    auto* save = new QPushButton(QObject::tr("Save preset…")); save->setToolTip(QObject::tr("Keep these settings as a preset of your own (in this computer's settings)"));
    del_ = new QPushButton(QObject::tr("Delete")); del_->setToolTip(QObject::tr("Delete the chosen preset of your own"));
    pr->addWidget(save); pr->addWidget(del_); v->addLayout(pr);
    auto* fr = new QHBoxLayout; fr->setSpacing(4); fr->addSpacing(68);
    auto* ex = new QPushButton(QObject::tr("Export…")); ex->setToolTip(QObject::tr("Save these settings to a file (.hmmat) — to share them or move to another computer"));
    auto* im = new QPushButton(QObject::tr("Import…")); im->setToolTip(QObject::tr("Load a material from a file — this program's .hmmat or another program's (the ? button)"));
    auto* q = new QPushButton("?"); q->setFixedWidth(26); q->setToolTip(QObject::tr(FORMATS));
    fr->addWidget(ex); fr->addWidget(im); fr->addWidget(q); fr->addStretch(1); v->addLayout(fr);
    auto* line = new QFrame; line->setFrameShape(QFrame::HLine); line->setStyleSheet("color:#3a3a3a"); v->addWidget(line);
    // ---- the settings ----
    auto* g = new QGridLayout; g->setHorizontalSpacing(6); g->setVerticalSpacing(2); int r = 0;
    auto rowName = [&](const QString& t, const QString& tip) { auto* l = new QLabel(t); l->setObjectName("rowName"); l->setToolTip(tip); l->setFixedWidth(64); g->addWidget(l, r, 0); };
    rowName(QObject::tr("Color"), QObject::tr("The material's colour"));
    color_ = new QPushButton; color_->setFixedHeight(16); color_->setToolTip(QObject::tr("Click — choose the colour")); g->addWidget(color_, r, 1, 1, 2); r++;
    QObject::connect(color_, &QPushButton::clicked, this, [this] { QColor c = QColorDialog::getColor(cur_.color, this, QObject::tr("Colour")); if (c.isValid()) { cur_.color = c; cur_.name.clear(); show_(); } });
    auto slider = [&](const QString& t, const QString& tip, double Mat::*f) {
      rowName(t, tip);
      auto* s = new QSlider(Qt::Horizontal); s->setRange(0, 1000); auto* n = new QDoubleSpinBox; n->setRange(0, 1); n->setSingleStep(0.01); n->setDecimals(3);
      g->addWidget(s, r, 1); g->addWidget(n, r, 2); r++;
      QObject::connect(s, &QSlider::valueChanged, this, [this, n, f](int x) { if (sync_) return; cur_.*f = x / 1000.0; cur_.name.clear(); show_(); });
      QObject::connect(n, &QDoubleSpinBox::valueChanged, this, [this, f](double x) { if (sync_) return; cur_.*f = x; cur_.name.clear(); show_(); });
      rows_.push_back({s, n, f});
    };
    slider(QObject::tr("Roughness"), QObject::tr("1 — matte, 0 — glossy"), &Mat::rough);
    slider(QObject::tr("Metalness"), QObject::tr("How metallic: 1 — the highlights take the colour"), &Mat::metal);
    slider(QObject::tr("Specular"), QObject::tr("The strength of the highlights"), &Mat::spec);
    g->setColumnStretch(1, 1); v->addLayout(g);
    on_ = new QCheckBox(QObject::tr("Material on")); on_->setChecked(it && !it->raw.value("matOff").toBool());
    QObject::connect(on_, &QCheckBox::toggled, this, [this] { show_(); }); v->addWidget(on_);
    auto* hint = new QLabel(QObject::tr("Seen at once · OK keeps · Cancel puts back")); hint->setObjectName("hint"); hint->setWordWrap(true); v->addWidget(hint);
    v->addStretch(1);
    auto* bar = new QHBoxLayout; bar->addStretch(1);
    auto* ok = new QPushButton(QObject::tr("OK")); ok->setDefault(true); ok->setMinimumWidth(60); auto* cancel = new QPushButton(QObject::tr("Cancel")); cancel->setMinimumWidth(60);
    bar->addWidget(ok); bar->addSpacing(16); bar->addWidget(cancel); bar->addStretch(1); v->addLayout(bar);
    QObject::connect(ok, &QPushButton::clicked, this, [this] { keep_ = true; commit_(); close(); });
    QObject::connect(cancel, &QPushButton::clicked, this, &QWidget::close);
    // ---- the presets' actions ----
    QObject::connect(preset_, &QComboBox::activated, this, [this](int i) {
      const QString key = preset_->itemData(i).toString(); if (key.isEmpty()) return;
      for (auto& m : BUILTIN) if ("b:" + m.name == key) cur_ = m;
      for (auto& m : userPresets()) if ("u:" + m.name == key) cur_ = m;
      on_->setChecked(true); show_(); });
    QObject::connect(save, &QPushButton::clicked, this, [this] {
      bool ok = false; QString n = QInputDialog::getText(this, QObject::tr("Save preset"), QObject::tr("Preset name:"), QLineEdit::Normal, cur_.name.isEmpty() ? name_ : cur_.name, &ok).trimmed();
      if (!ok || n.isEmpty()) return; auto L = userPresets(); L.erase(std::remove_if(L.begin(), L.end(), [&](const Mat& m) { return m.name == n; }), L.end());
      Mat m = cur_; m.name = n; L.push_back(m); saveUserPresets(L); cur_.name = n; fillPresets(); show_(); ui::toast(QObject::tr("Preset \"%1\" saved").arg(n)); });
    QObject::connect(del_, &QPushButton::clicked, this, [this] {
      const QString key = preset_->currentData().toString(); if (!key.startsWith("u:")) return; const QString n = key.mid(2);
      auto L = userPresets(); L.erase(std::remove_if(L.begin(), L.end(), [&](const Mat& m) { return m.name == n; }), L.end()); saveUserPresets(L);
      cur_.name.clear(); fillPresets(); show_(); ui::toast(QObject::tr("Preset \"%1\" deleted").arg(n)); });
    QObject::connect(ex, &QPushButton::clicked, this, [this] {
      QSettings st; const QString n = cur_.name.isEmpty() ? name_ : cur_.name;
      QString p = QFileDialog::getSaveFileName(this, QObject::tr("Export the material"), QFileInfo(st.value("materials/lastDir").toString(), n + ".hmmat").filePath(), QObject::tr("Material (*.hmmat)"));
      if (p.isEmpty()) return; st.setValue("materials/lastDir", QFileInfo(p).absolutePath());
      QJsonObject o = toJson(cur_); o.insert("app", "HairHarpy"); o.insert("type", "material"); o.insert("name", n);
      QFile f(p); if (!f.open(QIODevice::WriteOnly)) { ui::toast(QObject::tr("Can't write the file")); return; }
      f.write(QJsonDocument(o).toJson()); ui::toast(QObject::tr("Material saved: %1").arg(QFileInfo(p).fileName())); });
    QObject::connect(im, &QPushButton::clicked, this, [this] {
      QSettings st; QString p = QFileDialog::getOpenFileName(this, QObject::tr("Import a material"), st.value("materials/lastDir").toString(),
                                                            QObject::tr("Materials (*.hmmat *.mtl *.gltf *.mat *.tres *.material *.json);;All files (*)"));
      if (p.isEmpty()) return; st.setValue("materials/lastDir", QFileInfo(p).absolutePath());
      QString err; auto m = importFile(p, &err); if (!m) { QMessageBox::warning(this, QObject::tr("Import a material"), err); return; }
      cur_ = *m; on_->setChecked(true); show_(); ui::toast(QObject::tr("Material imported: %1 — Save preset… keeps it").arg(m->name)); });
    QObject::connect(q, &QPushButton::clicked, this, [this] { QMessageBox::information(this, QObject::tr("Supported materials"), QObject::tr(FORMATS)); });
    fillPresets(); show_();
  }
  ~MatWindow() override {
    QSettings().setValue("materialWindow/size3", size());
    if (!keep_) if (Item* it = vp_->scene()->item(id_)) { it->raw = was_; vp_->liveView(); vp_->topoDisplayChanged(); }   // (closed, not OK: as it was)
  }
  int id() const { return id_; }
private:
  struct Row { QSlider* s; QDoubleSpinBox* n; double Mat::*f; };
  void fillPresets() {
    sync_ = true; preset_->clear(); preset_->addItem(QObject::tr("— choose a preset —"), QString());
    for (auto& m : BUILTIN) preset_->addItem(m.name, "b:" + m.name);
    auto U = userPresets(); if (!U.empty()) { preset_->insertSeparator(preset_->count()); for (auto& m : U) preset_->addItem("★ " + m.name, "u:" + m.name); }
    sync_ = false;
  }
  // the settings into the controls, the mesh shown with them (not kept yet)
  void show_() {
    sync_ = true;
    color_->setStyleSheet(QString("QPushButton{background:%1;border:1px solid #555;border-radius:3px}").arg(cur_.color.name()));
    for (auto& r : rows_) { r.s->setValue(int(std::lround(cur_.*r.f * 1000))); r.n->setValue(cur_.*r.f); }
    int pi = 0; for (int i = 0; i < preset_->count(); i++) { const QString k = preset_->itemData(i).toString(); if (!cur_.name.isEmpty() && (k == "b:" + cur_.name || k == "u:" + cur_.name)) pi = i; }
    preset_->setCurrentIndex(pi); del_->setEnabled(preset_->currentData().toString().startsWith("u:"));
    sync_ = false;
    if (Item* it = vp_->scene()->item(id_)) {
      it->raw.insert("mat", toJson(cur_)); it->raw.insert("matPreset", cur_.name); if (on_->isChecked()) it->raw.remove("matOff"); else it->raw.insert("matOff", true);
      vp_->liveView();
    }
  }
  // OK: what was put back, then the new one written as one undo step
  void commit_() {
    QJsonObject now; if (Item* it = vp_->scene()->item(id_)) { now = it->raw; it->raw = was_; } else return;
    const int id = id_;
    vp_->runEdit([id, now](Scene& s, Selection&, const CurveCache&) { if (Item* it = s.item(id)) it->raw = now; return QString(); }, false);
    vp_->topoDisplayChanged();
  }
  Viewport* vp_; int id_; QString name_; QJsonObject was_; Mat cur_; bool keep_ = false, sync_ = false;
  QComboBox* preset_ = nullptr; QPushButton *del_ = nullptr, *color_ = nullptr; QCheckBox* on_ = nullptr; std::vector<Row> rows_;
};
QPointer<MatWindow> g_mat;
}  // namespace

QRect viewportZone(Viewport* vp, const QSize& s) {
  const QPoint o = vp->mapToGlobal(QPoint(0, 0)); const QRect vr(o, vp->size());
  QRect r(vr.right() - s.width() - 24, vr.top() + 36, s.width(), s.height());
  if (QScreen* sc = QGuiApplication::screenAt(vr.center())) { const QRect a = sc->availableGeometry();   // (kept on the screen: its title bar reachable)
    r.moveLeft(std::clamp(r.left(), a.left(), std::max(a.left(), a.right() - r.width()))); r.moveTop(std::clamp(r.top(), a.top() + 30, std::max(a.top() + 30, a.bottom() - r.height()))); }
  return r;
}

void openMaterialWindow(Viewport* vp, int id) {
  if (g_mat && g_mat->id() != id) g_mat->close();               // (another mesh's: closed, its changes put back)
  if (!g_mat) {
    g_mat = new MatWindow(vp, id);
    QSize s = QSettings().value("materialWindow/size3", QSize(340, 250)).toSize(); s = s.expandedTo(g_mat->minimumSize());
    g_mat->resize(s); const QRect z = viewportZone(vp, g_mat->frameGeometry().size().expandedTo(s)); g_mat->move(z.topLeft());
  }
  g_mat->show(); g_mat->raise(); g_mat->activateWindow();
}

}  // namespace hm
