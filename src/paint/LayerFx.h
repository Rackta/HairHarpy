// Layer effects (19.11, recording Dress_4, 93:15: the metal of the ornament got a stroke inside and an inner shadow as a Photoshop
// layer style, 3D-Coat did not read it, the layer was rasterized; the author: "живые эффекты слоя" — a stroke, an inner shadow, a
// bevel with the dark side down, a gradient on the stroke itself; the light set in 3D; on a layer and on a group; to PSD as layers).
// An effect is worked out in the texture from the source's alpha, but over the surface: two texels count as neighbours only when
// they lie side by side on the model (not across a UV seam, not into the empty UV), so no stroke runs along a seam; and "toward the
// light" is the 3D light laid onto the surface at every texel, so a turned UV island takes its light from the right side.
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <memory>
#include <vector>

namespace hm::paint {

// a UV set's surface for the effects: which texels hold surface; whether the texel on the right / below lies next to it on the model
// (link bits 1 / 2); the way toward the light in the texture's pixels (a unit vector ×127; 0, 0 — the light falls straight on it)
struct FxFrame {
  int w = 0, h = 0;
  std::vector<unsigned char> surf, link;
  std::vector<signed char> lx, ly;
  std::vector<signed char> nx, ny, nz;   // (the surface's normal ×127 — for the shine)
};
// P, N: w × h × 3 floats — the place and the normal of the surface under every texel (N = 0: no surface); light: toward the light
std::shared_ptr<FxFrame> fxFrame(int w, int h, const float* P, const float* N, const float light[3]);
// the light's direction from its two angles (degrees): azimuth round the up axis (0 — from the front), elevation over the horizon
void fxLight(double azimuth, double elevation, float out[3]);

// an effect's record: { "type": stroke | inner | bevel | shine, "src": layer id, sizes in texels, colours "#rrggbb", "az", "el" }
QJsonObject fxDefaults(const QString& type);
QString fxName(const QString& type);
// the effect's picture (ARGB32, the frame's size) from the source's coverage (Grayscale8 of that size: 255 — painted)
QImage fxRender(const QImage& alpha8, const FxFrame& F, const QJsonObject& fx);
// how strong the highlight is at every texel (Grayscale8, 255 — its peak): the light L and the view V (toward the eye), the roughness
// spreading it — the brush's "Shine" holds a stroke by it, the metal drawn where the light catches it as seen now
QImage fxShineMap(const FxFrame& F, const float L[3], const float V[3], float rough);

}  // namespace hm::paint
