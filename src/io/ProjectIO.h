// .hairmesh projects (JSON, app 'anime-hair-modeler', version 1) — the same files the HTML version writes.
#pragma once
#include "scene/Scene.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace hm {

struct LoadResult { bool ok = false; QString error; QStringList warnings; };

LoadResult loadProject(const QString& path, Scene& scene);
// the same from the file's bytes (a recovery copy, an autosave opened as a copy); path — where its mesh files are looked for
LoadResult loadProjectData(const QByteArray& bytes, const QString& path, Scene& scene);
// one record as loadProject reads it (migration brings records of another project in)
Anchor anchorFromJson(const QJsonObject& r);
Item itemFromJson(const QJsonObject& o, const QString& projectPath, LoadResult& res);
// an imported mesh's source ({ ext, text | b64 | ref }) read and parsed
std::shared_ptr<PolyMesh> readMeshSource(const QJsonObject& src, const QString& projectPath, QString* error = nullptr);

// What the file takes besides the scene: the camera, the File window's preview (a data: URL, empty — the old one kept),
// the cocoons as they are now (live ones reshaped since they were read)
class CocoonSystem;
// convChecked: the lock groups of the selected cocoons (renderConvList keeps S.convChecked so at every outliner refresh)
struct SaveExtras { QJsonObject camera; QString thumb; const CocoonSystem* cocoons = nullptr; std::vector<int> convChecked;
  // (a file written in a background thread: the live cocoons' state taken beforehand, in the window's thread — the cocoon
  // system itself is rebuilt and replaced there while the thread writes)
  bool cocoonSnap = false; std::map<int, std::pair<QJsonObject, QJsonArray>> cocoonLive; };
void snapCocoons(SaveExtras& x, const Scene& scene);        // (fills cocoonLive from x.cocoons and lets the pointer go)

// The project as the HTML version's projectData() writes it (the same keys, the thumb near the start); fields this
// version doesn't handle come from the records as they were read
QByteArray projectJson(const Scene& scene, const SaveExtras& extras);
void expandSharedImages(QJsonObject& project);               // (a texture several models hold is written once, in "images": put back into each map — before the items are read)
// the items and anchors as the file writes them; a room's record (P.rooms.<name>: roots, anchors, items, counters)
void sceneRecords(const Scene& sc, const SaveExtras& x, QJsonArray& items, QJsonArray& anchors);
QJsonObject roomRecord(const Scene& sc);
bool saveProject(const QString& path, const Scene& scene, const SaveExtras& extras, QString* error);

}  // namespace hm
