#include "app/MainWindow.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPalette>
#include <QSettings>
#include <algorithm>
#include <QStyleFactory>
#include <QTimer>
#include <QVulkanInstance>
#include <set>


// the program was "Stylized HairMesh Creator" before 2.0: its settings (recent projects, folders, preferences) carried over
// to HairHarpy's once, on the first start — a path into the old install folder that now lies under the renamed one follows it
static void carryOldSettings() {
  QSettings now("Catarackta", "HairHarpy"), old("Catarackta", "HairMeshCreatorNative");
  if (!now.allKeys().isEmpty() || old.allKeys().isEmpty()) return;
  auto moved = [](const QString& s) { if (!s.contains("Stylized HairMesh Creator")) return s; QString t = s; t.replace("Stylized HairMesh Creator", "HairHarpy");
    return QFileInfo::exists(t) || !QFileInfo::exists(s) ? t : s; };
  for (const QString& k : old.allKeys()) { QVariant v = old.value(k);
    if (v.typeId() == QMetaType::QString) v = moved(v.toString());
    else if (v.typeId() == QMetaType::QStringList) { QStringList l = v.toStringList(); for (QString& s : l) s = moved(s); v = l; }
    now.setValue(k, v); }
}

int main(int argc, char** argv) {
  // the interface size (👁 Display → Interface; 90 % — the size the HTML version was used at in the browser): a scale
  // factor Qt takes before the application starts — a change applies on the next start (⟳ Reload)
  { bool check = false; for (int i = 1; i < argc; i++) if (QByteArray(argv[i]) == "--shot") check = true;
    if (!check) carryOldSettings();
    QSettings s("Catarackta", check ? "HairHarpyCheck" : "HairHarpy");
    double k = std::clamp(s.value("ui/scale", 0.9).toDouble(), 0.6, 1.5);
    qputenv("QT_SCALE_FACTOR", QByteArray::number(k, 'f', 2)); }
  QApplication app(argc, argv);
  QApplication::setOrganizationName("Catarackta");
  // (checks run from the command line — --shot — keep their settings apart: recent projects, autosave, folders)
  QApplication::setApplicationName(app.arguments().contains("--shot") ? "HairHarpyCheck" : "HairHarpy");
  hm::ui::applyTheme(app);

  QVulkanInstance inst;
#ifndef NDEBUG
  inst.setLayers({"VK_LAYER_KHRONOS_validation"});   // (skipped quietly when the SDK's layers aren't installed)
#endif
  inst.setApiVersion(QVersionNumber(1, 1));
  if (!inst.create()) {
    QMessageBox::critical(nullptr, "HairHarpy",
                          QString("Vulkan is not available (error %1). Update the graphics driver.").arg(inst.errorCode()));
    return 1;
  }

  hm::MainWindow w(&inst);
  w.resize(1500, 900);
  w.show();
  // a project given on the command line (a double-click on a .hairmesh file);
  // --shot <png>: render it, save the viewport's frame and quit (checks / regression pictures); --fit: the default
  // framing of the whole model instead of the project's camera
  QStringList args = app.arguments(); args.removeFirst();
  QString shot;
  if (int i = int(args.indexOf("--shot")); i >= 0 && i + 1 < args.size()) { shot = args.at(i + 1); args.remove(i, 2); }
  bool fit = args.removeAll("--fit") > 0;
  if (int i = int(args.indexOf("--size")); i >= 0 && i + 1 < args.size()) {   // --size WxH: the window's size (a check made at the user's screen size)
    const QStringList s = args.at(i + 1).split('x'); if (s.size() == 2) w.resize(s[0].toInt(), s[1].toInt()); args.remove(i, 2); }
  QString view;                                     // --view persp | front | right | left | back | top
  if (int i = int(args.indexOf("--view")); i >= 0 && i + 1 < args.size()) { view = args.at(i + 1); args.remove(i, 2); }
  QString select;                                   // --select <id,id,…>: items selected (checks of what a selection shows)
  if (int i = int(args.indexOf("--select")); i >= 0 && i + 1 < args.size()) { select = args.at(i + 1); args.remove(i, 2); }
  bool given = false;
  if (!args.isEmpty() && !args.first().startsWith("--") && (given = w.openProject(args.first()))) {
    if (fit) w.resetView();
    if (!view.isEmpty()) w.setView(view);
    if (!select.isEmpty()) { std::set<int> ids; for (const QString& s : select.split(',')) ids.insert(s.toInt()); w.selectItems(ids); }
  }
  // --events "<script>": mouse events posted to the viewport once it is up (see MainWindow::runEvents); the report goes
  // next to the shot (<shot>.txt)
  QString events;
  if (int i = int(args.indexOf("--events")); i >= 0 && i + 1 < args.size()) { events = args.at(i + 1); args.remove(i, 2); }
  // (a normal start: autosave, the recovery copy; with no project — ⟳ Reload's project, else the File window)
  if (shot.isEmpty()) QTimer::singleShot(300, &w, [&w, given] { w.startup(given); });
  if (!shot.isEmpty()) QTimer::singleShot(1500, &w, [&w, shot, events] {
    w.forgetChanges();
    if (!events.isEmpty()) {
      QString rep = w.runEvents(events);
      QFile f(shot + ".txt"); if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(rep.toUtf8());
      QTimer::singleShot(700, &w, [&w, shot] { w.saveViewportShot(shot); QApplication::quit(); });
      return;
    }
    w.saveViewportShot(shot); QApplication::quit();
  });
  return app.exec();
}
