#include "scene/Toon.h"
#include <QJsonValue>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace hm::toon {

const char* slotName(int s) {
  static const char* N[SlotCount] = {"BaseColor", "ShadeColor", "ShadowMask", "Specular", "RimMask", "Emission", "Normal", "Flow", "FaceSDF", "OutlineWidth", "OverHair", "ID"};
  return s >= 0 && s < SlotCount ? N[s] : "";
}
bool slotSrgb(int s) { return s == SBase || s == SShade || s == SEmission; }

const std::vector<ModuleDef>& modules() {
  static const std::vector<ModuleDef> M = {
    {"base", "Base colour", "The lit colour: the Base colour map (or the model's own texture) times the tint"},
    {"bands", "Light steps", "The light cut into steps by the angle to the light: lit, the terminator, the penumbra, the shadow (Guilty Gear's / your MF_MainShader's 1-2-3)"},
    {"line", "Terminator line", "A coloured line where the light turns into the penumbra — the warm edge of a cel shadow"},
    {"shade", "Shade colour", "The colour in the shadow: the Shade colour map where it is painted; else made from the base colour — its hue turned, saturation and value changed — and tinted"},
    {"mask", "Shadow mask", "How far the Shadow mask (50 % gray — no change) moves the place where the light turns"},
    {"normal", "Normals", "The Normal map of the Paint room (the painted bump, the bake through its mask) bending the light"},
    {"spec", "Highlight", "A hard highlight where the surface faces the light and the eye, through the Highlight mask"},
    {"aniso", "Hair highlight", "The anisotropic band across the strands (Kajiya-Kay): along the Flow map, or along the UV's V / U; a second band shifted"},
    {"rim", "Rim light", "A rim on the edge seen from the camera, through the Rim mask"},
    {"sdf", "Face shadow (SDF)", "The face's shadow from a signed-distance picture as Genshin's: the light's turn about the head (the head locator) against the picture — a clean nose / cheek shadow at every angle"},
    {"emission", "Emission", "What glows by itself: the Emission map times its colour"},
    {"outline", "Outline", "The inverted hull: the mesh pushed out along its normals by the width (screen pixels) times the Outline width map, its back faces in the line's colour"},
    {"over", "Over the hair", "For the parts whose role is Over the hair (brows, lashes): how strongly they show through the hair (times the Over hair map), and between which angles of the face to the camera (the head locator) — none from the side and the back"},
    {"matid", "Material ID", "The Material ID map's zones (red, green, blue, the rest) each tint the shadow its own way"},
    {"light", "Light", "The light's strength and the ambient added everywhere"},
  };
  return M;
}

const std::vector<ParamDef>& params() {
  static const std::vector<ParamDef> P = {
    {"baseTint", "Tint", "base", 'c', 0, 0, 0, "Multiplies the base colour"},
    {"terminator", "Terminator", "bands", 'f', 0, 1, 0.55, "Where the light turns: above — lit, below — the penumbra"},
    {"shadow", "Shadow", "bands", 'f', 0, 1, 0.35, "Below it: the full shadow (between it and the terminator — the penumbra)"},
    {"soft", "Softness", "bands", 'f', 0, 0.2, 0.015, "How soft the steps' edges are"},
    {"lineOn", "On", "line", 'b', 0, 1, 1, ""},
    {"lineColor", "Colour", "line", 'c', 0, 0, 0, ""},
    {"lineWidth", "Width", "line", 'f', 0, 0.2, 0.02, ""},
    {"lineIntensity", "Strength", "line", 'f', 0, 1, 0.3, ""},
    {"shadeColor", "Tint", "shade", 'c', 0, 0, 0, "The shadow's tint (painted Shade colour maps are used as they are)"},
    {"shadeTint", "Tint amount", "shade", 'f', 0, 1, 0.35, "How much of the tint goes into the shade made from the base colour"},
    {"shadeHue", "Hue turn", "shade", 'f', -0.5, 0.5, 0.03, "The base colour's hue turned in the shadow (of a full turn)"},
    {"shadeSat", "Saturation", "shade", 'f', 0, 3, 1.25, ""},
    {"shadeVal", "Value", "shade", 'f', 0, 1.5, 0.62, ""},
    {"penumbraMix", "Penumbra mix", "shade", 'f', 0, 1, 0.5, "The penumbra between the shade (0) and the lit colour (1)"},
    {"maskShift", "Strength", "mask", 'f', 0, 1, 0.5, "How far the Shadow mask moves the terminator"},
    {"normalOn", "On", "normal", 'b', 0, 1, 1, ""},
    {"normalStrength", "Strength", "normal", 'f', 0, 2, 1, ""},
    {"specOn", "On", "spec", 'b', 0, 1, 1, ""},
    {"specColor", "Colour", "spec", 'c', 0, 0, 0, ""},
    {"specSize", "Size", "spec", 'f', 0, 1, 0.05, "How wide the highlight is"},
    {"specSoft", "Softness", "spec", 'f', 0, 0.2, 0.01, ""},
    {"specIntensity", "Strength", "spec", 'f', 0, 2, 0.25, ""},
    {"anisoOn", "On", "aniso", 'b', 0, 1, 0, "Usually on the hair's materials only"},
    {"anisoAxis", "Strands run", "aniso", 'e', 0, 2, 1, "Along the Flow map where it is painted; along the UV's V (the hair cards' usual) or U elsewhere"},
    {"anisoColor", "Colour", "aniso", 'c', 0, 0, 0, ""},
    {"anisoStrength", "Strength", "aniso", 'f', 0, 2, 0.6, ""},
    {"anisoWidth", "Sharpness", "aniso", 'f', 1, 300, 80, "Higher — a thinner band"},
    {"anisoShift", "Shift", "aniso", 'f', -1, 1, 0, "Moves the band along the strand"},
    {"aniso2", "Second band", "aniso", 'f', 0, 1, 0.3, "The second, shifted band's strength"},
    {"aniso2Shift", "Second shift", "aniso", 'f', -1, 1, 0.15, ""},
    {"rimOn", "On", "rim", 'b', 0, 1, 1, ""},
    {"rimColor", "Colour", "rim", 'c', 0, 0, 0, ""},
    {"rimWidth", "Width", "rim", 'f', 0, 1, 0.3, "How far in from the edge"},
    {"rimSoft", "Softness", "rim", 'f', 0, 0.3, 0.04, ""},
    {"rimIntensity", "Strength", "rim", 'f', 0, 2, 0.35, ""},
    {"rimLitOnly", "Lit side only", "rim", 'b', 0, 1, 1, "Only where the light falls"},
    {"sdfOn", "On", "sdf", 'b', 0, 1, 0, "Usually on the face's material only; needs the head locator"},
    {"sdfPath", "Picture", "sdf", 's', 0, 0, 0, "The face shadow's signed-distance picture (its red: the light's angle where each texel turns dark, lit from the left)"},
    {"sdfSoft", "Softness", "sdf", 'f', 0, 0.2, 0.02, ""},
    {"sdfFlip", "Flip U", "sdf", 'b', 0, 1, 0, "The picture made for light from the right"},
    {"emisOn", "On", "emission", 'b', 0, 1, 1, ""},
    {"emisColor", "Colour", "emission", 'c', 0, 0, 0, ""},
    {"emisIntensity", "Strength", "emission", 'f', 0, 5, 1, ""},
    {"outlineOn", "On", "outline", 'b', 0, 1, 1, ""},
    {"outlineColor", "Colour", "outline", 'c', 0, 0, 0, ""},
    {"outlineWidth", "Width, px", "outline", 'f', 0, 12, 1.5, "On the screen; times the Outline width map"},
    {"overStrength", "Strength", "over", 'f', 0, 1, 1, "Times the Over hair map"},
    {"faceFrom", "Fades from", "over", 'f', -1, 1, 0.15, "The face turned this far from the camera (1 — straight at it, 0 — the side): nothing through the hair"},
    {"faceTo", "Full at", "over", 'f', -1, 1, 0.55, "From here on: fully through the hair"},
    {"idOn", "On", "matid", 'b', 0, 1, 0, ""},
    {"idTint1", "Red zone", "matid", 'c', 0, 0, 0, ""},
    {"idTint2", "Green zone", "matid", 'c', 0, 0, 0, ""},
    {"idTint3", "Blue zone", "matid", 'c', 0, 0, 0, ""},
    {"idTint4", "The rest", "matid", 'c', 0, 0, 0, ""},
    {"lightIntensity", "Light", "light", 'f', 0, 3, 1, ""},
    {"ambientColor", "Ambient colour", "light", 'c', 0, 0, 0, ""},
    {"ambientI", "Ambient", "light", 'f', 0, 1, 0.08, ""},
  };
  return P;
}

QColor colorDef(const char* key) {
  const QString k = QString::fromLatin1(key);
  if (k == "lineColor") return QColor(255, 138, 92);
  if (k == "shadeColor") return QColor(125, 112, 184);
  if (k == "ambientColor") return QColor(60, 66, 80);
  if (k == "outlineColor") return QColor(30, 22, 32);
  if (k == "anisoColor") return QColor(255, 250, 235);
  return QColor(255, 255, 255);
}
QStringList enumOpts(const char* key) { if (QString::fromLatin1(key) == "anisoAxis") return {"Flow map", "Along V", "Along U"}; return {}; }

QJsonObject defaults() {
  QJsonObject o;
  for (const ParamDef& d : params()) {
    if (d.kind == 'c') o[d.key] = colorDef(d.key).name(); else if (d.kind == 'b') o[d.key] = d.def > 0.5; else if (d.kind == 's') o[d.key] = QString(); else o[d.key] = d.def; }
  return o;
}
QJsonObject merged(const QJsonObject& base, const QJsonObject& over) { QJsonObject o = defaults(); for (auto it = base.begin(); it != base.end(); ++it) o[it.key()] = it.value(); for (auto it = over.begin(); it != over.end(); ++it) o[it.key()] = it.value(); return o; }
double num(const QJsonObject& p, const char* key) { const QJsonValue v = p.value(key); if (v.isDouble()) return v.toDouble(); for (const ParamDef& d : params()) if (!qstrcmp(d.key, key)) return d.def; return 0; }
QColor col(const QJsonObject& p, const char* key) { const QColor c(p.value(key).toString()); return c.isValid() ? c : colorDef(key); }
bool flag(const QJsonObject& p, const char* key) { const QJsonValue v = p.value(key); if (v.isBool()) return v.toBool(); for (const ParamDef& d : params()) if (!qstrcmp(d.key, key)) return d.def > 0.5; return false; }
QString str(const QJsonObject& p, const char* key) { return p.value(key).toString(); }

void Head::axes(double f[3], double u[3], double r[3]) const {
  const double cy = std::cos(yaw * M_PI / 180), sy = std::sin(yaw * M_PI / 180), cp = std::cos(pitch * M_PI / 180), sp = std::sin(pitch * M_PI / 180);
  f[0] = sy * cp; f[1] = sp; f[2] = cy * cp;
  u[0] = -sy * sp; u[1] = cp; u[2] = -cy * sp;
  r[0] = u[1] * f[2] - u[2] * f[1]; r[1] = u[2] * f[0] - u[0] * f[2]; r[2] = u[0] * f[1] - u[1] * f[0];   // (up × forward: the head's own right — the viewer's left when it faces them)
}

Lib Lib::fromJson(const QJsonObject& o) {
  Lib L;
  for (const QJsonValue& v : o.value("materials").toArray()) { const QJsonObject m = v.toObject(); L.mats.push_back({m.value("id").toString(), m.value("name").toString(), m.value("params").toObject()}); }
  for (const QJsonValue& v : o.value("instances").toArray()) { const QJsonObject m = v.toObject(); L.insts.push_back({m.value("id").toString(), m.value("name").toString(), m.value("material").toString(), m.value("over").toObject()}); }
  L.assign = o.value("assign").toObject(); L.roles = o.value("roles").toObject(); L.def = o.value("default").toString();
  const QJsonObject h = o.value("head").toObject(); L.head.on = h.value("on").toBool(); const QJsonArray p = h.value("pos").toArray();
  for (int i = 0; i < 3 && i < p.size(); i++) L.head.pos[i] = p[i].toDouble();
  L.head.yaw = h.value("yaw").toDouble(); L.head.pitch = h.value("pitch").toDouble(); L.head.size = h.value("size").toDouble(20);
  return L;
}
QJsonObject Lib::toJson() const {
  QJsonArray ms, is; for (const Material& m : mats) ms.append(QJsonObject{{"id", m.id}, {"name", m.name}, {"params", m.params}});
  for (const Instance& i : insts) is.append(QJsonObject{{"id", i.id}, {"name", i.name}, {"material", i.material}, {"over", i.over}});
  const QJsonObject h{{"on", head.on}, {"pos", QJsonArray{head.pos[0], head.pos[1], head.pos[2]}}, {"yaw", head.yaw}, {"pitch", head.pitch}, {"size", head.size}};
  return QJsonObject{{"materials", ms}, {"instances", is}, {"assign", assign}, {"roles", roles}, {"default", def}, {"head", h}};
}
void Lib::ensureDefault() {
  if (mats.empty()) mats.push_back({"m1", "Toon", defaults()});
  if (insts.empty()) insts.push_back({"i1", "Toon", mats.front().id, {}});
  if (!inst(def)) def = insts.front().id;
}
const Material* Lib::mat(const QString& id) const { for (const Material& m : mats) if (m.id == id) return &m; return nullptr; }
Material* Lib::mat(const QString& id) { for (Material& m : mats) if (m.id == id) return &m; return nullptr; }
const Instance* Lib::inst(const QString& id) const { for (const Instance& i : insts) if (i.id == id) return &i; return nullptr; }
Instance* Lib::inst(const QString& id) { for (Instance& i : insts) if (i.id == id) return &i; return nullptr; }
QString Lib::instFor(int obj, const QString& set) const {
  for (const QString& k : {QString("%1:%2").arg(obj).arg(set), QString::number(obj)}) { const QString v = assign.value(k).toString(); if (inst(v)) return v; }
  return inst(def) ? def : (insts.empty() ? QString() : insts.front().id);
}
QString Lib::roleFor(int obj, const QString& set) const { return roles.value(QString("%1:%2").arg(obj).arg(set)).toString(); }
QJsonObject Lib::paramsOf(const QString& instId) const {
  const Instance* i = inst(instId); if (!i) return defaults(); const Material* m = mat(i->material);
  return merged(m ? m->params : QJsonObject(), i->over);
}
QString Lib::newId(const QString& prefix) const {
  QSet<QString> used; for (const Material& m : mats) used.insert(m.id); for (const Instance& i : insts) used.insert(i.id);
  for (int n = 1;; n++) { const QString id = prefix + QString::number(n); if (!used.contains(id)) return id; }
}

std::array<float, UboVec4 * 4> ubo(const QJsonObject& p, const std::array<bool, SlotCount>& has, const Head& head) {
  std::array<float, UboVec4 * 4> u{}; int k = 0;
  auto v4 = [&](float a, float b, float c, float d) { u[size_t(k)] = a; u[size_t(k) + 1] = b; u[size_t(k) + 2] = c; u[size_t(k) + 3] = d; k += 4; };
  auto lin = [](double c) { return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)); };   // (colours given in sRGB, worked in linear as the textures are read)
  auto c3 = [&](const char* key, float w) { const QColor c = col(p, key); v4(lin(c.redF()), lin(c.greenF()), lin(c.blueF()), w); };
  auto f = [&](const char* key) { return float(num(p, key)); };
  auto b = [&](const char* key) { return flag(p, key) ? 1.f : 0.f; };
  double F[3], U[3], R[3]; head.axes(F, U, R);
  c3("baseTint", 0);                                                              // 0
  v4(f("terminator"), f("shadow"), f("soft"), 0);                               // 1
  c3("lineColor", f("lineIntensity"));                                           // 2
  v4(f("lineWidth"), b("lineOn"), 0, 0);                                         // 3
  c3("shadeColor", f("penumbraMix"));                                            // 4
  v4(f("shadeHue"), f("shadeSat"), f("shadeVal"), f("maskShift"));              // 5
  c3("specColor", f("specIntensity"));                                           // 6
  v4(f("specSize"), f("specSoft"), b("specOn"), f("shadeTint"));                // 7
  c3("rimColor", f("rimIntensity"));                                             // 8
  v4(f("rimWidth"), f("rimSoft"), b("rimOn"), b("rimLitOnly"));                 // 9
  c3("emisColor", f("emisIntensity"));                                           // 10
  v4(b("emisOn"), b("normalOn"), f("normalStrength"), 0);                       // 11
  v4(f("lightIntensity"), f("ambientI"), 0, 0);                                  // 12
  c3("ambientColor", 0);                                                         // 13
  v4(has[SBase], has[SShade], has[SShadowMask], has[SSpec]);                     // 14
  v4(has[SRim], has[SEmission], has[SNormal], has[SFlow]);                       // 15
  v4(has[SSdf], has[SOutline], has[SOverHair], has[SMatId]);                     // 16
  c3("outlineColor", f("outlineWidth"));                                         // 17
  v4(b("outlineOn"), 0, 0, 0);                                                   // 18
  c3("anisoColor", f("anisoStrength"));                                          // 19
  v4(b("anisoOn"), f("anisoWidth"), f("anisoShift"), f("aniso2"));              // 20
  v4(f("anisoAxis"), f("aniso2Shift"), 0, 0);                                    // 21
  v4(float(F[0]), float(F[1]), float(F[2]), head.on ? 1.f : 0.f);               // 22
  v4(float(R[0]), float(R[1]), float(R[2]), 0);                                  // 23
  v4(float(U[0]), float(U[1]), float(U[2]), 0);                                  // 24
  v4(b("sdfOn"), f("sdfSoft"), b("sdfFlip"), 0);                                 // 25
  v4(f("overStrength"), f("faceFrom"), f("faceTo"), 0);                          // 26
  v4(b("idOn"), 0, 0, 0);                                                        // 27
  c3("idTint1", 0); c3("idTint2", 0); c3("idTint3", 0); c3("idTint4", 0);       // 28 … 31
  return u;
}

}  // namespace hm::toon
