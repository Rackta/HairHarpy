// The tools at the top of the right column (the HTML version's #toolbar): ⧉ SEL, About the author, Support the author
// over rows of icons — Cursor, Move, Rotate, Scale, Brush; Curves: Curve, Scissors, Curve modifiers, Crown, Edges;
// Cocoons: Cocoon (an action), Shell modifiers. The tool in use lit.
#pragma once
#include <QFrame>
#include <QHash>
#include <QString>

class QPushButton;

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
  QPushButton* cocoon_ = nullptr;
  QPushButton* snap_ = nullptr; QPushButton *sym_ = nullptr, *symSet_ = nullptr;
};

}  // namespace hm::ui
