// The Paint room's look settings, kept in the project: the lighting rig (what the viewport's preview is lit by) and the toon
// shader with its anisotropic highlight (which reads the Flow map). Plain data + JSON; the viewport reads them to light the
// mesh, the exporters write them next to the maps.
#pragma once
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <vector>

namespace hm::paint {

struct Light {
  QString name = "Key";
  bool on = true;
  float azimuth = 40, elevation = 35;      // degrees, from the viewer's forward; 0 azimuth = straight on
  QColor color = Qt::white;
  float intensity = 1;
  bool follow = true;                      // follows the camera (else fixed in the world)
};

struct LightRig {
  std::vector<Light> lights;
  QColor ambient = QColor(60, 66, 80);
  float ambientIntensity = 0.6f;
  QString environment;                     // an HDRI's path (empty — the plain sky)
  float envIntensity = 1, envRotation = 0;
  bool shadows = false;

  static LightRig studio() {
    LightRig r; Light k; Light f; f.name = "Fill"; f.azimuth = -60; f.elevation = 10; f.intensity = 0.4f; f.color = QColor(190, 210, 255);
    Light b; b.name = "Rim"; b.azimuth = 160; b.elevation = 30; b.intensity = 0.8f; r.lights = {k, f, b}; return r;
  }
  QJsonObject toJson() const {
    QJsonObject o; QJsonArray a;
    for (const Light& l : lights) { QJsonObject j; j["name"] = l.name; j["on"] = l.on; j["az"] = l.azimuth; j["el"] = l.elevation; j["color"] = l.color.name(); j["i"] = l.intensity; j["follow"] = l.follow; a.push_back(j); }
    o["lights"] = a; o["ambient"] = ambient.name(); o["ambientI"] = ambientIntensity; o["env"] = environment; o["envI"] = envIntensity; o["envRot"] = envRotation; o["shadows"] = shadows; return o;
  }
  static LightRig fromJson(const QJsonObject& o) {
    LightRig r; if (!o.contains("lights")) return studio();
    for (const QJsonValue& v : o.value("lights").toArray()) { const QJsonObject j = v.toObject(); Light l; l.name = j.value("name").toString(); l.on = j.value("on").toBool(true);
      l.azimuth = float(j.value("az").toDouble()); l.elevation = float(j.value("el").toDouble()); l.color = QColor(j.value("color").toString("#ffffff")); l.intensity = float(j.value("i").toDouble(1)); l.follow = j.value("follow").toBool(true); r.lights.push_back(l); }
    r.ambient = QColor(o.value("ambient").toString("#3c4250")); r.ambientIntensity = float(o.value("ambientI").toDouble(0.6)); r.environment = o.value("env").toString();
    r.envIntensity = float(o.value("envI").toDouble(1)); r.envRotation = float(o.value("envRot").toDouble()); r.shadows = o.value("shadows").toBool(); return r;
  }
};

enum class FlowSource { None, Painted, Hair };   // Painted: the Flow layers' map; Hair: along the hair strands' UV direction (the hair meshes)

struct ToonSettings {
  bool on = false;
  int bands = 3;                           // the light's steps (2…8)
  float threshold = 0.5f;                  // where the light turns to shadow, 0…1
  float softness = 0.03f;                  // the step's width
  QColor shadow = QColor(120, 110, 150);   // the shadow band's tint
  float shadowMix = 0.55f;
  float rim = 0.25f, rimWidth = 0.35f;     // a rim light
  float outline = 0;                       // px; 0 — none
  QColor outlineColor = QColor(25, 20, 30);
  // the anisotropic highlight (hair / satin): a band across the flow direction
  bool aniso = false;
  FlowSource flow = FlowSource::Painted;
  float anisoStrength = 0.6f, anisoWidth = 0.25f, anisoShift = 0.0f, anisoSecondary = 0.35f;   // (the second band, shifted: the hair's two highlights)
  QColor anisoColor = Qt::white, anisoColor2 = QColor(255, 220, 200);
  bool anisoSteps = true;                  // the highlight cut to hard bands like the light

  QJsonObject toJson() const {
    QJsonObject o; o["on"] = on; o["bands"] = bands; o["thr"] = threshold; o["soft"] = softness; o["shadow"] = shadow.name(); o["shadowMix"] = shadowMix; o["rim"] = rim; o["rimW"] = rimWidth;
    o["outline"] = outline; o["outlineC"] = outlineColor.name(); o["aniso"] = aniso; o["flow"] = int(flow); o["aStr"] = anisoStrength; o["aW"] = anisoWidth; o["aShift"] = anisoShift; o["aSec"] = anisoSecondary;
    o["aC"] = anisoColor.name(); o["aC2"] = anisoColor2.name(); o["aSteps"] = anisoSteps; return o;
  }
  static ToonSettings fromJson(const QJsonObject& o) {
    ToonSettings t; t.on = o.value("on").toBool(); t.bands = o.value("bands").toInt(3); t.threshold = float(o.value("thr").toDouble(0.5)); t.softness = float(o.value("soft").toDouble(0.03));
    t.shadow = QColor(o.value("shadow").toString("#786e96")); t.shadowMix = float(o.value("shadowMix").toDouble(0.55)); t.rim = float(o.value("rim").toDouble(0.25)); t.rimWidth = float(o.value("rimW").toDouble(0.35));
    t.outline = float(o.value("outline").toDouble()); t.outlineColor = QColor(o.value("outlineC").toString("#19141e")); t.aniso = o.value("aniso").toBool(); t.flow = FlowSource(o.value("flow").toInt(1));
    t.anisoStrength = float(o.value("aStr").toDouble(0.6)); t.anisoWidth = float(o.value("aW").toDouble(0.25)); t.anisoShift = float(o.value("aShift").toDouble()); t.anisoSecondary = float(o.value("aSec").toDouble(0.35));
    t.anisoColor = QColor(o.value("aC").toString("#ffffff")); t.anisoColor2 = QColor(o.value("aC2").toString("#ffdcc8")); t.anisoSteps = o.value("aSteps").toBool(true); return t;
  }
};

}  // namespace hm::paint
