#pragma once
// The session recording (HM_DEVTOOLS — on in every build, the published one too): a session's record for the analysis, a JSON-lines
// file in the session's folder — and annotations (the camera, the project's state, a note, its tags and a drawing over the window's
// picture) beside it. Recorded only while the recording is on (● Rec, or the "auto" of the recorder's settings file):
//   what was done      — actions, keys (and how long a key was held), buttons, menu items, lists, sliders, the tools, modes, selections;
//   where and how      — every press and release of the mouse with its place in the window, the part of the window it fell on, how
//                        long the gesture took and how far it went; the pointer's path (sampled), the pen's pressure;
//   the state          — the room, the tool, the room's own values (the Paint room: the tool, the drawing mode, the layer, the UV set,
//                        the brush) — a line whenever one changes;
//   how long it took   — the timed operations of the program (a stroke, an undo, a save, a room entered…), the frames drawn each second
//                        and the slowest of them, the window's stalls — with where the program stood at that moment (the call stack);
//   what was seen      — pictures of the window every few seconds of work (only if the settings file asks for them).
// The settings file — <Documents>/HairHarpy Sessions/recorder.json, none by default:
//   {"root": "D:/HairHarpy Sessions", "auto": true, "frames": true, "frameEvery": 2, "frameWidth": 1920, "pointer": true, "stacks": true}
// root: where the sessions go; auto: every run of the program recorded from its start. Nothing is sent anywhere.
#ifdef HM_DEVTOOLS
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

class QTimer;
class QWidget;

namespace hm {
class Viewport; class Scene;

class DevSession : public QObject {
public:
  // grabWindow: the whole window's picture (the viewport's frame in its place)
  DevSession(QWidget* window, Viewport* vp, Scene* sc, std::function<QImage()> grabWindow, std::function<QString()> projectName);
  ~DevSession() override;
  bool recording() const { return on_; }
  bool autoStart() const { return cfg_.autoRec; }       // (the settings file asks for every run to be recorded)
  void start();
  void stop(bool quiet = false);                         // (→ the session's folder, shown in a message and copied; quiet: neither — the program is closing)
  void event(const QString& kind, const QJsonObject& data = {});
  void annotate();                                       // (the annotation window: a note, its tags, a drawing over the picture)
  QString folder() const { return dir_; }
  std::function<void()> changed;                         // (recording started / stopped: the buttons follow)
  // (the room's own state — small values by name: asked a few times a second, a "state" line written when one has changed and settled)
  std::function<QJsonObject()> stateHook;
  // (what the program knows of itself, written once at the start: every action with its key — what was never used shows against it)
  std::function<QJsonObject()> catalogHook;

private:
  struct Cfg { QString root; bool autoRec = false, frames = false, pointer = true, stacks = true; double frameEvery = 2; int frameWidth = 1920; } cfg_;
  void readConfig();
  static QString defaultRoot();
  QString sessionsRoot(bool make = true) const;          // (where the sessions go; make: the folder created)
  QJsonObject state() const;                             // (the project's state now)
  bool eventFilter(QObject* o, QEvent* e) override;
  void tick();                                           // (every 25 ms: the heartbeat, the stalls; the slower rounds — the state, the second's sums, the frames)
  void pollState();
  void flushSecond();
  void maybeFrame(bool force = false);
  void ptrSample(const QPointF& g, int buttons, double pressure, bool force);
  void flushPtr();
  static QString zoneOf(QWidget* w);
  QWidget* win_; Viewport* vp_; Scene* sc_; std::function<QImage()> grab_; std::function<QString()> name_;
  QPointer<QObject> vpAlive_;                 // (the viewport may be gone before this: its hook cleared only while it lives)
  bool on_ = false; QString dir_, file_; QFile out_, outImp_; int notes_ = 0, frames_ = 0; quint64 lastKeyStamp_ = 0; QElapsedTimer clock_, tick_; QTimer* timer_ = nullptr; qint64 lastInputMs_ = 0;   // (lastInputMs_: when the last key / press came — a stall long after it: not the work of what was done)
  QString lastAction_, lastTool_, lastMode_, lastSel_;
  // the state: what was seen at the last look, what was written
  QJsonObject statePending_, stateOut_; int rounds_ = 0;
  // the gesture going (a mouse button down) and the pointer's path
  struct Gesture { bool on = false, pen = false; double pMax = 0, pSum = 0; int pN = 0; int button = 0, mods = 0, moves = 0; qint64 t0 = 0; QPointF p0, last; double dist = 0; QString window, zone; } gest_;
  QJsonArray ptr_; qint64 ptrAt_ = -1000, ptrT0_ = 0; QPointF ptrLast_; double hoverDist_ = 0;
  QHash<int, qint64> keyDown_;
  // the second's sums: the frames drawn, the timed scopes
  struct Sec { int frames = 0; double frameSum = 0, frameMax = 0; int latN = 0; double latSum = 0, latMax = 0; } sec_;   // (lat: from a move of the pressed pointer to the next frame drawn)
  struct Op { int n = 0; double total = 0, max = 0; }; std::map<QString, Op> ops_;
  qint64 secAt_ = 0, frameShotAt_ = -100000, wheelAt_ = -1000, grabMs_ = 0, quietSince_ = 0; double wheelSum_ = 0; bool wheelOpen_ = false; QString wheelWin_;
  // the stalls: the main thread's heartbeat, looked at from a thread of its own — where the program stands when the window stops
  std::atomic<qint64> beat_{0}; std::atomic<bool> watch_{false}; std::thread watcher_; std::mutex stackMx_;
  struct StackShot { qint64 t; QStringList frames; }; std::vector<StackShot> stacks_;
  void watchLoop(unsigned long mainThread);
  // what the record itself needs to be trusted: the machine's load, the disk's room, the program's own warnings
  void sysSample(); void drainWarnings();
  qint64 pendingInputAt_ = 0, diskAt_ = -100000, sysAt_ = 0; bool lowDisk_ = false; quint64 cpuIdle_ = 0, cpuAll_ = 0, cpuOwn_ = 0; QString lastWarn_; int warns_ = 0;
};
}  // namespace hm
#endif
