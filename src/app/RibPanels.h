// ▬ Flat locks' settings (the HTML version's ribAnchorProps / ribCurveProps): the selected anchors' width and rotation;
// the lock's profile smoothing, tip, the Extrude / Offset / Glue brushes, Convert, back to a plain curve.
#pragma once

class QVBoxLayout;
class QString;

namespace hm {

class Viewport;
class Scene;

// a lock started between anchors: where it leaves its root — "along" the curves it starts from (as made), "flip" the other way,
// "free" (no handle of its own: toward its next point)
void setRibRootDir(Viewport* vp, int c, const QString& mode);
void setRibDetached(Viewport* vp, int c, bool on);   // (the lock into a group of its own / back)

void ribAnchorProps(QVBoxLayout* v, Viewport* vp, Scene* sc);   // (nothing when no selected anchor is on a flat lock)
void ribCurveProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int c);

}  // namespace hm
