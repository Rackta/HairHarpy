// An imported mesh's settings (the HTML version's imeshProps / meshXmodProps): its polygroups shown / hidden, the
// Thickness & mask modifier, the modifiers' list; a mesh modifier's settings — Transform (position, rotation °, scale,
// Reset), Thickness (amount, Clear, its brush; its mask: Clear / Fill / Invert, the Mask brush), an older Mask.
#pragma once

class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;

void imeshExtras(QVBoxLayout* v, Viewport* vp, Scene* sc, int id);
void meshXmodProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id);

}  // namespace hm
