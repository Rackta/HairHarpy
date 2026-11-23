// The shader mode's controls (HairHarpy Toon): one switch for the program — its button in the Topology room's modes (under UV Mats) and
// in the Paint room's tools; a click switches the mode, a double click opens the Materials & Shader window (materials, their instances,
// the parameters by module, who wears which instance — an object's UV set).
#pragma once
#include <QObject>
#include <QPushButton>
#include <functional>

namespace hm {
class Viewport; struct Scene;

class ToonHub : public QObject {
  Q_OBJECT
public:
  static ToonHub* get();
  std::function<bool()> isOn; std::function<void(bool)> setOn; std::function<void()> openMenu;
  std::function<QString(const QString&)> exportTo;   // (the shader with its maps written into a folder for Unreal, the website, Unity — what was written, or "!" + why not)
  std::function<bool(double[3], double&)> headGuess;  // (where the head is: its middle and a size — from Head Collision or the parts over the hair)
signals:
  void changed();                       // (the mode switched, the library edited: the buttons and the window follow)
};

class ToonButton : public QPushButton {
  Q_OBJECT
public:
  explicit ToonButton(int px, QWidget* parent = nullptr);
protected:
  void mouseDoubleClickEvent(QMouseEvent* e) override;
};

// the Materials & Shader window (one, reused); libChanged — the library written back to the project and drawn again
QWidget* toonWindow(Scene* sc, Viewport* vp, std::function<void()> libChanged, QWidget* parent);

}  // namespace hm
