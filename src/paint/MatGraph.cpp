#include "paint/MatGraph.h"
#include <QColor>
#include <QJsonArray>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace hm::paint {

const std::vector<MatNodeType>& matNodeTypes() {
  static const std::vector<MatNodeType> T = {
    {"map", "Map", {}, {}, "One of the room's maps as it is painted: its colour (a gray map — its gray), its alpha"},
    {"color", "Colour", {}, {}, "A constant colour"},
    {"value", "Value", {}, {}, "A constant number"},
    {"add", "Add", {"A", "B"}, {0, 0}, "A + B"},
    {"sub", "Subtract", {"A", "B"}, {0, 0}, "A − B"},
    {"mul", "Multiply", {"A", "B"}, {1, 1}, "A × B"},
    {"mix", "Mix", {"A", "B", "T"}, {0, 1, 0.5f}, "A where T is 0, B where it is 1, between — a blend"},
    {"step", "Step", {"Edge", "X"}, {0.5f, 0}, "0 where X is under the edge, 1 where it is over: the toon shading's hard border"},
    {"smooth", "Smooth step", {"Min", "Max", "X"}, {0.4f, 0.6f, 0}, "0 under Min, 1 over Max, a smooth rise between: a soft border"},
    {"invert", "Invert", {"X"}, {0}, "1 − X"},
    {"pow", "Power", {"X", "Exp"}, {0, 2}, "X raised to Exp: a highlight made tighter"},
    {"lambert", "Light", {}, {}, "How much the surface faces the light: 0 — turned away, 1 — straight at it (the first light of Lighting & Toon)"},
    {"fresnel", "Fresnel", {}, {}, "The rim: 0 where the surface faces the camera, 1 at its edge (its own power — double click)"},
    {"output", "Output", {"Colour", "Emission", "Opacity"}, {0.5f, 0, 1}, "What the model shows: the colour, light added to it, how much of it is there"},
  };
  return T;
}
const MatNodeType* matNodeType(const QString& key) { for (const auto& t : matNodeTypes()) if (t.key == key) return &t; return nullptr; }

MatNode* MatGraph::node(int id) { for (auto& n : nodes) if (n.id == id) return &n; return nullptr; }
const MatNode* MatGraph::node(int id) const { for (const auto& n : nodes) if (n.id == id) return &n; return nullptr; }
int MatGraph::add(const QString& type, const QPointF& pos) {
  MatNode n; n.id = nextId++; n.type = type; n.pos = pos;
  if (type == "map") n.p["map"] = "color"; if (type == "color") n.p["color"] = "#c8c8c8"; if (type == "value") n.p["v"] = 0.5; if (type == "fresnel") n.p["v"] = 3.0;
  nodes.push_back(n); return n.id;
}
void MatGraph::remove(int id) {
  const MatNode* n = node(id); if (!n || n->type == "output") return;
  nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const MatNode& x) { return x.id == id; }), nodes.end());
  links.erase(std::remove_if(links.begin(), links.end(), [&](const MatLink& l) { return l.from == id || l.to == id; }), links.end());
}
bool MatGraph::reaches(int from, int target) const {
  std::set<int> seen; std::vector<int> st{target};
  while (!st.empty()) { const int x = st.back(); st.pop_back(); if (x == from) return true; if (!seen.insert(x).second) continue; for (const MatLink& l : links) if (l.to == x) st.push_back(l.from); }
  return false;
}
bool MatGraph::uses(const QString& type) const { for (const MatNode& o : nodes) if (o.type == "output") for (const MatNode& n : nodes) if (n.type == type && reaches(n.id, o.id)) return true; return false; }
void MatGraph::unlink(int to, int input) { links.erase(std::remove_if(links.begin(), links.end(), [&](const MatLink& l) { return l.to == to && l.input == input; }), links.end()); }
void MatGraph::link(int from, int to, int input) {
  if (from == to || !node(from) || !node(to) || reaches(to, from)) return;   // (a loop: the node would feed itself)
  unlink(to, input); links.push_back({from, to, input});
}
QJsonObject MatGraph::toJson() const {
  QJsonArray ns, ls; for (const MatNode& n : nodes) ns.append(QJsonObject{{"id", n.id}, {"type", n.type}, {"x", n.pos.x()}, {"y", n.pos.y()}, {"p", n.p}});
  for (const MatLink& l : links) ls.append(QJsonObject{{"from", l.from}, {"to", l.to}, {"in", l.input}});
  return QJsonObject{{"nodes", ns}, {"links", ls}, {"next", nextId}};
}
void MatGraph::fromJson(const QJsonObject& o) {
  nodes.clear(); links.clear(); nextId = 1;
  for (const QJsonValue& v : o.value("nodes").toArray()) { const QJsonObject n = v.toObject(); if (!matNodeType(n.value("type").toString())) continue;
    MatNode x; x.id = n.value("id").toInt(); x.type = n.value("type").toString(); x.pos = QPointF(n.value("x").toDouble(), n.value("y").toDouble()); x.p = n.value("p").toObject(); nodes.push_back(x); nextId = std::max(nextId, x.id + 1); }
  for (const QJsonValue& v : o.value("links").toArray()) { const QJsonObject l = v.toObject(); if (node(l.value("from").toInt()) && node(l.value("to").toInt())) links.push_back({l.value("from").toInt(), l.value("to").toInt(), l.value("in").toInt()}); }
  nextId = std::max(nextId, o.value("next").toInt(1));
  bool out = false; for (const MatNode& n : nodes) out |= n.type == "output"; if (!out) *this = toon();
}
MatGraph MatGraph::toon() {
  MatGraph g;
  const int base = g.add("map", {40, 60}), tint = g.add("color", {40, 190}), shade = g.add("mul", {250, 150}), light = g.add("lambert", {40, 300}), st = g.add("step", {250, 290}), mix = g.add("mix", {470, 170}), out = g.add("output", {690, 170});
  g.node(tint)->p["color"] = "#8f86b4"; g.node(st)->p["d0"] = 0.35;
  g.link(base, shade, 0); g.link(tint, shade, 1); g.link(light, st, 1); g.link(shade, mix, 0); g.link(base, mix, 1); g.link(st, mix, 2); g.link(mix, out, 0);
  return g;
}

// ---- worked out over the texture ----
QImage matEvaluate(const MatGraph& g, const MatContext& c) {
  const MatNode* out = nullptr; for (const MatNode& n : g.nodes) if (n.type == "output") out = &n;
  if (!out || c.w <= 0 || c.h <= 0) return {};
  const size_t N = size_t(c.w) * size_t(c.h); using Buf = std::vector<float>;
  std::map<int, Buf> memo; std::set<int> busy;
  struct In { const Buf* b = nullptr; float k = 0; float at(size_t i, int ch) const { return b ? (*b)[i * 4 + size_t(ch)] : k; } };
  std::function<const Buf&(int)> eval = [&](int id) -> const Buf& {
    auto f = memo.find(id); if (f != memo.end()) return f->second;
    Buf& o = memo[id]; o.assign(N * 4, 0.f); const MatNode* n = g.node(id); const MatNodeType* t = n ? matNodeType(n->type) : nullptr;
    if (!n || !t || !busy.insert(id).second) return o;
    std::vector<In> in(size_t(t->inputs.size()));
    for (int i = 0; i < t->inputs.size(); i++) { in[size_t(i)].k = float(n->p.value(QString("d%1").arg(i)).toDouble(t->defaults[size_t(i)]));
      for (const MatLink& l : g.links) if (l.to == id && l.input == i) { const Buf& src = eval(l.from); in[size_t(i)].b = &memo.find(l.from)->second; (void)src; } }
    const QString& ty = n->type;
    if (ty == "map") { const QImage m = c.map ? c.map(n->p.value("map").toString("color")) : QImage();
      if (!m.isNull()) { const QImage s = m.scaled(c.w, c.h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < c.h; y++) { const QRgb* p = reinterpret_cast<const QRgb*>(s.constScanLine(y)); float* q = &o[size_t(y) * size_t(c.w) * 4];
          for (int x = 0; x < c.w; x++) { q[x * 4] = qRed(p[x]) / 255.f; q[x * 4 + 1] = qGreen(p[x]) / 255.f; q[x * 4 + 2] = qBlue(p[x]) / 255.f; q[x * 4 + 3] = qAlpha(p[x]) / 255.f; } } } }
    else if (ty == "color") { const QColor col(n->p.value("color").toString("#c8c8c8")); for (size_t i = 0; i < N; i++) { o[i * 4] = float(col.redF()); o[i * 4 + 1] = float(col.greenF()); o[i * 4 + 2] = float(col.blueF()); o[i * 4 + 3] = 1; } }
    else if (ty == "value") { const float v = float(n->p.value("v").toDouble(0.5)); std::fill(o.begin(), o.end(), v); }
    else if (ty == "lambert" || ty == "fresnel") { const float pw = float(n->p.value("v").toDouble(3));
      for (size_t i = 0; i < N; i++) { const float* nn = c.normal ? c.normal + i * 3 : nullptr; float v = 0;
        if (nn && (nn[0] || nn[1] || nn[2])) {
          if (ty == "lambert") v = std::max(0.f, nn[0] * c.light[0] + nn[1] * c.light[1] + nn[2] * c.light[2]);
          else { float vx, vy, vz; if (c.ortho || !c.pos) { vx = -c.look[0]; vy = -c.look[1]; vz = -c.look[2]; } else { const float* pp = c.pos + i * 3; vx = c.eye[0] - pp[0]; vy = c.eye[1] - pp[1]; vz = c.eye[2] - pp[2]; const float l = std::sqrt(vx * vx + vy * vy + vz * vz); if (l > 0) { vx /= l; vy /= l; vz /= l; } }
            v = std::pow(std::clamp(1.f - std::abs(nn[0] * vx + nn[1] * vy + nn[2] * vz), 0.f, 1.f), std::max(0.05f, pw)); } }
        o[i * 4] = o[i * 4 + 1] = o[i * 4 + 2] = v; o[i * 4 + 3] = 1; } }
    else for (size_t i = 0; i < N; i++) for (int ch = 0; ch < 4; ch++) { float v = 0; const auto a = [&](int k) { return in[size_t(k)].at(i, ch); };
        if (ty == "add") v = a(0) + a(1); else if (ty == "sub") v = a(0) - a(1); else if (ty == "mul") v = a(0) * a(1);
        else if (ty == "mix") { const float t2 = std::clamp(in[2].at(i, ch == 3 ? 0 : ch), 0.f, 1.f); v = a(0) * (1 - t2) + a(1) * t2; }
        else if (ty == "step") v = a(1) >= a(0) ? 1.f : 0.f;
        else if (ty == "smooth") { const float lo = a(0), hi = a(1); const float t2 = hi > lo ? std::clamp((a(2) - lo) / (hi - lo), 0.f, 1.f) : (a(2) >= lo ? 1.f : 0.f); v = t2 * t2 * (3 - 2 * t2); }
        else if (ty == "invert") v = 1 - a(0); else if (ty == "pow") v = std::pow(std::max(0.f, a(0)), in[1].at(i, ch == 3 ? 0 : ch));
        else if (ty == "output") v = ch == 3 ? std::clamp(in[2].at(i, 0), 0.f, 1.f) : a(0) + in[1].at(i, ch);
        if ((ty == "add" || ty == "sub" || ty == "mul" || ty == "step" || ty == "smooth" || ty == "invert" || ty == "pow") && ch == 3 && !in[0].b) v = 1;   // (numbers alone: an opaque result)
        o[i * 4 + size_t(ch)] = v; }
    busy.erase(id); return o;
  };
  const Buf& r = eval(out->id);
  QImage img(c.w, c.h, QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < c.h; y++) { QRgb* q = reinterpret_cast<QRgb*>(img.scanLine(y)); const float* p = &r[size_t(y) * size_t(c.w) * 4];
    for (int x = 0; x < c.w; x++) { const float a = std::clamp(p[x * 4 + 3], 0.f, 1.f); auto b = [&](int ch) { return int(std::clamp(p[x * 4 + ch], 0.f, 1.f) * a * 255 + 0.5f); }; q[x] = qRgba(b(0), b(1), b(2), int(a * 255 + 0.5f)); } }
  return img;
}

}  // namespace hm::paint
