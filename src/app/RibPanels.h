// ▬ Flat locks' settings (the HTML version's ribAnchorProps / ribCurveProps): the selected anchors' width and rotation;
// the lock's profile smoothing, tip, the Extrude / Offset / Glue brushes, Convert, back to a plain curve.
#pragma once

class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;

void ribAnchorProps(QVBoxLayout* v, Viewport* vp, Scene* sc);   // (nothing when no selected anchor is on a flat lock)
void ribCurveProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int c);

}  // namespace hm
