// The tools at the top of the right column (the HTML version's #toolbar): ⧉ SEL, About the author, Support the author
// over rows of icons — Cursor, Move, Rotate, Scale, Brush; Curves: Curve, Scissors, Curve modifiers, Crown, Edges;
// Cocoons: Cocoon (an action), Shell modifiers. The tool in use lit.
#pragma once
#include <QFrame>
#include <QHash>
#include <QPoint>
#include <QString>
#include <QStringList>

class QPushButton; class QSplitter; class QScrollArea;

namespace hm::ui {

class ToolBox : public QFrame {
  Q_OBJECT
public:
  explicit ToolBox(QWidget* parent = nullptr);
  void setCurrent(const QString& tool);              // (empty: none lit — a brush of its own in use)
  QString tip(const QString& tool) const;
  void setRoom(const QString& room);                 // (the Topology room: the Hair tools hidden)
  void setSnap(bool on);                             // (the Hair room's Snap lit)
  void setSym(bool on);                              // (Symmetry lit)
  // the tools of the tool ring, in its order: { id, label, icon }
  struct Tool { QString id, label, icon; };
  static const std::vector<Tool>& ringTools();
  QWidget* rows() const { return rows_; }              // (the tool rows: the Hair room puts them in a floating window — HairToolWin)
signals:
  void toolPicked(const QString& tool);
  void cocoonClicked();
  void snapClicked();
  void symClicked();
  void symSettings(QWidget* anchor);
  void selCopy();
  void about();
  void support();
private:
  QHash<QString, QPushButton*> btns_;
  QWidget* rows_ = nullptr;
  QPushButton* cocoon_ = nullptr;
  QPushButton* snap_ = nullptr; QPushButton *sym_ = nullptr, *symSet_ = nullptr;
};

// The Hair room's tools in a floating window of their own, as the Topology room's: the tools on the left, the tool's
// options on the right (the line between them dragged); drag its bar, its corner — its size; its place, size, split kept.
class HairToolWin : public QFrame {
  Q_OBJECT
public:
  HairToolWin(QWidget* window, QWidget* rows);
  void place(QWidget* vpBox);                           // (its kept place, else the viewport's top left)
  void setOptions(QWidget* w);                          // (the tool in use's options; nullptr — none: a note)
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
private:
  void save() const;
  QWidget* bar_ = nullptr; QPoint grab_; bool drag_ = false;
  QSplitter* split_ = nullptr; QScrollArea* sa_ = nullptr;
  QPushButton* help_ = nullptr; QStringList helpText_;   // (the options' long notes: under this ?, not in the window)
};

}  // namespace hm::ui
