// The Paint room's manager window, its model mode (as the author's Visible Model plugin for 3D-Coat, made better): the whole model in a
// small view of its own — nothing is painted there, it is for choosing and hiding parts. The polygons are drawn by the processor into a
// picture of their own (the viewport's frame is not touched while a stroke is laid), with the painted map's colour on each; the hidden
// ones stay as faint ghosts that can be picked to bring them back. The selection tools (a polygon, Loop / Ring across the strand, Fill,
// the part, a rectangle, a lasso) pick into the room's own polygon selection — the same one the Select tool and H use. Its own camera:
// LMB on the empty ground / RMB — orbit, MMB — pan, the wheel — zoom, F — the shown parts framed; it may follow the main view's angle.
// The room's visibility and selection have an undo history of their own here (VisHistory): Ctrl+Z over the manager window takes back
// the last hiding — wherever it was done — not the last stroke.
#pragma once
#include "view/Camera.h"
#include <QElapsedTimer>
#include <QImage>
#include <QPolygonF>
#include <QWidget>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

class QTimer;

namespace hm {

class Viewport;
class Scene;
struct PolyMesh;

// the room's visibility, locks and polygon selection: every change (made anywhere — the preview, H over the model, the lists' eyes, a
// set) one step; a gesture held with a mouse button is one step when it ends. Ctrl+Z over the manager window — back a step
class VisHistory {
public:
  explicit VisHistory(Viewport* vp);
  void changed();                       // (the viewport's paintVisChanged)
  bool undo(); bool redo();             // (false: nothing there)
  void begin() { batch_++; } void end();   // (a change made of several: one step)
  int undoCount() const { return int(undo_.size()) + (pending_ ? 1 : 0); }
  int redoCount() const { return int(redo_.size()); }
  void reset();                         // (another project: the history starts anew)
private:
  struct State; Viewport* vp_; std::vector<std::shared_ptr<State>> undo_, redo_; std::shared_ptr<State> last_, before_;
  bool pending_ = false, restoring_ = false; int batch_ = 0; QTimer* wait_ = nullptr;
  void commit();
};

class PaintPreview : public QWidget {
public:
  PaintPreview(Viewport* vp, Scene* sc, VisHistory* hist, QWidget* parent = nullptr);
  QString kind = "part";                // face | loop | ring | fill | part | rect | lasso
  bool ghosts = true, follow = false;
  void setKind(const QString& k);
  void frameShown();                    // (F: what is shown framed)
  void takeMainView();                  // (the main view's angle, this view's framing)
  bool key(const QString& op);          // H and its kin over this view: on the selected, else on the part under the pointer (a hidden one — shown again)
  bool keyAt(const QString& op, const QPointF& w);   // (the same at a widget point — checks)
  void refresh();                       // (the room's visibility / the scene changed: drawn anew)
  bool pointerOver() const;
  QString status() const;               // (checks: hidden / selected counts, the last frame's time)
  QImage grabShot();                    // (checks: the picture as it is drawn)
  bool test(int phase, const QPointF& at, Qt::KeyboardModifiers mods, Qt::MouseButton b = Qt::LeftButton);   // (checks: a mouse event at a widget point)
  std::function<void()> stateChanged;   // (the strip's tools / the counts: made anew)
protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void leaveEvent(QEvent*) override;
  void showEvent(QShowEvent*) override;
private:
  Viewport* vp_; Scene* sc_; VisHistory* hist_; Camera cam_; bool camSet_ = false;
  // the geometry, flattened: every painted mesh's base polygons (their shown vertices), one numbering for all
  struct Mesh { int id = -1, faceOff = 0, vertOff = 0, nf = 0; std::shared_ptr<const PolyMesh> B; };
  std::vector<Mesh> meshes_; QByteArray geoKey_;
  std::vector<std::array<float, 3>> pos_, scr_;      // (world, screen x / y / depth — x NaN: behind the camera)
  std::vector<int> fStart_, fVerts_, fMesh_;          // (per polygon: its corners, its mesh — meshes_ index)
  std::vector<std::array<float, 3>> fNorm_; std::vector<float> shade_;
  std::vector<char> hid_, lock_, sel_, hov_; qint64 visKey_ = -1; quint64 selSeen_ = 0;
  std::vector<QRgb> col_; QByteArray colKey_; QElapsedTimer colT_;
  // the picture: per pixel the nearest shown polygon and the nearest hidden one (the ghost)
  int W_ = 0, H_ = 0; double dpr_ = 1; std::vector<float> zv_, zg_; std::vector<int> iv_, ig_; QImage img_;
  bool rasterDirty_ = true, compDirty_ = true; double lastMs_ = 0;
  // the pointer
  QPointF hoverAt_; int hoverFace_ = -1; bool hoverGhost_ = false;
  int drag_ = 0; QPointF press_, last_; Qt::KeyboardModifiers pressMods_; int selOp_ = 0, lastPick_ = -1; bool moved_ = false;   // (drag_: 1 orbit, 2 pan, 3 the selection painted, 4 a rectangle / lasso, 5 zoom)
  QPolygonF shape_; QTimer* followT_ = nullptr; Quat followQ_;
  void ensureGeometry();
  void ensureFlags();
  void ensureColours(bool force = false);
  QByteArray colourKey() const;
  void raster();
  void compose();
  int pickAt(const QPointF& w, bool* ghost = nullptr) const;
  std::optional<Vec3> pointAt(const QPointF& w) const;          // (the surface's point under a widget point: the turn's centre)   // (a widget point → the global polygon there, −1: none)
  std::vector<int> walkAt(const QPointF& w, int g, const QString& kind) const;   // (the polygons a click takes — global numbers)
  std::map<int, std::vector<int>> toMeshes(const std::vector<int>& g) const;
  void setHover(const QPointF& w);
  Box3 shownBox(bool all) const;
  void fitCam();
  void press(const QPointF& w, Qt::MouseButton b, Qt::KeyboardModifiers mods);
  void move(const QPointF& w, Qt::MouseButtons bs, Qt::KeyboardModifiers mods);
  void release(const QPointF& w, Qt::MouseButton b);
};

}  // namespace hm
