// HairHarpy Toon — the project's own toon shader, kept so it can be carried into Unreal, the website and Unity (Release V2\Shader
// Plan\Shader_Plan.md). Plain data here: the parameters by module, the materials (a full set of parameters each), the instances (a
// material and the parameters changed from it), which instance each object's UV set wears — an object and a UV set, not the set alone:
// the face and the eyes may share one texture and still want two materials — the parts' roles (hair; drawn over the hair — brows,
// lashes) and the head locator (where the head is and which way it faces: no joints in HairHarpy). Kept in the project (fileRaw "toon").
// The GPU reads the parameters packed as std140 vec4s (ubo) — shaders/toon_core.glsl's Params block, the same order.
#pragma once
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <array>
#include <vector>

namespace hm::toon {

// the maps a toon material reads (each optional: without it the module takes its plain value — ShadowMask 50 % gray, Highlight, Rim and
// Over hair everywhere, no Emission, the shade made from the base colour, the mesh's own normals, the strand along the UV)
enum Slot { SBase = 0, SShade, SShadowMask, SSpec, SRim, SEmission, SNormal, SFlow, SSdf, SOutline, SOverHair, SMatId, SlotCount };
const char* slotName(int s);                  // (the exported file's suffix: BaseColor, ShadeColor, …)
bool slotSrgb(int s);                         // (colour maps read as sRGB; masks, normals, directions as they are)

// one parameter: its key in the JSON, its label, the module it belongs to, its kind and range
// kind: 'f' float, 'c' colour (colorDef), 'b' bool, 'e' a choice (enumOpts: its labels; the value — the index), 's' a file's path
struct ParamDef { const char* key; const char* label; const char* module; char kind; double lo, hi, def; const char* tip; };
const std::vector<ParamDef>& params();
QColor colorDef(const char* key);
QStringList enumOpts(const char* key);
struct ModuleDef { const char* key; const char* name; const char* tip; };
const std::vector<ModuleDef>& modules();

// parameters: a JSON object key → value (a colour as "#rrggbb", a bool, a number, a path); missing keys — the defaults
QJsonObject defaults();
QJsonObject merged(const QJsonObject& base, const QJsonObject& over);   // (an instance's overrides over its material's)
double num(const QJsonObject& p, const char* key);
QColor col(const QJsonObject& p, const char* key);
bool flag(const QJsonObject& p, const char* key);
QString str(const QJsonObject& p, const char* key);

// the head locator: where the head is, where it faces (yaw — about Y, 0 = +Z, the front view's; pitch — up), how far it is drawn
struct Head { bool on = false; double pos[3] = {0, 0, 0}; double yaw = 0, pitch = 0, size = 20;
  void axes(double fwd[3], double up[3], double right[3]) const; };

struct Material { QString id, name; QJsonObject params; };
struct Instance { QString id, name, material; QJsonObject over; };
struct Lib {
  std::vector<Material> mats; std::vector<Instance> insts;
  QJsonObject assign;                       // "<object id>:<UV set>" or "<object id>" → instance id
  QJsonObject roles;                        // "<object id>:<UV set>" → "hair" | "over" (absent — an ordinary part)
  QString def;                              // the instance worn where nothing is assigned
  Head head;
  static Lib fromJson(const QJsonObject& o);
  QJsonObject toJson() const;
  void ensureDefault();                     // (a material "Toon" and its instance "Toon" when there is none)
  const Material* mat(const QString& id) const; Material* mat(const QString& id);
  const Instance* inst(const QString& id) const; Instance* inst(const QString& id);
  QString instFor(int obj, const QString& set) const;   // (the object's set, else the object, else the default)
  QString roleFor(int obj, const QString& set) const;   // ("hair", "over" or "")
  QJsonObject paramsOf(const QString& instId) const;    // (the material's with the instance's changes)
  QString newId(const QString& prefix) const;
};

constexpr int UboVec4 = 32;
// the std140 block: the parameters, which maps the set has (has[slot]) and the head locator's axes
std::array<float, UboVec4 * 4> ubo(const QJsonObject& p, const std::array<bool, SlotCount>& has, const Head& head);

}  // namespace hm::toon
