// 〰 A curve group's scheme (the Curves column): a metro map of the lock (schemeLayout / schemeBlock).
// Stations — the curves' ends (roots on the left, forks and merges inside, the tip on the right); tracks — the curves.
// Columns: how far along the lock; lanes: across it (the place round the lock against its middle line) — sorted column by
// column so that no tracks cross, then straightened. The layout is kept per group while its curves and their ends stay
// the same. Below: the group's walls and service curves as chips; ≡ List — the group as the usual list.
#pragma once
#include "core/Math.h"
#include "scene/Walls.h"
#include <QPainterPath>
#include <QString>
#include <QWidget>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace hm {

class Scene;
class Viewport;
struct Item;

struct SchemeLayout {
  struct Node { int id = -1; Vec3 p; int col = 0; double ang = 0, tseed = 0, seed = 0, k = 0, t = 0; bool hasT = false; char kind = 'n';
                std::vector<int> cs; int c = -1; int i = 0, lane = 0, pin = 0; };
  struct Chain { int c; std::vector<int> ch; };   // (indices into items, by column)
  QString key;
  int NC = 2, R = 1, crossings = 0;
  std::vector<Node> items;
  std::vector<int> nodes;                         // the stations (indices into items)
  std::vector<Chain> chains;
  QString thru;                                   // the through-wall's name (empty: none)
};

// (checks) a group's layout as text: NC R crossings thru, then each station "anchor:col:lane"
QString schemeDump(Scene& sc, const CurveCache& cc, int gid);

class SchemeView : public QWidget {
  Q_OBJECT
public:
  // (gid: the group; width — the Curves column's)
  SchemeView(Viewport* vp, Scene* sc, int gid, int width, QWidget* parent = nullptr);
  static bool hasScheme(const Scene& sc, const Item& g);   // 2+ plain curves
  std::function<void(int id)> rename;
  std::function<void()> changed;                           // (the outliner drawn anew)
  QSize sizeHint() const override;
protected:
  void paintEvent(QPaintEvent*) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void leaveEvent(QEvent*) override;
  bool event(QEvent* e) override;
private:
  struct Track { int c; std::vector<QPointF> q; };
  struct Chip { int id; QRectF r; QString text; bool wall; };
  void layoutAll();
  std::vector<QPointF> trackQ(size_t chain) const;
  QPointF nodePt(int item) const;
  std::vector<QPointF> sidePts(int curve) const;
  std::vector<int> routeIds(int curve) const;
  QPainterPath wallPath(int w, double* area) const;
  void hover(const std::set<int>& ids, int band = -1);
  int trackAt(const QPointF& p) const;
  int stationAt(const QPointF& p) const;
  int zoneAt(const QPointF& p) const;
  int chipAt(const QPointF& p) const;

  Viewport* vp_; Scene* sc_; int gid_; int W_, H_;
  const SchemeLayout* L_ = nullptr;
  std::map<int, std::pair<double, double>> sp_;        // stations moved by hand: anchor → fractions of the field
  std::map<size_t, double> lens_;                      // (two curves between the same two stations: parted in a lens)
  std::vector<Track> tracks_;
  std::vector<std::pair<int, QPainterPath>> zones_;    // (drawn big first)
  std::vector<Chip> chips_;
  std::vector<int> ws_, ecs_;
  QRectF listBtn_, resetBtn_;
  double chipsY_ = 0, h_ = 0;
  std::set<int> hov_; int band_ = -1, hovChip_ = -1;
  int dragNode_ = -1; bool moved_ = false; QPointF pressAt_;
};

}  // namespace hm
