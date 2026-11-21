// The material builder's graph (the Paint room): nodes joined output → input, as in the engines' material editors — the room's maps,
// constants, arithmetic, the light and the view — ending in the Output node. It is worked out per texel of the texture (the surface's
// normal and place there given by the caller) into a picture the viewport shows on the model: a toon look assembled from the painted
// maps and seen at once. Kept with the project (P.paint.mat).
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

namespace hm::paint {

struct MatNode { int id = 0; QString type; QPointF pos; QJsonObject p; };   // p: the node's own values (map, color, v, d0… — an input's value while nothing is joined to it)
struct MatLink { int from = 0, to = 0, input = 0; };

struct MatNodeType { QString key, name; QStringList inputs; std::vector<float> defaults; QString tip; };
const std::vector<MatNodeType>& matNodeTypes();
const MatNodeType* matNodeType(const QString& key);

struct MatGraph {
  std::vector<MatNode> nodes; std::vector<MatLink> links; int nextId = 1;
  MatNode* node(int id); const MatNode* node(int id) const;
  int add(const QString& type, const QPointF& pos);
  void remove(int id);                                       // (the Output node stays)
  void link(int from, int to, int input);                    // (an input takes one link: the old one goes; a loop is refused)
  void unlink(int to, int input);
  bool reaches(int from, int target) const;                  // (target is fed, through any links, by from)
  bool uses(const QString& type) const;                      // (a node of this type feeds the Output)
  QJsonObject toJson() const; void fromJson(const QJsonObject& o);
  static MatGraph toon();                                    // the starting graph: the base colour, darker where the light doesn't reach — a hard step
};

// what the graph is worked out over: the picture's size; a map of the room by its key (any size — scaled here); per texel the surface's
// unit normal and its place (3 floats each, w·h texels; a texel no polygon covers: a zero normal); the light's direction (towards it)
// and where the camera is (ortho: the direction it looks along)
struct MatContext {
  int w = 0, h = 0; std::function<QImage(const QString&)> map;
  const float* normal = nullptr; const float* pos = nullptr;
  float light[3] = {0, 0, 1}, eye[3] = {0, 0, 5}, look[3] = {0, 0, -1}; bool ortho = false;
};
QImage matEvaluate(const MatGraph& g, const MatContext& c);   // ARGB32_Premultiplied, w × h (null: nothing to show)

}  // namespace hm::paint
