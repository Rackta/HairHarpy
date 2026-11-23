#version 450
// HairHarpy Toon's outline (the inverted hull): the mesh pushed out along its normals by the width in screen pixels (times the Outline
// width map), its back faces drawn in the line's colour (front faces culled). pc.params.yz — the pane's size in pixels.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 1, binding = 9) uniform sampler2D tOutline;
layout(std140, set = 1, binding = 12) uniform Params { vec4 v[32]; } PB;
layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 color;
  vec4 lightDir;
  vec4 params;
  vec4 extra;
} pc;

void main() {
  float w = PB.v[17].w;
  if (PB.v[16].y > 0.5) w *= textureLod(tOutline, inUV, 0.0).r;
  vec4 c = pc.viewProj * vec4(inPos, 1.0);
  if (PB.v[18].x < 0.5 || w <= 0.0 || dot(inNormal, inNormal) < 1e-12) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }   // (off: behind the far plane — nothing drawn)
  vec4 cn = pc.viewProj * vec4(inPos + normalize(inNormal) * 0.01, 1.0);
  vec2 d = cn.xy / cn.w - c.xy / c.w;
  if (dot(d, d) > 1e-14) d = normalize(d); else d = vec2(0.0);
  c.xy += d * w * 2.0 / max(pc.params.yz, vec2(1.0)) * c.w;
  gl_Position = c;
}
