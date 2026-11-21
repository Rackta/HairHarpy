// The Paint room's panels of the right column: Layers (layers, masks, fill / adjustment layers, blends), Brush (the brush list with
// the ABR import, the dynamics, the bump / flow modes), Bake (a high-poly onto a low-poly's UVs, the bump read as a normal map),
// Bridge (Photoshop / 3D-Coat) and Lighting & Toon (the light rig, the toon shader with its anisotropic highlight).
#pragma once
#include <QWidget>
#include <functional>

namespace hm {

class PaintSession;
class Viewport;
class Scene;

QWidget* makePaintLayersPanel(PaintSession* s, Viewport* vp);
// the reference: a picture beside the work (the author, 18.11 — the dress's concept was looked at in a messenger and a viewer) —
// opened, dropped or pasted; the wheel zooms, a drag moves it, a click takes its colour into the palette
QWidget* makePaintReferencePanel(PaintSession* s);
// listOut: the brush list (with its commands) made as a widget of its own — a column beside the settings; null: in the panel
// the notes of a panel: kept on it (its "help" property), shown by the "?" on its title — not as text in the panel (the author, 17.11)
void addHelp(QWidget* panel, const QString& text);
QString helpOf(QWidget* panel);                    // (the panel's notes and every part's of it, one paragraph each)
void exportPaintMaps(PaintSession* s, QWidget* parent);   // (every map that has layers, for every UV set, as PNGs into a folder)
std::function<int(int, int)> layerSizeAsk(PaintSession* s, Viewport* vp);   // (Layers from a file bigger than the texture: raise the texture / scale the file down / cancel → 1 / 0 / −1)
QWidget* makePaintBrushPanel(PaintSession* s, QWidget** listOut = nullptr);
QWidget* makePaintBakePanel(PaintSession* s, Viewport* vp, Scene* sc);
QWidget* makePaintBridgePanel(PaintSession* s, Viewport* vp, Scene* sc);
QWidget* makePaintLookPanel(PaintSession* s);

}  // namespace hm
