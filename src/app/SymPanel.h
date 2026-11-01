#pragma once
class QWidget;

namespace hm {
class Viewport;
// Symmetry's settings as a popup by the button `anchor` (both rooms): on, the planes shown, the context, the type
// (planar / topology / radial), the axes, the space, Restrict, the tolerance; uv — the UV editor's own part under it
void openSymPanel(Viewport* vp, QWidget* anchor, bool uv);
}  // namespace hm
