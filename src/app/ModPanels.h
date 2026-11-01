// 〰 Curve modifiers' panels (the HTML version's modifierPanel / extraPanel / tubeProps / xmodProps / profileEditor):
// the Modifier tool's window (Profile — presets, the profile editor, Apply; Extra — add Mask / Thickness / Twist / Noise /
// Curls, the weight brush), the modifier's settings (three sections — radius, rotation, profile; the tip; Convert, Copy /
// Paste) and an extra modifier's settings.
#pragma once
#include "scene/Tubes.h"
#include <QWidget>
#include <functional>

class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;

// Profile editor: LMB drags a point (Shift — snap to 0.05), RMB adds a point on the nearest edge, double click deletes one
// (at least 3 stay). glue: a point brought within 8 px of a neighbour sticks to it (Ctrl + drag pulls one away);
// onInsert / onDelete: the caller adds / removes the point (in several profiles at once) instead of this one alone
class ProfileEditor : public QWidget {
  Q_OBJECT
public:
  struct Opts { bool glue = false; std::function<void(int, double, std::array<double, 2>)> onInsert; std::function<void(int)> onDelete; };
  ProfileEditor(std::function<tube::Profile*()> get, std::function<void()> onEdit, std::function<void()> onDone, Opts opts = {}, QWidget* parent = nullptr);
  QSize sizeHint() const override { return {238, 188}; }
protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void leaveEvent(QEvent*) override;
private:
  std::function<tube::Profile*()> get_;
  std::function<void()> onEdit_, onDone_;
  Opts opts_;
  int dragI_ = -1, hoverI_ = -1;
  std::vector<int> group_;
  QPointF toS(const std::array<double, 2>& q) const;
  std::array<double, 2> toP(const QPointF& s) const;
  int nearest(const QPointF& s) const;
};

QWidget* buildModPanel(Viewport* vp, Scene* sc);
void tubeProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id);
void xmodProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id);
void brushFor(Viewport* vp, const QString& kind);   // the weight brush of that kind on (the Modifier tool, Extra tab)

}  // namespace hm
