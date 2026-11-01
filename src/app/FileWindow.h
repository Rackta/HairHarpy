// 📁 File (the HTML version's File window — also the start screen): Recent projects (cards with a preview, Open…, + New
// project), Examples (projects shipped with the program — opened as a copy), Autosaves (the newest 5 in the autosave
// folder — opened as a copy), Migrate (another project's hair into this one; presets: the examples' hair and your own
// preset files, ＋ Create preset from the selected group). Previews: the `thumb` stored near the start of a project file.
// Also: the autosave settings dialog, the drop overlay (files dropped onto the window: Open / Migrate / Import as head /
// Import as mesh), the recovery and broken-walls offers.
#pragma once
#include <QDialog>
#include <QString>
#include <functional>
#include <vector>

class QWidget;

namespace hm::files {

struct Recent { QString path, name, thumb; qint64 time = 0; };
std::vector<Recent> recentList();
void addRecent(const QString& path, const QString& thumb);    // (no thumb — the one kept from before)
void removeRecent(const QString& path);
QString fileThumb(const QString& path);                       // the preview stored near the start of a file (a data: URL)

struct AutosaveSettings { bool on = true; int minutes = 10, copies = 5; QString dir; };
AutosaveSettings autosaveSettings();
void setAutosaveSettings(const AutosaveSettings& s);
QString presetsDir();
void setPresetsDir(const QString& d);
QString examplesDir();                                        // the program's Examples folder ("" — none)
QString appDataDir();                                         // (recovery copy, reload, default folders)
QString dataDir(const QString& sub);                          // Presets / Autosaves / Projects: the installation's, else app data
QString baseName(const QString& file);                        // name without .hairmesh / .json

// what the window asks the main window to do
struct Host {
  std::function<bool(const QString& path)> open;              // a project file (asks about unsaved changes itself)
  std::function<void()> openDialog;
  std::function<void(const QString& room)> newProject;       // ＋ New project, opened in that room (hair / topo)
  std::function<bool(const QString& path, const QString& name, bool unsaved)> openCopy;   // an example / autosave: no file of its own
  std::function<bool(const QString& path, const QString& name, bool wrap)> migrate;
  std::function<void()> migrateDialog;
  std::function<QString()> presetGroupName;                   // "" — no group selected
  std::function<bool(const QString& dir)> createPreset;
  std::function<void()> autosaveSettings;
};

void openFileWindow(QWidget* parent, const Host& host, const QString& tab = QString());
void closeFileWindow();
void openAutosaveDialog(QWidget* parent, const QString& projectBase, const QString& lastMsg, const std::function<void()>& saveNow);

}  // namespace hm::files
