#include "paint/Bridge.h"
#include <QRegularExpression>
#include "scene/Scene.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

namespace hm::paint {

Bridge::Bridge(QObject* parent) : QObject(parent) {
  debounce_.setSingleShot(true); debounce_.setInterval(400);
  connect(&debounce_, &QTimer::timeout, this, &Bridge::flush);
  connect(&watch_, &QFileSystemWatcher::fileChanged, this, [this](const QString& p) { changed(p); if (QFileInfo::exists(p) && !watch_.files().contains(p)) watch_.addPath(p); });   // (Photoshop saves by replacing the file)
  connect(&watch_, &QFileSystemWatcher::directoryChanged, this, [this](const QString& d) {
    for (const QFileInfo& f : QDir(d).entryInfoList({"*.png", "*.tga", "*.jpg", "*.jpeg", "*.tif", "*.tiff", "*.bmp"}, QDir::Files)) {
      const qint64 m = f.lastModified().toMSecsSinceEpoch(); if (seen_.value(f.absoluteFilePath(), 0) != m) { seen_[f.absoluteFilePath()] = m; changed(f.absoluteFilePath()); } } });
  QSettings s; returnDir_ = s.value("paint/returnFolder").toString(); if (!returnDir_.isEmpty() && QDir(returnDir_).exists()) watch_.addPath(returnDir_);
}

QString Bridge::appName(BridgeApp a) { switch (a) { case BridgeApp::Photoshop: return "Photoshop"; case BridgeApp::ThreeDCoat: return "3D-Coat"; case BridgeApp::Krita: return "Krita"; case BridgeApp::Sai: return "PaintTool SAI"; default: return tr("the editor"); } }
BridgeApp Bridge::editor() const { const int v = QSettings().value("paint/editor", 0).toInt(); return v == 2 ? BridgeApp::Krita : v == 3 ? BridgeApp::Sai : v == 4 ? BridgeApp::Other : BridgeApp::Photoshop; }
void Bridge::setEditor(BridgeApp a) { QSettings().setValue("paint/editor", int(a)); }

QString Bridge::findExe(BridgeApp a) {
  const QStringList roots{qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)")};
  for (const QString& root : roots) {
    if (root.isEmpty()) continue;
    if (a == BridgeApp::Photoshop) {
      for (const QFileInfo& d : QDir(root + "/Adobe").entryInfoList({"Adobe Photoshop*"}, QDir::Dirs, QDir::Name | QDir::Reversed)) { const QString e = d.absoluteFilePath() + "/Photoshop.exe"; if (QFile::exists(e)) return e; }
    } else if (a == BridgeApp::Krita) { for (const char* n : {"/Krita (x64)/bin/krita.exe", "/Krita/bin/krita.exe"}) if (QFile::exists(root + n)) return root + n;
    } else if (a == BridgeApp::Sai) { for (const QFileInfo& d : QDir(root).entryInfoList({"PaintTool SAI*", "SYSTEMAX*"}, QDir::Dirs, QDir::Name | QDir::Reversed))
        for (const char* n : {"/sai2.exe", "/sai.exe", "/PaintTool SAI Ver2/sai2.exe"}) if (QFile::exists(d.absoluteFilePath() + n)) return d.absoluteFilePath() + n;
    } else if (a == BridgeApp::Other) { return {};
    } else {
      for (const QFileInfo& d : QDir(root).entryInfoList({"3D-Coat*"}, QDir::Dirs, QDir::Name | QDir::Reversed))
        for (const char* n : {"/3D-CoatDX64.exe", "/3D-CoatGL64.exe", "/3D-Coat.exe"}) { const QString e = d.absoluteFilePath() + n; if (QFile::exists(e)) return e; }
    }
  }
  return {};
}

QString Bridge::exePath(BridgeApp a) const {
  const QString k = a == BridgeApp::Photoshop ? "paint/exePS" : a == BridgeApp::ThreeDCoat ? "paint/exe3DC" : a == BridgeApp::Krita ? "paint/exeKrita" : a == BridgeApp::Sai ? "paint/exeSai" : "paint/exeOther"; const QString v = QSettings().value(k).toString();
  return !v.isEmpty() && QFile::exists(v) ? v : findExe(a);
}
void Bridge::setExePath(BridgeApp a, const QString& path) { QSettings().setValue(a == BridgeApp::Photoshop ? "paint/exePS" : a == BridgeApp::ThreeDCoat ? "paint/exe3DC" : a == BridgeApp::Krita ? "paint/exeKrita" : a == BridgeApp::Sai ? "paint/exeSai" : "paint/exeOther", path); }

QString Bridge::workFolder() const { const QString d = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/HairHarpyBridge"; QDir().mkpath(d); return d; }

void Bridge::setReturnFolder(const QString& dir) {
  if (!returnDir_.isEmpty()) watch_.removePath(returnDir_);
  returnDir_ = dir; QSettings().setValue("paint/returnFolder", dir);
  if (!dir.isEmpty() && QDir(dir).exists()) { watch_.addPath(dir); for (const QFileInfo& f : QDir(dir).entryInfoList(QDir::Files)) seen_[f.absoluteFilePath()] = f.lastModified().toMSecsSinceEpoch(); }   // (what lies there now is not new)
}

bool Bridge::sendImage(BridgeApp app, const QString& key, const QImage& img, QString* error) {
  const QString exe = exePath(app);
  if (exe.isEmpty()) { if (error) *error = tr("%1 is not found: set its path in the Bridge panel").arg(appName(app)); return false; }
  QString safe = key; safe.replace(QRegularExpression("[^A-Za-z0-9_@.-]"), "_");   // (the key may hold a UV set's name: its file name kept plain)
  const QString path = workFolder() + "/layer_" + safe + ".png";
  if (!img.save(path)) { if (error) *error = tr("The file can't be written: %1").arg(path); return false; }
  keyOf_[path] = key; noteSent(path); if (!watch_.files().contains(path)) watch_.addPath(path);
  if (!QProcess::startDetached(exe, {QDir::toNativeSeparators(path)})) { if (error) *error = tr("%1 can't be started").arg(appName(app)); return false; }
  return true;
}

void Bridge::noteSent(const QString& path) { const QFileInfo f(path); if (f.exists()) sent_[path] = {f.lastModified().toMSecsSinceEpoch(), f.size()}; }

bool Bridge::sendFile(BridgeApp app, const QString& path, QString* error) {
  const QString exe = exePath(app);
  if (exe.isEmpty()) { if (error) *error = tr("%1 is not found: set its path in the Bridge panel").arg(appName(app)); return false; }
  keyOf_[path] = QStringLiteral("\x01layers"); noteSent(path); if (!watch_.files().contains(path)) watch_.addPath(path);
  if (!QProcess::startDetached(exe, {QDir::toNativeSeparators(path)})) { if (error) *error = tr("%1 can't be started").arg(appName(app)); return false; }
  return true;
}

bool Bridge::writeObj(const hm::PolyMesh& m, const QString& path) {
  QFile f(path); if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
  QTextStream o(&f); o.setRealNumberNotation(QTextStream::FixedNotation); o.setRealNumberPrecision(6); o << "# HairHarpy\n";
  for (const auto& p : m.pos) o << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
  const bool uv = m.cornerUV.size() == m.faceVerts.size() && !m.cornerUV.empty();
  if (uv) for (const auto& u : m.cornerUV) o << "vt " << u[0] << ' ' << (m.flipY ? 1.0 - u[1] : u[1]) << '\n';
  for (int fI = 0; fI < m.faceCount(); fI++) { o << 'f'; for (int i = m.faceStart[fI]; i < m.faceStart[fI + 1]; i++) { o << ' ' << m.faceVerts[i] + 1; if (uv) o << '/' << i + 1; } o << '\n'; }
  return true;
}

bool Bridge::sendMesh(const hm::PolyMesh& mesh, const QString& name, const QHash<QString, QImage>& maps, QString* error) {
  const QString exe = exePath(BridgeApp::ThreeDCoat);
  if (exe.isEmpty()) { if (error) *error = tr("3D-Coat is not found: set its path in the Bridge panel"); return false; }
  const QString dir = workFolder() + "/" + name; QDir().mkpath(dir);
  if (!writeObj(mesh, dir + "/" + name + ".obj")) { if (error) *error = tr("The OBJ can't be written"); return false; }
  for (auto it = maps.begin(); it != maps.end(); ++it) it.value().save(dir + "/" + name + "_" + it.key() + ".png");
  if (!QProcess::startDetached(exe, {QDir::toNativeSeparators(dir + "/" + name + ".obj")})) { if (error) *error = tr("3D-Coat can't be started"); return false; }
  return true;
}

void Bridge::changed(const QString& path) { if (!pending_.contains(path)) pending_ << path; debounce_.start(); }

void Bridge::flush() {
  const QStringList p = pending_; pending_.clear();
  for (const QString& path : p) {
    if (const auto s = sent_.constFind(path); s != sent_.constEnd()) { const QFileInfo f(path);   // (the program's own write of the work file — a second send: not something back from the editor)
      if (f.exists() && f.lastModified().toMSecsSinceEpoch() == s->first && f.size() == s->second) continue; }
    if (keyOf_.value(path) ==QStringLiteral("\x01layers")) { if (QFileInfo::exists(path)) emit layersReturned(path); }
    else if (keyOf_.contains(path)) { QImage im(path); if (!im.isNull()) emit imageReturned(keyOf_[path], im); }
    else emit fileReturned(path);
  }
}

}  // namespace hm::paint
