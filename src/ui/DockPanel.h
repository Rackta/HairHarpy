// A window of the right column (Active Tool / Settings / Convert to geometry), as the HTML version's .dpanel: a title
// bar (drag it — the window floats; dropped on the right column — pinned back, at that place), "Dock" while floating,
// optionally ▾ collapse and 📌 pin; the body scrolls. The column (DockColumn) keeps its windows' order.
#pragma once
#include <QFrame>
#include <QPointer>
#include <QString>
#include <vector>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace hm::ui {

class DockColumn;

class DockPanel : public QFrame {
  Q_OBJECT
public:
  DockPanel(const QString& title, DockColumn* column, bool collapsible = false, bool pin = false, bool grow = false);
  QWidget* body() const { return body_; }
  void setBody(QWidget* w);                       // (replaces the content; the old one is deleted)
  void setTitle(const QString& t);
  bool floating() const { return floating_; }
  bool collapsed() const { return collapsed_; }
  bool pinned() const { return pinned_; }
  bool grows() const { return grow_; }
  void setFloating(bool on, const QPoint& global = {});
  void setWanted(bool on);                        // (off — hidden wherever it is: a tool without a panel)
  bool wanted() const { return wanted_; }
  void setCollapsed(bool on);
  QSize sizeHint() const override;
signals:
  void pinToggled(bool on);
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
private:
  DockColumn* column_;
  QWidget* head_ = nullptr;
  QLabel* title_ = nullptr;
  QPushButton* helpBtn_ = nullptr; QStringList help_;
  void foldHelp();
  void showHelp(bool on);
  QPushButton *dockBtn_ = nullptr, *colBtn_ = nullptr, *pinBtn_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QWidget* body_ = nullptr;
  bool floating_ = false, collapsed_ = false, pinned_ = false, grow_ = false, dragging_ = false, wanted_ = true;
  QPoint pressAt_, grabOff_;
};

// the right column: the tool box on top, then the windows
class DockColumn : public QFrame {
  Q_OBJECT
public:
  explicit DockColumn(QWidget* parent = nullptr);
  void setTop(QWidget* w);
  void addPanel(DockPanel* p);
  void dock(DockPanel* p, int index = -1);         // (index: among the docked ones; -1 — its own place)
  int indexAt(const QPoint& global) const;         // where a dragged window would go (-1: not over the column)
  void showMark(int index);                        // the blue line where it would go (-1: none)
  void relayout();
private:
  QVBoxLayout* lay_;
  QWidget* top_ = nullptr;
  std::vector<DockPanel*> order_;                  // (all, docked or not: their places)
  QFrame* mark_ = nullptr;
};

}  // namespace hm::ui
