// The key display (the Inspector's ⌨ button): a small window over the program showing what is pressed — the keys as key
// caps with their modifiers (Ctrl + Shift + K), a mouse button as a mouse with that button lit (the wheel: an arrow up /
// down), a repeat as ×N, the last few above it; for recording lessons and showing the hotkeys. Placed only in the Inspector's
// preview (as the frame counter and the numbers) — never caught by the mouse: clicks go through it to the program
#pragma once
#include <QElapsedTimer>
#include <QWidget>
#include <vector>

class QTimer;

namespace hm {

class KeyCastWindow : public QWidget {
  Q_OBJECT
public:
  explicit KeyCastWindow(QWidget* mainWindow);
  ~KeyCastWindow() override;
  void open();                                     // (shown at its place — set in the Inspector, else the viewport's bottom left)
  void setHost(QWidget* host) { host_ = host; }    // (the viewport: what its place is a share of)
  void place();
  // (checks: what a key / a mouse button sends, as if pressed — `mods` Qt::KeyboardModifiers)
  void testKey(int key, int mods, bool press);
  void testMouse(int button, int mods, bool press, int wheel = 0);
  // its look (kept): the keys' and the mouse's size (1 — as made), the background's opacity (0 — the keys alone)
  void setLook(double scale, double bgOpacity);
  double scale() const { return scale_; }
  double bgOpacity() const { return bgOp_; }
signals:
  void closed();
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void enterEvent(QEnterEvent*) override;
  void leaveEvent(QEvent*) override;
private:
  enum : int { L = 1, R = 2, M = 4, WUp = 8, WDown = 16, X1 = 32, X2 = 64 };
  struct Token { QString text; int mouse = 0; bool operator==(const Token& o) const { return text == o.text && mouse == o.mouse; } };
  using Chord = std::vector<Token>;
  void onKey(int key, quint32 vk, Qt::KeyboardModifiers mods, bool press);
  void onMouse(int bits, Qt::KeyboardModifiers mods, bool press);
  void commit(const Chord& c);
  Chord modTokens(Qt::KeyboardModifiers mods) const;
  static QString keyName(int key, quint32 vk);
  int chordWidth(const Chord& c, double s) const;
  int drawChord(class QPainter& p, const Chord& c, int x, int y, double s, double alpha) const;
  void fit();
  QWidget* main_; QWidget* host_ = nullptr;
  double scale_ = 1, bgOp_ = 0.88;
  Chord cur_;                                      // (what is shown big)
  bool modsOnly_ = false;                          // (only modifiers held so far: not yet a combination)
  Chord before_; int beforeCount_ = 1;             // (what was shown before them)
  int count_ = 1;                                  // (the same combination again: ×N)
  std::vector<Chord> hist_;                        // (the ones before, oldest first)
  int held_ = 0;                                   // (the mouse buttons held now)
  QElapsedTimer idle_;                             // (since the last press / release: the big row dims)
  QTimer* tick_ = nullptr;
  ulong lastStamp_ = 0; int lastType_ = 0, lastCode_ = 0;   // (one press seen once: the app's filter gets it per receiver)
  QPoint grab_; bool drag_ = false, hover_ = false;
};

}  // namespace hm
