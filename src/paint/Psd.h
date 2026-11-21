// A layered Photoshop file (PSD, 8 bits a channel, RGB): the Paint room's layers written as Photoshop's own — their names, blend
// modes, opacity and visibility — and read back. Each layer is cropped to what it holds and packed (PackBits), as Photoshop writes
// them. Read: plain pixel layers of an 8-bit RGB file, raw or PackBits; groups' markers and adjustment layers are passed over.
#pragma once
#include "paint/PaintDoc.h"
#include <QImage>
#include <QString>
#include <vector>

namespace hm::paint {

struct PsdLayer {
  QString name;
  QImage image;               // ARGB32, the document's size (written: cropped to its paint)
  Blend blend = Blend::Normal;
  float opacity = 1;
  bool visible = true;
  bool pixels = true;         // (read: false — a layer with no picture of its own: an adjustment, a group's marker)
};

// layers: bottom → top. merged: the picture shown when the file is opened by a program that reads no layers (may be null: transparent)
// alphas: extra alpha channels of the document (Photoshop's Channels panel — Ctrl+click loads one as a selection), each a gray
// picture of the document's size with its name
struct PsdAlpha { QString name; QImage gray; };
bool writePsd(const QString& path, int w, int h, const std::vector<PsdLayer>& layers, const QImage& merged, QString* error = nullptr, const std::vector<PsdAlpha>& alphas = {});
bool readPsd(const QString& path, int* w, int* h, std::vector<PsdLayer>* layers, QString* error = nullptr);

}  // namespace hm::paint
