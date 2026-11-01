// Textures of imported meshes (the HTML version's looks): a mesh with UVs keeps raw.look = { tex — textures shown, flat —
// unlit, sets: [{ name, on, color? ('#rrggbb' over the file's), alpha? (the colour map's own alpha is its opacity; absent —
// as the file says), maps: { map / alphaMap: { name, data } — an image kept in the project (a data URL) | { name, path,
// dir } — an image of the connected texture folder (linked: read from there each time) | null — the file's own map removed
// | absent — the file's own } }] }, a set per material of the file (texSets). The texture folder: a folder of images
// (sub-folders too) — its thumbnails in the texture window, its files linked into maps.
#pragma once
#include "io/MeshImport.h"
#include "scene/Scene.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <vector>

namespace hm::looks {

inline const char* SLOT_KEY[2] = {"map", "alphaMap"};
inline const char* SLOT_LABEL[2] = {"Colour", "Opacity"};
constexpr unsigned HEAD_GREY = 0x9a9a9a;

const PolyMesh* baseOf(const Item& m);
std::vector<TexSet> setsOf(const Item& m);
bool hasUV(const Item& m);
// a look made to fit the sets (normLook): a set takes the old set of its name (or of its place when the count is the same)
QJsonObject norm(const QJsonValue& look, const std::vector<TexSet>& sets);
QJsonObject lookOf(const Item& m);                           // normalised (none yet — the default)
bool shown(const Item& m);                                   // textures on and UVs there (imeshTexShown)

// where a set's map comes from now
struct MapSrc {
  enum Kind { None, File, Kept, Linked } kind = None;
  QString key;          // the image's identity (cached textures)
  QByteArray bytes;     // the image file's bytes (File / Kept)
  QString path;         // Linked: the file on disk
  QString label;        // what the window shows
  bool missing = false; // Linked, not found in the folder
  bool removed = false; // the file's own map taken off (null)
};
MapSrc resolve(const Item& m, int set, int slot);
// a set's colour: the user's, else white under a colour map (the texture as it is), else the file's / the plain grey
QString setColor(const Item& m, int set, bool hasMap);
bool setAlpha(const Item& m, int set);                       // the colour map's alpha is its opacity
QByteArray dataUrlBytes(const QString& url);
QImage decodeImage(const QByteArray& bytes);                 // Qt's formats + TGA
QImage loadImage(const QString& file);
QJsonObject keptImage(const QString& file);                  // { name, data } from an image file

// the texture folder (kept between runs)
struct TexFile { QString path, name, abs; qint64 mtime = 0, size = 0; };
struct TexDir { QString name, root; std::vector<TexFile> files; QString filter; };
const TexDir& texDir();
void connectTexDir(const QString& root);                     // "" — disconnected
bool rescanTexDir();                                         // the folder as it is now; true — something changed
const TexFile* texDirFile(const QJsonObject& o);
int revision();                                              // bumps when the folder's images change (textures reload)
// every linked map of the scene's meshes (💾 Save textures into the project)
int linkedCount(const Scene& sc);
// the texture folder of each project: kept in the project (texFolder) and by its path in the settings; opening a
// project connects its own (or a folder of the name its links ask for, next to it) — never the last project's
void projectTexDir(const Scene& sc);
void rememberTexDir(Scene& sc);                              // (after the folder is connected / disconnected by hand)

}  // namespace hm::looks
