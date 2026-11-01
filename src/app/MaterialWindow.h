// The material window (double click on a mesh's material picture): a ready preset, one's own presets saved / deleted,
// a preset exported to a file / imported from one (this program's, or a material of another program: Wavefront .mtl,
// glTF, Unity .mat, Godot .tres, three.js JSON), and the material's own settings — colour, roughness, metalness,
// specular. The changes show at once; OK keeps them (one undo step), Cancel puts back what was.
#pragma once
#include <QRect>

namespace hm {

class Viewport;

void openMaterialWindow(Viewport* vp, int id);
// where the material / texture windows open: over the viewport's top right (size s, kept on the screen)
QRect viewportZone(Viewport* vp, const QSize& s);

}  // namespace hm
