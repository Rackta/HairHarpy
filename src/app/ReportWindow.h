// The Inspector's Report (Check connections, Convert with Weld locks): in the bottom right corner over the viewport, no
// backdrop — the scene stays usable. Mara, the text, the places (click — the place highlighted), the buttons:
// Auto-fix (its review — blue / orange pins, Accept all / Reject all), Check again, OK; nothing found — "no faults"
// with Convert / Close.
#pragma once
#include "scene/Convert.h"
#include <QFrame>
#include <vector>

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace hm {

class Viewport;
class Scene;

class ReportWindow : public QFrame {
  Q_OBJECT
public:
  ReportWindow(QWidget* host, Viewport* vp, Scene* sc);
  void showProblems(const std::vector<WeldProblem>& problems, bool report);   // (Viewport::weldIssues)
  void runAutoFix();
  void place();                                    // (the host moved / resized)
  void dismiss();                                  // OK / Close
signals:
  void convertRequested();
private:
  void setMara(const QString& img);
  QWidget* host_;
  Viewport* vp_;
  Scene* sc_;
  QLabel *mara_, *title_, *text_;
  QListWidget* list_;
  QProgressBar* prog_;
  QPushButton *again_, *convert_, *ok_, *fix_, *accAll_, *rejAll_;
  std::vector<WeldProblem> places_;
  bool afterFix_ = false;
};

}  // namespace hm
