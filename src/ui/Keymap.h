// Hotkeys as the HTML version's ACTIONS / KEYS: every action has a key combination ("Ctrl+Shift+KeyH" — modifiers +
// the physical key's code, so they work in any keyboard layout), changed in the hotkey manager and kept in the
// settings. Besides the actions: keys put on any button of the window (End over it), pie menus of one's own, the mouse
// buttons of the camera and its turning, presets saved to a file. What the program comes with is the standard (the
// factory settings). Esc and End are never a hotkey; B held, H tapped / held and ~ stay fixed.
#pragma once
#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

class QKeyEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QVBoxLayout;
class QPushButton;
class QLineEdit;
class QListWidget;
class QLabel;
class QAbstractButton;

namespace hm::ui {

struct ActionDef { QString id, label, defKey; bool repeatable = false; QString cat; };
struct CustomBind { QString id, label, text, tip, combo, tool; };    // (a button of the window, clicked by its key: found by its text and tip; tool — the one whose options hold it)
struct CustomPie { QString id, name, combo; QStringList items; };    // (a pie of one's own: up to 8 actions on a ring)
// a mouse binding: button 1 — LMB, 2 — MMB, 3 — RMB (0 — none); mods: 1 — Ctrl, 2 — Shift, 4 — Alt
struct NavBind { int button = 0, mods = 0; };
struct NavScheme {
  QString preset = "standard";
  NavBind orbit{1, 4}, pan{2, 4}, pan2{2, 0}, zoom{3, 4}, mainPie{3, 2}, opsPie{3, 1};
  QString rotation = "turntable";                                    // turntable (the up stays up) / trackball (free)
};

class Keymap {
public:
  static Keymap& get();
  const std::vector<ActionDef>& defs() const { return defs_; }
  QString key(const QString& id) const;                   // (an action's, a button's, a pie's)
  void setKey(const QString& id, const QString& combo);   // (a clash: the other one loses its key)
  QString clashWith(const QString& id, const QString& combo) const;
  void reset();                                           // (the actions' keys back to the standard)
  void factoryReset();                                    // (everything: keys, buttons, pies, the mouse)
  QString actionFor(const QString& combo) const;
  const ActionDef* def(const QString& id) const;
  QString labelOf(const QString& id) const;
  QStringList categories() const;

  // keys on buttons of the window
  const std::vector<CustomBind>& custom() const { return custom_; }
  QString bindButton(const QString& label, const QString& text, const QString& tip, const QString& combo, const QString& tool = {});   // → its id
  QString buttonId(const QString& label, const QString& text, const QString& tip, const QString& tool = {});   // (found or added — its key as it was)
  const CustomBind* customBind(const QString& id) const { for (auto& b : custom_) if (b.id == id) return &b; return nullptr; }
  std::function<QString(const QWidget*)> toolOf;           // (set by the window: the tool whose options hold this button — none: always on screen)
  void removeCustom(const QString& id);
  void setCustomTool(const QString& id, const QString& tool);
  // (false: no such button on screen now; justMade: a button not shown yet counts — its panel was just made)
  QAbstractButton* customButton(const QString& id, bool justMade = false) const;
  bool triggerCustom(const QString& id, bool justMade = false) const;
  // pies of one's own
  const std::vector<CustomPie>& pies() const { return pies_; }
  void setPies(const std::vector<CustomPie>& p);
  // the mouse
  const NavScheme& nav() const { return nav_; }
  void setNav(const NavScheme& n);
  static NavScheme navPreset(const QString& name);        // standard / blender / maya / max
  int navFor(int button, int mods) const;                 // 0 none, 1 orbit, 2 pan, 3 zoom (the binding with the most modifiers held)
  bool pieMouse(const QString& which, int button, int mods) const;   // "main" / "ops"
  static int buttonOf(int qtButton);                      // Qt::MouseButton → 1 / 2 / 3
  static int modsOf(int qtMods);                          // Qt::KeyboardModifiers → 1 / 2 / 4
  static QString navLabel(const NavBind& b);

  QJsonObject toJson() const;
  bool fromJson(const QJsonObject& o);

  static QString codeOf(const QKeyEvent* e);              // "KeyA", "Digit1", "F2", "Space", "BracketLeft", … ("" — none)
  static QString comboOf(const QKeyEvent* e);             // modifiers + code
  static QString label(const QString& combo);             // "Ctrl + H"
  static bool assignable(const QString& combo);           // (Esc and End — never)

private:
  Keymap();
  void save() const;
  void load();
  std::vector<ActionDef> defs_;
  QHash<QString, QString> keys_;
  std::vector<CustomBind> custom_;
  std::vector<CustomPie> pies_;
  NavScheme nav_;
};

// the hotkey manager: sections on the left (the actions by kind, pie menus, keys on buttons, the mouse and camera,
// presets), a search over every key on top; a key clicked — the new combination pressed and let go (Esc — cancel,
// Backspace — clear)
class HotkeyDialog : public QDialog {
  Q_OBJECT
public:
  explicit HotkeyDialog(QWidget* parent);
signals:
  void message(const QString& text);
protected:
  void keyPressEvent(QKeyEvent* e) override;
  void keyReleaseEvent(QKeyEvent* e) override;
  // (open, it is not modal: any button or tool of the window dragged onto it — into a slot of a pie of one's own)
  bool eventFilter(QObject* o, QEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
  void dropEvent(QDropEvent* e) override;
private:
  void fill();
  void setSlot(int slot, const QString& id);     // (a place of the pie being edited: its action — empty, cleared)
  void swapSlots(int a, int b);                 // (two places of it: their actions swapped)
  void dropFunc(const QString& id, int slot);   // (slot −1: the first empty one of the pie open — none: the first pie, or a new one)
  QPointer<QWidget> dragSrc_;
  QPoint dragAt_;
  void keyRow(QVBoxLayout* v, const QString& id, const QString& label, bool removable);
  void piesPage(QVBoxLayout* v);
  void mousePage(QVBoxLayout* v);
  void presetsPage(QVBoxLayout* v);
  QVBoxLayout* rows_ = nullptr;
  QLineEdit* search_ = nullptr;
  QListWidget* cats_ = nullptr;
  QString capture_, pending_;
  QPushButton* captureBtn_ = nullptr;
  std::function<void(const QString&)> onCaptured_;
  int editPie_ = -1;
};

// End over anything of the window: "press the keys for it" — let go, they are its hotkey (Esc — cancel)
class KeyCapture : public QWidget {
  Q_OBJECT
public:
  static void start(QWidget* window, const QString& name, std::function<void(const QString&)> done);
  static bool active();
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
  void paintEvent(QPaintEvent*) override;
private:
  KeyCapture(QWidget* window, const QString& name, std::function<void(const QString&)> done);
  QString name_, combo_, note_;
  std::function<void(const QString&)> done_;
  void finish(bool ok);
};

}  // namespace hm::ui
