// The Maranspector (the button left of the FAQ): Mara (petted as in the Inspector's Report), the highlights on the
// meshes — n-gons, open edges, non-planar polygons, non-manifold edges, interior markers — and the viewport's info
// windows: the frame counter (on / off) and the model's numbers (a column / a row / off), each with its look (the
// background's colour and opacity, the font's size, weight and opacity), shown in a preview as the viewport will.
#pragma once
#include <QFrame>
#include <functional>

class QPushButton;

namespace hm {

class Viewport;

class InspectorWindow : public QFrame {
  Q_OBJECT
public:
  InspectorWindow(QWidget* host, Viewport* vp);
  void place();                                    // (the viewport's top right)
  void sync();
  std::function<void()> toggleHistory;             // (⏱ History: the History slider shown / hidden — the main window's)
  std::function<bool()> historyShown;
  std::function<void()> historyStyled;            // (its look / place / step count changed here)
  class KeyCastWindow* keyCast(bool make = true); // (⌨ the key display: made on first use)
  void showKeyCast(bool on);
protected:
  void mousePressEvent(QMouseEvent* e) override;   // (dragged by its empty parts; its place kept)
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
signals:
  void closed();
private:
  QWidget* host_;
  Viewport* vp_;
  QWidget* preview_ = nullptr;
  QPushButton* histBtn_ = nullptr;
  QPushButton* castBtn_ = nullptr; class KeyCastWindow* cast_ = nullptr;
  QPushButton *fpsBtn_ = nullptr, *statsBtn_ = nullptr;
  QWidget *topoBox_ = nullptr, *hairBox_ = nullptr;      // (the Topology room's highlights / the Hair room's checks)
  QPoint grab_; bool drag_ = false;
};

// the button: Mara's picture, animated while the pointer is on it
QPushButton* makeInspectorButton(QWidget* parent);

}  // namespace hm
