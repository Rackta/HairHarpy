// The History slider (the Inspector's ⏱ History): the undo history as a strip along the viewport's bottom — each step a
// cell named after what it did; a click / a drag jumps to a step (the later ones stay — dimmed — until something is
// made there: then they are overwritten, said beforehand), RMB on a step — take it out (what it did undone, the later
// steps kept).
#pragma once
#include <QFrame>

class QLabel;

namespace hm {

class Viewport;

class HistoryBar : public QFrame {
  Q_OBJECT
public:
  HistoryBar(QWidget* host, Viewport* vp);
  void place();                                    // (the viewport's bottom; the host moved / resized)
  void sync();                                     // (the history changed)
  void applyStyle();                               // (the Inspector changed its look / place / the step count)
  static QSize sizeFor(QWidget* host, int height); // (its width over the viewport — the Inspector's preview too)
signals:
  void closed();
private:
  class Strip;
  QWidget* host_; Viewport* vp_;
  Strip* strip_ = nullptr; QLabel *title_ = nullptr, *now_ = nullptr, *warn_ = nullptr, *hint_ = nullptr;
};

}  // namespace hm
