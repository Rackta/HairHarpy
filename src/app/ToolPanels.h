// The Active Tool window's content for the tool in use (renderToolPanel): Brush — Screen / Surface, Radius, Hardness;
// Curve — Curve / Lock; the other tools — as they are ported. nullptr — the window is hidden (the Cursor, the gizmos).
#pragma once
#include <QWidget>
#include <functional>

class QSlider;
class QLabel;
class QLineEdit;

namespace hm {

class Viewport;
class Scene;

QWidget* buildToolPanel(Viewport* vp, Scene* sc);
void installSliderAssist();                      // every plain slider of the program: double click — back to its first value, Ctrl + drag — finely

// a labelled slider row of the panels (label, the slider, its value) — as the HTML version's .f rows. Every one: a
// double click on the slider — back to its default (the value it first had, or setReset's); the value typed in (its
// number as shown — %, mm…); Ctrl + drag on the slider or the number — finely (a tenth as fast)
class SliderRow : public QWidget {
  Q_OBJECT
public:
  SliderRow(const QString& label, double lo, double hi, double step, double value, std::function<QString(double)> fmt, QWidget* parent = nullptr);
  void setValue(double v);
  void setReset(double v);                        // double-click on the slider — back to v (changed + released)
  void setLog(bool on);                           // (the slider's way on a log scale — a value of many sizes, as a weld distance)
  void setTip(const QString& t);
  double value() const;
signals:
  void changed(double v);
  void released(double v);
private:
  QSlider* s_;
  QLineEdit* v_;
  double lo_, hi_, step_;
  std::function<QString(double)> fmt_;
  double reset_ = 0; bool hasReset_ = false, log_ = false, fine_ = false; double fineV0_ = 0, fineX0_ = 0, exact_ = 0; bool hasExact_ = false;
  int toPos(double v) const;
  double fromPos(int p) const;
  double shownK() const;                          // (the shown number to the value: 100 for %, …)
  void typed();
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
};

}  // namespace hm
