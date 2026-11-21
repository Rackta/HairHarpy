// The viewport's frame: light-grey translucent controls along its top edge, seen only near the pointer (a 100 px zone that
// fades out to its edge). Right to left: the wire-only view, the wire of the meshes, the viewport's grids (the floor), the
// world axes, and the room's own (Hair: the display strip; Topology: PDO).
#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPoint>
#include <functional>

class QWidget;

namespace hm {
class Viewport;

class ViewFrame : public QObject {
public:
  struct Hooks {
    std::function<QJsonObject()> view;                              // (the project's settings.view)
    std::function<void(const QString&, const QJsonValue&)> setView;
    std::function<void(const QPoint& anchorBottomRight)> openHairDisplay;
    std::function<void(const QPoint& anchorBottomRight)> openTopoDisplay;
    std::function<QWidget*()> strip;                                // (the Hair room's display panel: shown under the buttons)
    std::function<void()> syncStrip;                                // (its values read from the project, as it opens)
  };
  ViewFrame(QWidget* host, Viewport* vp, Hooks hooks);
  struct Impl;
private:
  Impl* d_;
};
}  // namespace hm
