// The bridge to Photoshop and 3D-Coat: a layer / map / mesh is written to a working folder and the program opened on it; the file
// is watched, and what the program saves back arrives as a signal (the Paint room puts it into the layer). Photoshop: the PNG
// of a layer's map round-trips (Save in Photoshop - back into the layer). 3D-Coat: the OBJ with the UVs and the maps' PNGs are
// sent; the textures 3D-Coat exports to the return folder (File - Export - Painted Textures) are picked up the same way.
#pragma once
#include <QFileSystemWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QTimer>

namespace hm { struct PolyMesh; }

namespace hm::paint {

enum class BridgeApp { Photoshop, ThreeDCoat, Krita, Sai, Other };   // (Krita / SAI / any program: the 2D editor of one's choice)

class Bridge : public QObject {
  Q_OBJECT
public:
  explicit Bridge(QObject* parent = nullptr);
  static QString appName(BridgeApp a);
  QString exePath(BridgeApp a) const;                    // the chosen one, else the one found in the usual places ('' - none)
  void setExePath(BridgeApp a, const QString& path);
  static QString findExe(BridgeApp a);
  // the 2D editor the layers go to (Photoshop, Krita, SAI or one's own program) — kept in the settings
  BridgeApp editor() const;
  void setEditor(BridgeApp a);
  QString workFolder() const;
  QString returnFolder() const { return returnDir_; }
  void setReturnFolder(const QString& dir);

  // a map to Photoshop: key says where it comes back to (a layer's id as text). false + error when the program isn't found
  bool sendImage(BridgeApp app, const QString& key, const QImage& img, QString* error = nullptr);
  // a file of several layers (a PSD the caller wrote at `path`) opened in the editor and watched: saved there — layersReturned(path)
  bool sendFile(BridgeApp app, const QString& path, QString* error = nullptr);
  // a mesh with its maps to 3D-Coat (maps: file name -> image)
  bool sendMesh(const hm::PolyMesh& mesh, const QString& name, const QHash<QString, QImage>& maps, QString* error = nullptr);
  static bool writeObj(const hm::PolyMesh& mesh, const QString& path);
signals:
  void imageReturned(const QString& key, const QImage& img);      // (Photoshop saved the file)
  void fileReturned(const QString& path);                          // (a new / changed file in the return folder)
  void layersReturned(const QString& path);                        // (the layered file sent with sendFile was saved)
private:
  QFileSystemWatcher watch_; QHash<QString, QString> keyOf_;      // path -> key
  QHash<QString, qint64> seen_; QString returnDir_; QTimer debounce_; QStringList pending_;
  QHash<QString, QPair<qint64, qint64>> sent_;                    // (a work file as this program wrote it — its time and size: that change is not the editor's)
  void noteSent(const QString& path);
  void changed(const QString& path);
  void flush();
};

}  // namespace hm::paint
