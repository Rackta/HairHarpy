// The bridge from ZBrush (its Python plugin, resources/bridge/hairharpy_zbrush.py): SubTools sent from ZBrush come into the
// open scene. While "Receive from ZBrush" is on, the program keeps %LOCALAPPDATA%\HairHarpy\ZBrush\running_<pid>.pid (the
// plugin's sign that it is open) and watches %LOCALAPPDATA%\HairHarpy\ZBrush\inbox: each OBJ the plugin puts there is
// imported (a SubTool sent again — its mesh replaced in place) and the file deleted. Off — nothing is watched or written.
// Install: the plugin copied into %LOCALAPPDATA%\HairHarpy\ZBrush\plugin and that folder added to ZBRUSH_PLUGIN_PATH in the
// user's environment (ZBrush 2026+ runs every .py there at start).
#pragma once
#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

class QWidget;

namespace hm {

class Viewport;
class Scene;

class ZBrushLink : public QObject {
  Q_OBJECT
public:
  ZBrushLink(QWidget* window, Viewport* vp, Scene* sc);
  ~ZBrushLink() override;
  static ZBrushLink* get();                        // (the program's one; nullptr — not made yet)
  bool receiving() const { return on_; }
  void setReceiving(bool on);                      // (kept in the settings)
  static QString root();                           // %LOCALAPPDATA%\HairHarpy\ZBrush
  static QString pluginDir();
  bool pluginInstalled() const;                    // (the file in place and its folder in ZBRUSH_PLUGIN_PATH)
  QString installPlugin();                         // → the message ('!…' — failed)
  void checkNow();                                 // (the inbox looked through now)
private:
  void take();
  QWidget* win_; Viewport* vp_; Scene* sc_;
  QFileSystemWatcher watch_; QTimer debounce_; bool on_ = false, busy_ = false;
  QString pidFile_;
};

}  // namespace hm
