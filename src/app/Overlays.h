// The viewport's info windows (the frame counter, the model's numbers): whether each is on (the numbers — in a row or
// a column), how it looks (the background's colour and opacity, the font's size, weight and opacity) and where it sits
// in the viewport (dragged there — a fraction of the free room, −1: its corner). Kept between runs; the Inspector edits
// them, the overlay reads them.
#pragma once
#include <QColor>
#include <QPointF>

namespace hm::overlays {

struct Style { QColor bg = QColor(0, 0, 0); double bgOp = 0.43; double size = 8.5; int weight = 400; double textOp = 1; };
struct Cfg {
  bool fps = true;
  int stats = 2;                                 // 0 — off, 1 — in a row, 2 — in a column
  Style fpsS, statsS;                            // (statsS: in the Hair room — what Convert would make of the selected cocoons)
  Style histS{QColor(0x11, 0x17, 0x25), 1.0, 9, 400, 1};   // the History slider's look (shown / hidden — the Inspector's button)
  bool histSteps = true;                         // the History slider: "Step 3 of 12" before the step's name
  int histShown = 40;                            // the History slider: how many steps its strip shows (round the current one)
  QPointF fpsPos{-1, -1}, statsPos{-1, -1}, histPos{-1, -1};
  QPointF keyPos{-1, -1}, busyPos{-1, -1};      // the key display; the busy curl (saving, a computation) — placed only in the Inspector
};
Cfg& cfg();
void save();                                     // (kept, the revision bumped)
int revision();

}  // namespace hm::overlays

#include <QPainter>
#include <QStringList>

namespace hm::overlays {
// the cards as the viewport shows them (the Inspector's preview draws the same)
QFont fontOf(const Style& s);
QSize fpsSize(const Style& s, const QString& text);
void paintFps(QPainter& p, const QRect& r, const Style& s, const QString& text, QColor col);
QSize statsSize(const Style& s, const QStringList& rows, bool inRow);   // rows: "label\tvalue", the first one the title
void paintStats(QPainter& p, const QRect& r, const Style& s, const QStringList& rows, bool inRow);
}  // namespace hm::overlays
