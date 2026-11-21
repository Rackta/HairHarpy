// The bridge from ZBrush (ZBrushLink.h).
#include "app/ZBrushLink.h"
#include "app/ImportMesh.h"
#include "ui/Pie.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QWidget>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace hm {

namespace { ZBrushLink* g_link = nullptr; const char* PLUGIN = "hairharpy_zbrush.py"; }

ZBrushLink* ZBrushLink::get() { return g_link; }

QString ZBrushLink::root() {
  QString base = qEnvironmentVariable("LOCALAPPDATA");
  if (base.isEmpty()) base = QDir::homePath();
  return QDir(base).filePath("HairHarpy/ZBrush");
}
QString ZBrushLink::pluginDir() { return QDir(root()).filePath("plugin"); }

ZBrushLink::ZBrushLink(QWidget* window, Viewport* vp, Scene* sc) : QObject(window), win_(window), vp_(vp), sc_(sc) {
  g_link = this;
  debounce_.setSingleShot(true); debounce_.setInterval(300);
  connect(&debounce_, &QTimer::timeout, this, &ZBrushLink::take);
  connect(&watch_, &QFileSystemWatcher::directoryChanged, this, [this] { debounce_.start(); });
  setReceiving(QSettings().value("zbrush/receive", false).toBool());
}
ZBrushLink::~ZBrushLink() { if (!pidFile_.isEmpty()) QFile::remove(pidFile_); if (g_link == this) g_link = nullptr; }

void ZBrushLink::setReceiving(bool on) {
  QSettings().setValue("zbrush/receive", on);
  if (on == on_) return;
  on_ = on;
  const QString inbox = QDir(root()).filePath("inbox");
  if (on) {
    QDir().mkpath(inbox);
    pidFile_ = QDir(root()).filePath(QString("running_%1.pid").arg(QCoreApplication::applicationPid()));
    QFile f(pidFile_); if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::number(QCoreApplication::applicationPid()));
    watch_.addPath(inbox);
    debounce_.start();                                         // (what was sent while the program was closed)
  } else {
    if (!watch_.directories().isEmpty()) watch_.removePaths(watch_.directories());
    if (!pidFile_.isEmpty()) QFile::remove(pidFile_); pidFile_.clear();
  }
}

void ZBrushLink::checkNow() { take(); }

void ZBrushLink::take() {
  if (!on_ || busy_) return;
  busy_ = true;
  const QDir inbox(QDir(root()).filePath("inbox"));
  const auto files = inbox.entryInfoList({"*.obj"}, QDir::Files, QDir::Time | QDir::Reversed);
  int got = 0; QStringList names;
  for (const QFileInfo& fi : files) {
    if (QDateTime::currentDateTime().msecsTo(fi.lastModified()) > -200) { debounce_.start(); continue; }   // (still being moved in: next time)
    ui::ThinkScope think(tr("From ZBrush: %1…").arg(fi.completeBaseName()), 400); think.now();
    const QString r = receiveMesh(fi.absoluteFilePath(), fi.completeBaseName(), "ZBrush", vp_, sc_);
    QFile::remove(fi.absoluteFilePath());
    if (r.isEmpty()) { ui::toast(tr("From ZBrush: %1 could not be read").arg(fi.completeBaseName())); continue; }
    got++; names << fi.completeBaseName();
  }
  busy_ = false;
  if (got && win_) QApplication::alert(win_->window());               // (the taskbar button flashes: it has come)
}

bool ZBrushLink::pluginInstalled() const {
  if (!QFile::exists(QDir(pluginDir()).filePath(PLUGIN))) return false;
#ifdef _WIN32
  const QSettings env("HKEY_CURRENT_USER\\Environment", QSettings::NativeFormat);
  const QStringList parts = env.value("ZBRUSH_PLUGIN_PATH").toString().split(';', Qt::SkipEmptyParts);
  for (const QString& p : parts) if (QDir::cleanPath(QDir::fromNativeSeparators(p)).compare(QDir::cleanPath(pluginDir()), Qt::CaseInsensitive) == 0) return true;
  return false;
#else
  return true;
#endif
}

QString ZBrushLink::installPlugin() {
  QFile src(QString(":/bridge/%1").arg(PLUGIN)); if (!src.open(QIODevice::ReadOnly)) return "!" + tr("The plugin is missing from the program");
  QDir().mkpath(pluginDir());
  const QString dst = QDir(pluginDir()).filePath(PLUGIN);
  QFile::remove(dst);
  { QFile o(dst); if (!o.open(QIODevice::WriteOnly) || o.write(src.readAll()) < 0) return "!" + tr("Could not write %1").arg(QDir::toNativeSeparators(dst)); }
#ifdef _WIN32
  QSettings env("HKEY_CURRENT_USER\\Environment", QSettings::NativeFormat);
  QStringList parts = env.value("ZBRUSH_PLUGIN_PATH").toString().split(';', Qt::SkipEmptyParts);
  const QString mine = QDir::toNativeSeparators(pluginDir()); bool have = false;
  for (const QString& p : parts) if (QDir::cleanPath(QDir::fromNativeSeparators(p)).compare(QDir::cleanPath(pluginDir()), Qt::CaseInsensitive) == 0) have = true;
  if (!have) { parts << mine; env.setValue("ZBRUSH_PLUGIN_PATH", parts.join(';')); env.sync(); }
  DWORD_PTR res = 0;                                                  // (programs started from now on see the new variable)
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, LPARAM(L"Environment"), SMTO_ABORTIFHUNG, 2000, &res);
  if (env.status() != QSettings::NoError) return "!" + tr("Could not set ZBRUSH_PLUGIN_PATH");
#endif
  setReceiving(true);
  return tr("ZBrush plugin installed — restart ZBrush from the Start menu or a shortcut (a launcher that was already running keeps the old environment): Zplugin → HairHarpy → Send SubTool");
}

}  // namespace hm
