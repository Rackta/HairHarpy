#version 450
// Lines (curves, wireframes, the floor grid, the world axes): a colour per vertex. bias.x pulls the line toward the
// camera by that share of its distance (a curve lying on its wall's edge stays on top of it, as LINE_BIAS did).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 eye;          // camera position (w: 1 — perspective)
  vec4 bias;         // x — toward the camera, y — opacity multiplier, z — depth pulled forward (orthographic PDO)
  vec4 tint;         // a > 0: this colour instead of the vertices' (a mesh's own wire drawn as an overlay)
} pc;

layout(location = 0) out vec4 vColor;

void main() {
  vec3 p = inPos;
  if (pc.bias.x > 0.0) p = mix(p, pc.eye.xyz, pc.bias.x);
  vColor = pc.tint.a > 0.0 ? vec4(pc.tint.rgb, pc.tint.a * pc.bias.y) : vec4(inColor.rgb, inColor.a * pc.bias.y);
  gl_Position = pc.viewProj * vec4(p, 1.0);
  if (pc.bias.z > 0.0) gl_Position.z -= pc.bias.z * gl_Position.w;
}
