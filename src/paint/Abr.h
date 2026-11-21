// Photoshop brush files (.abr): versions 1 / 2 (the old computed + sampled brushes - only the sampled ones carry a tip) and 6.x
// (the "8BIM" blocks: the tips in "samp", RLE or raw, 8 or 16 bit; the names, the round "computed" brushes and the dynamics in
// the "desc" descriptor). What comes out is each brush: its tip as a Grayscale8 mask (255 = paint; null - a round one) and what
// the file says of its size, hardness, spacing, angle and dynamics (-1 / 0 where it says nothing).
#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <vector>

namespace hm::paint {

struct AbrBrush {
  QString name, id;                  // (id: the sample's own, which the descriptor names)
  QImage tip;
  float spacing = -1;                // 0...1 of the size; -1 - none given
  float diameter = -1;               // px
  float hardness = -1;               // 0...1 (a computed brush)
  float roundness = 1;               // 0...1 (a computed brush: an ellipse)
  float angle = 0;                   // degrees
  float sizeJitter = 0, angleJitter = 0, scatter = 0;   // 0...1
  bool pressureSize = false;
};

std::vector<AbrBrush> readAbr(const QByteArray& data, QString* error = nullptr);

}  // namespace hm::paint
