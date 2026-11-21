// References (the HTML version's References part): reference PLANES — an image on a plane in a view, behind the model
// (or over it), moved / turned / scaled by the gizmo while it is the active one in the 🖼 menu — and the reference BOARD
// (its items kept here as the file has them; the board window draws and edits them). Saved in the project's refs; the
// planes are in the undo history, the board isn't.
#pragma once
#include "core/Math.h"
#include <QJsonArray>
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <array>
#include <optional>
#include <vector>

namespace hm {

class Camera;
class Scene;

// a 4×4 transform in three.js's order (column-major: elements 0–2 the X column, 12–14 the translation)
using M4 = std::array<double, 16>;
M4 m4Identity();
M4 m4Compose(const Vec3& t, const Quat& q, const Vec3& s);
void m4Decompose(const M4& m, Vec3& t, Quat& q, Vec3& s);
M4 m4Mul(const M4& a, const M4& b);
M4 m4Inverse(const M4& m);
Vec3 m4Apply(const M4& m, const Vec3& p);
M4 m4FromAffine(const Affine& a);
Affine m4ToAffine(const M4& m);

struct RefPlane {
  int id = 0;
  QString view;                      // front / back / left / right / top / ortho (follows the free Orthographic camera)
  QString name, img;                 // img — a data: URL (empty — none yet)
  double aspect = 1, opacity = 0.6;
  bool visible = true, onlyView = true, over = true;
  double piv[2] = {0, 0};            // the gizmo's pivot: a point of the image (the plane's own units, the centre 0, 0)
  Vec3 pos; Quat quat; Vec3 scl{1, 1, 1};
  // an ortho plane: its place in the camera's frame (loc); 📌 locked to the model — the frame turns at a fixed point
  bool hasLoc = false; M4 loc = m4Identity();
  int canvas = -1;                   // the canvas (a smart object in Scene::refSketches) it shows; several planes may share one (-1 — none)
  bool sheet = false;                // a screen-sized canvas sheet: no grey placeholder, Opacity applies to its drawing (older planes: an image with a drawing over it)
  QJsonValue lock;                   // { anchor [x,y,z], view { pos, quat, zoom, pivot } } or null
  bool follows() const { return view == "ortho"; }
  M4 matrix() const { return m4Compose(pos, quat, scl); }
};

// a canvas, a smart object of the project (the ✏ canvas — the Paint room's layers and brushes): every plane bound to it shows it, a change reaches them all; kept in the project, NOT in the undo
// history (the canvas has its own Ctrl+Z); data — the layer document with its pixels (Document::packJson), flat — the
// composed picture the viewport shows over the plane's image, rev — changed with every stroke
struct RefSketch { QJsonObject data; QImage flat; qint64 rev = 0; QString name; };
// a marker: a point in the world seen on every view — each reference canvas (projected onto its plane, with a guide line at its
// height) and every pane of the viewport; put and moved on a canvas (only the canvas's two directions change, its depth stays)
struct RefMarker { int id = 0; Vec3 pos; QColor color; };

namespace refs {

extern const char* VIEWS[6];         // front back left right top ortho
QString viewLabel(const QString& v);
QString opposite(const QString& v);  // front ↔ back, left ↔ right ("" — none)

// the frame an ortho plane is kept in: the camera (free), or its turn at the lock's anchor (locked)
M4 frame(const RefPlane& rp, const Camera& cam);
void apply(RefPlane& rp, const Camera& cam);           // an ortho plane in front of this camera (pos / quat / scl from loc)
void capture(RefPlane& rp, const Camera& cam);         // its place (moved in the world) back into the camera's frame
void setLock(RefPlane& rp, const Camera& cam, bool on);
Box3 modelBox(const Scene& sc, const Camera& cam);     // the head's box, else a cube round the scene's centre
void place(RefPlane& rp, const Scene& sc, const Camera& cam);   // behind the model, centred, as high as it (top: as deep)
void centre(RefPlane& rp, const Scene& sc, const Camera& cam);  // the pivot onto the view's centre
Vec3 pivotWorld(const RefPlane& rp);
// shown by this camera: visible, and — "only in its view" — the camera in that view; the free Orthographic camera turned to
// that view (within some 8°) shows the side views' planes too
bool shownIn(const RefPlane& rp, const Camera& cam);
RefPlane copyTo(const RefPlane& src, const QString& view, const Scene& sc, const Camera& cam, int newId, const QString& name);   // (the same canvas: shared; copyCanvas makes it a separate one)
// canvases: a new one (W×H px, one clear layer), an independent copy of one (all its layers), how many planes show one
int newCanvas(Scene& sc, int W, int H, const QString& name);
int copyCanvas(Scene& sc, int id);
int canvasUsers(const Scene& sc, int id);
// a sheet in its view at the camera's middle, one canvas pixel to one pixel of the screen at this zoom
void placeSheet(RefPlane& rp, const Scene& sc, const Camera& cam, int W, int H);

QColor markerColor(int n);            // the n-th marker's colour (a set of 8 that stand apart)
QJsonObject markerJson(const RefMarker& m);
RefMarker markerFromJson(const QJsonObject& o, int id);

RefPlane fromJson(const QJsonObject& o, int id);
QJsonObject toJson(const RefPlane& rp);

}  // namespace refs

}  // namespace hm
