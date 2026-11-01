// ▦ The texture window (the HTML version's renderHeadTex): floats over the scene and shows the textures of the selected
// models — the texture folder on the left (thumbnails: click — into the chosen map, or drag one onto a map), the sets on
// the right (on / off, colour, the colour map's alpha, maps: load / replace / remove / the file's own), 💾 the linked images
// into the project, ⇉ the first model's textures onto the other selected ones. Every change — one undo step.
#pragma once
#include <QJsonObject>
#include <functional>
#include <vector>

class QWidget;

namespace hm {

class Viewport;
class Scene;

// a change of looks from the UI: each model's look made to fit its sets first, then fn; one undo step
void lookChanged(Viewport* vp, const std::vector<int>& ids, const std::function<void(QJsonObject& look)>& fn);
// the window opened on a model (opened on a selected one — every selected model with UVs)
void openTexWindow(QWidget* parent, Viewport* vp, Scene* sc, int id, int mat = -2);   // (mat: only that UV Mat's set)
// the head's models with textures (the Head row's ▦ / ◐)
std::vector<int> headLookTargets(const Scene& sc);

}  // namespace hm
