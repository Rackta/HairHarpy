#version 450
// Textured meshes (an imported mesh's look: its texture sets). Positions are world space; uv already in the image's
// orientation (row 0 — the top: the file's flipY applied when the vertices were built).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 color;        // linear rgb, a — opacity
  vec4 lightDir;
  vec4 params;       // x — unlit, y — hemisphere, z — directional, w — flags (1 colour map, 2 opacity map, 4 colour map's alpha, 8 see-through, 16 orthographic)
  vec4 extra;        // xyz — the eye, w — PDO (as mesh.vert)
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vPos;

void main() {
  vNormal = inNormal;
  vUV = inUV;
  vPos = inPos;
  vec3 p = inPos;
  bool ortho = mod(floor(pc.params.w / 16.0), 2.0) > 0.5;
  // (PDO only where the mesh faces the camera: its far side stays at its depth — never pulled through what is in front of it)
  float pdo = pc.extra.w;
  if (pdo > 0.0 && dot(inNormal, inNormal) > 1e-12) {
    vec3 n = normalize(inNormal), toEye = ortho ? -normalize(vec3(pc.viewProj[0][2], pc.viewProj[1][2], pc.viewProj[2][2])) : normalize(pc.extra.xyz - inPos);
    pdo *= smoothstep(-0.1, 0.1, dot(n, toEye));
  }
  if (pdo > 0.0 && !ortho) p = mix(p, pc.extra.xyz, pdo);
  gl_Position = pc.viewProj * vec4(p, 1.0);
  if (pdo > 0.0 && ortho) gl_Position.z -= pdo * 0.5 * gl_Position.w;
}
