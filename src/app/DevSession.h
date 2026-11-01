#pragma once
// The session recording (HM_DEVTOOLS — on in every build, the published one too): a session's record for the analysis — what was done
// (actions, tools, modes, selections, messages, the window's stalls) into a JSON-lines file, and annotations — the
// camera, the project's state, a note and a drawing over the window's picture — beside it in the session's folder.
#ifdef HM_DEVTOOLS
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>

class QTimer;

namespace hm {
class Viewport; class Scene;

class DevSession : public QObject {
public:
  // grabWindow: the whole window's picture (the viewport's frame in its place)
  DevSession(QWidget* window, Viewport* vp, Scene* sc, std::function<QImage()> grabWindow, std::function<QString()> projectName);
  bool recording() const { return on_; }
  void start();
  void stop();                                           // (→ the session's folder, shown in a message)
  void event(const QString& kind, const QJsonObject& data = {});
  void annotate();                                       // (the annotation window: a note, a drawing over the picture)
  QString folder() const { return dir_; }
  std::function<void()> changed;                         // (recording started / stopped: the buttons follow)

private:
  static QString sessionsRoot(bool make = true);         // (where the sessions go; make: the folder created)
  QJsonObject state() const;                             // (the project's state now)
  bool eventFilter(QObject* o, QEvent* e) override;     // (the buttons and the menu items clicked — recorded)
  QWidget* win_; Viewport* vp_; Scene* sc_; std::function<QImage()> grab_; std::function<QString()> name_;
  bool on_ = false; QString dir_, file_; int notes_ = 0; quint64 lastKeyStamp_ = 0; QElapsedTimer clock_, tick_; QTimer* timer_ = nullptr;
  QString lastAction_, lastTool_, lastMode_, lastSel_;
};
}  // namespace hm
#endif
