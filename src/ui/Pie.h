// The pie menu (Shift+RMB — the actions for what is selected; Space — the tool ring), as the HTML version's openPie:
// a block of items above the pointer (with a title), a block below it, or items on a ring around it (icons + labels).
// Held: releasing the button / the key over an item (or towards a ring item) runs it; otherwise it stays open for a
// click; a click elsewhere or Esc closes it. Items may have a sub-list (shown under the pointer) and a hot callback (the
// pointer on / off the item — a preview) and a dwell (held a moment → the next menu).
// Also the toast (a line at the bottom for 2.2 s) and the HUD (a line at the top of the viewport while it is set).
#pragma once
#include <QImage>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>

class QLabel;
class QTimer;

namespace hm::ui {

struct PieItem {
  QString label, icon, tip;                     // icon: an image name (ring items)
  std::function<void()> act;
  std::vector<PieItem> sub;
  bool danger = false, cur = false;             // cur: the tool in use (ring)
  bool keep = false;                            // a setting: run, the menu stays open (made anew by the spec's rebuild)
  std::function<void(bool)> hot;                // the pointer came onto (true) / left (false) the item
  std::function<void()> dwell;                  // held 220 ms under the pointer
};
struct PieSpec {
  QString title;
  std::vector<PieItem> top, bottom, ring;
  std::vector<PieItem> inner;                   // a second ring inside the ring (the tool's settings)
  bool bare = false;                            // ring items with pictures: no box behind them (the picture has its own), the pictures 20% bigger
  double arc0 = 0, arc1 = 0;                    // ring on an arc only (radians, 0 — right, π/2 — down); equal — a full ring
  std::vector<double> ringAng;                  // (a full ring: each ring item at its own angle — eight places, gaps kept; empty — evenly)
  std::function<void()> onEnd;                  // the menu closes / another one takes its place
  std::function<PieSpec()> rebuild;             // (the items anew after a keep item ran — in place, no new window)
};

class Pie : public QWidget {
  Q_OBJECT
public:
  // over the window (global position of the pointer); hold — the opening button / key is still down
  static Pie* open(QWidget* window, const QPoint& global, const PieSpec& spec, bool hold, const QString& holdKey = {},
                   std::function<void()> onCancel = {});
  static Pie* current();
  static std::function<void(const QString&)> faqHook;   // (~ over an item: its help in the FAQ — the item's label and tip)
  static void closeCurrent();
  void keyReleased(const QString& code);       // (the window passes the key the pie is held by)

protected:
  void paintEvent(QPaintEvent*) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void keyReleaseEvent(QKeyEvent* e) override;

private:
  struct Btn { QRect r; const PieItem* it; bool ring = false; double ang = 0; int level = 0; int parent = -1; bool inner = false; };
  Pie(QWidget* window, const PieSpec& spec);
  void layoutItems();
  int hit(const QPoint& p) const;
  int ringPick(const QPoint& p) const;
  int ringIcon() const { return spec_.bare ? 43 : 36; }    // (a ring picture's side)
  void setHot(int i);
  void run(int i);
  void finish(bool cancelled);
  QPoint c_;                                    // the centre (local)
  PieSpec spec_;
  std::vector<Btn> btns_;
  int hot_ = -1, openSub_ = -1;
  bool hold_ = false;
  QString holdKey_;
  double ringR_ = 0, ringStep_ = 0, innerR_ = 0, innerStep_ = 0;
  std::function<void()> onCancel_;
  QTimer* dwell_ = nullptr;
  static QPointer<Pie> cur_;
};

// the toast and the HUD, over the given widget (the viewport's container)
void toast(const QString& msg); QString lastToast();   // (lastToast: the last one shown — checks)
std::function<void(const QString&)>& toastHook();       // (each message shown passed on too — the development session's record)
std::function<void(const QString&)>& stepHook();        // (each message shown — the history names its step after it)
// the busy indicator (busyStart / busyEnd): the program's little hair picture filling blue and a label ("Opening…") at
// the bottom right of the viewport while a long task holds the program; painted before the task starts
class BusyScope {
public:
  explicit BusyScope(const QString& label);
  ~BusyScope();
  void progress(double p);                                  // 0…1 (none — it creeps toward 0.9)
private:
  qint64 t0_;
};
// a wait with no scope of its own (a save written in the background): the curl from busyBegin till busyEnd, creeping on
void busyBegin(const QString& label);
void busyEnd();
std::function<QPointF()>& busyPosHook();               // (where the curl sits: a share of the viewport's free room, −1 — its corner; set by the Inspector's settings)
QSize busySize();                                     // (its usual size — the Inspector's preview)
// a longer wait (a computation): the waiting window — the program dimmed, Mara at her board, «Let me think…», the message
// and the curl filling as it goes; shown only when the work takes longer than delayMs (or now()), gone with the scope
void thinkBegin(const QString& msg);   // (a wait with no scope of its own — a manual save written in the background: till thinkEnd)
void thinkEnd();
QImage thinkSample(const QString& msg, double p, const QSize& size);   // (checks: the window as it would look)
class ThinkScope {
public:
  explicit ThinkScope(const QString& msg = {}, int delayMs = 250);
  ~ThinkScope();
  void progress(double p);                                  // 0…1 (shows it once the delay is past)
  void progress(double p, const QString& stage);            // (the message too: what is being done now)
  void now();                                               // (shown at once — before a long step with no progress of its own)
private:
  qint64 t0_; int delay_; QString msg_; bool shown_ = false;
};
void hud(const QString& msg);                  // empty — hide
void hud(const QString& msg, int ms);          // (shown for ms, then hidden — unless another message came)
void setOverlayHost(QWidget* toastHost, QWidget* hudHost);

}  // namespace hm::ui
