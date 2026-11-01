#version 450
// Anchor points: size and shape per point (0 — a circle, 1 — a diamond: a curve's first anchor, 2 — a hollow ring: its
// last one), drawn over everything (no depth test), as anchorMat did.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;    // rgb, a — size in pixels
layout(location = 2) in float inShape;

layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 params;       // x — device pixel ratio, y — opacity, z — orthographic, w — the viewport's height in pixels
  vec4 eye;          // xyz — the eye, w — PDO (the Topology room's overlays)
} pc;

layout(location = 0) out vec3 vColor;
layout(location = 1) out float vShape;

void main() {
  vColor = inColor.rgb; vShape = inShape;
  vec3 p = inPos;
  if (pc.eye.w > 0.0 && pc.params.z < 0.5) p = mix(p, pc.eye.xyz, pc.eye.w);
  gl_Position = pc.viewProj * vec4(p, 1.0);
  if (pc.eye.w > 0.0 && pc.params.z > 0.5) gl_Position.z -= pc.eye.w * 0.5 * gl_Position.w;
  gl_PointSize = inColor.a * pc.params.x;
  // (3, 4 — a Weld locks pin: the sprite moved up by half its size, its tip on the place; Vulkan's y runs down)
  if (inShape > 2.5 && pc.params.w > 0.0) gl_Position.y -= gl_PointSize / pc.params.w * gl_Position.w;
}
