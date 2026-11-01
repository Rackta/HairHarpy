// The reference board (the HTML version's #refBoard — like PureRef): a free window over the viewport with reference
// images. Images: drop files, Ctrl+V, ＋ / double-click on empty space; LMB — select / move, corner squares — scale, the
// blue circle — rotate (Shift — 15° steps), C — crop (drag the white edges), Shift+C — uncrop, H — mirror, 0 — reset the
// rotation, [ / ] or Alt+wheel — an image's opacity, Del, PageUp / PageDown — to front / back. The board: wheel — zoom,
// drag on empty space / MMB / Shift+drag — pan, F — fit all. The window: its title bar moves it, the corner resizes it,
// ● (hold, drag up / down) — its opacity, 🔒 — locked: the mouse goes through it (only 🔒 stays; double-click — unlock).
// Its items are the project's (scene.refBoard: saved, not in the undo history); its place, size, opacity, lock and
// whether it is shown are kept between runs.
#pragma once
#include <QWidget>
#include <functional>

namespace hm {

class Scene;

class RefBoard : public QWidget {
public:
  RefBoard(QWidget* host, Scene* sc, std::function<void()> changed);
  bool isOpen() const;
  bool isLocked() const;
  void showBoard(bool on);
  void setLocked(bool on);
  void reload();                                       // (a project opened: its board)
  void addFiles(const QStringList& files, const QPointF* at = nullptr);
protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;
private:
  struct Impl; Impl* d;
};

}  // namespace hm
