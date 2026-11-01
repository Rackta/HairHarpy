#include "app/Overlays.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <algorithm>

namespace hm::overlays {

namespace {
int rev_ = 1;
QJsonObject styleJson(const Style& s) { return {{"bg", s.bg.name()}, {"bgOp", s.bgOp}, {"size", s.size}, {"weight", s.weight}, {"textOp", s.textOp}}; }
Style styleOf(const QJsonObject& o, const Style& d) {
  Style s = d; if (QColor c(o.value("bg").toString()); c.isValid()) s.bg = c;
  s.bgOp = std::clamp(o.value("bgOp").toDouble(d.bgOp), 0.0, 1.0); s.size = std::clamp(o.value("size").toDouble(d.size), 6.0, 28.0);
  s.weight = std::clamp(o.value("weight").toInt(d.weight), 100, 900); s.textOp = std::clamp(o.value("textOp").toDouble(d.textOp), 0.1, 1.0);
  return s;
}
QPointF posOf(const QJsonValue& v) { const QJsonObject o = v.toObject(); return o.isEmpty() ? QPointF(-1, -1) : QPointF(o.value("x").toDouble(-1), o.value("y").toDouble(-1)); }
}  // namespace

Cfg& cfg() {
  static Cfg c; static bool loaded = false;
  if (!loaded) {
    loaded = true; const QJsonObject o = QJsonDocument::fromJson(QSettings().value("overlays").toByteArray()).object();
    if (!o.isEmpty()) {
      c.fps = o.value("fps").toBool(true); c.stats = std::clamp(o.value("stats").toInt(2), 0, 2);
      c.fpsS = styleOf(o.value("fpsS").toObject(), c.fpsS); c.statsS = styleOf(o.value("statsS").toObject(), c.statsS);
      c.fpsPos = posOf(o.value("fpsPos")); c.statsPos = posOf(o.value("statsPos"));
      c.histS = styleOf(o.value("histS").toObject(), c.histS); c.histPos = posOf(o.value("histPos")); c.histSteps = o.value("histSteps").toBool(true); c.histShown = std::clamp(o.value("histShown").toInt(40), 5, 200);
      c.keyPos = posOf(o.value("keyPos")); c.busyPos = posOf(o.value("busyPos"));
    }
  }
  return c;
}
void save() {
  const Cfg& c = cfg(); rev_++;
  auto pos = [](const QPointF& p) { return p.x() < 0 ? QJsonValue() : QJsonValue(QJsonObject{{"x", p.x()}, {"y", p.y()}}); };
  QSettings().setValue("overlays", QJsonDocument(QJsonObject{{"fps", c.fps}, {"stats", c.stats}, {"fpsS", styleJson(c.fpsS)}, {"statsS", styleJson(c.statsS)},
                                                              {"fpsPos", pos(c.fpsPos)}, {"statsPos", pos(c.statsPos)},
                                                              {"histS", styleJson(c.histS)}, {"histPos", pos(c.histPos)}, {"histSteps", c.histSteps}, {"histShown", c.histShown}, {"keyPos", pos(c.keyPos)}, {"busyPos", pos(c.busyPos)}}).toJson(QJsonDocument::Compact));
}
int revision() { return rev_; }

}  // namespace hm::overlays

namespace hm::overlays {

QFont fontOf(const Style& s) { QFont f; f.setPointSizeF(s.size); f.setWeight(QFont::Weight(s.weight)); return f; }
namespace {
QColor withA(QColor c, double a) { c.setAlphaF(std::clamp(a, 0.0, 1.0) * c.alphaF()); return c; }
void card(QPainter& p, const QRect& r, const Style& s) { p.setPen(Qt::NoPen); p.setBrush(withA(s.bg, s.bgOp)); p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4); }
}  // namespace
QSize fpsSize(const Style& s, const QString& text) { QFontMetrics fm(fontOf(s)); return {fm.horizontalAdvance(text) + 14, fm.height() + 8}; }
void paintFps(QPainter& p, const QRect& r, const Style& s, const QString& text, QColor col) {
  p.setRenderHint(QPainter::Antialiasing); card(p, r, s); p.setFont(fontOf(s)); p.setPen(withA(col, s.textOp)); p.drawText(r, Qt::AlignCenter, text);
}
QSize statsSize(const Style& s, const QStringList& rows, bool inRow) {
  QFontMetrics fm(fontOf(s)); int w = 0, h = 0;
  if (inRow) { for (int i = 0; i < rows.size(); i++) { const auto q = rows[i].split('\t'); w += fm.horizontalAdvance(q[0]) + (q.size() > 1 ? 5 + fm.horizontalAdvance(q[1]) : 0) + (i ? 14 : 0); } return {w + 16, fm.height() + 10}; }
  for (const auto& r : rows) { const auto q = r.split('\t'); w = std::max(w, fm.horizontalAdvance(q[0]) + (q.size() > 1 ? 14 + fm.horizontalAdvance(q[1]) : 0)); }
  h = int(rows.size()) * (fm.height() + 1) + 10; return {w + 16, h};
}
void paintStats(QPainter& p, const QRect& r, const Style& s, const QStringList& rows, bool inRow) {
  p.setRenderHint(QPainter::Antialiasing); card(p, r, s); p.setFont(fontOf(s)); QFontMetrics fm(p.font());
  const QColor head = withA(QColor(0xc8, 0xc8, 0xc8), s.textOp), lab = withA(QColor(0x9a, 0x9a, 0x9a), s.textOp), val = withA(QColor(0xe8, 0xe8, 0xe8), s.textOp);
  if (inRow) {
    int x = r.left() + 8; const int y = r.top() + 5 + fm.ascent();
    for (int i = 0; i < rows.size(); i++) { const auto q = rows[i].split('\t'); if (i) x += 14;
      p.setPen(i == 0 ? head : lab); p.drawText(x, y, q[0]); x += fm.horizontalAdvance(q[0]);
      if (q.size() > 1) { x += 5; p.setPen(val); p.drawText(x, y, q[1]); x += fm.horizontalAdvance(q[1]); } }
    return;
  }
  int y = r.top() + 5 + fm.ascent();
  for (int i = 0; i < rows.size(); i++) { const auto q = rows[i].split('\t');
    p.setPen(i == 0 ? head : lab); p.drawText(r.left() + 8, y, q[0]);
    if (q.size() > 1) { p.setPen(val); p.drawText(r.right() - 8 - fm.horizontalAdvance(q[1]), y, q[1]); }
    y += fm.height() + 1; }
}

}  // namespace hm::overlays
