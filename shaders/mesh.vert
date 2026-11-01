#version 450
// Lit meshes (the head, imported and converted meshes). Positions are world space (the model matrix is identity for now).
// PDO (the Topology room's polygon draw offset): the mesh drawn nearer the camera than it is — perspective: moved toward
// the eye by that share of the distance (it stays put on the screen); orthographic: its depth pulled forward.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTint;     // per vertex (a modifier's painted weights), else white
layout(location = 3) in vec3 inGeo;      // the polygon's own normal where inNormal is the file's (smoothed, turned), else zero

layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 color;        // linear rgb, a — opacity
  vec4 lightDir;     // world, toward the light
  vec4 params;       // x — unlit, y — hemisphere intensity, z — directional intensity, w — flags (1 two-tone, 2 orthographic)
  vec4 extra;        // xyz — the eye, w — PDO
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vTint;
layout(location = 2) out vec3 vPos;

void main() {
  vNormal = inNormal;
  vTint = inTint;
  vPos = inPos;
  vec3 p = inPos;
  bool ortho = mod(floor(pc.params.w / 2.0), 2.0) > 0.5;
  // (the selection's halo, flag 64: the mesh blown up along its normals by params.y × its distance (orthographic: params.z) —
  // a few pixels — and pushed back from the eye by twice that: the mesh itself hides it but round its outline)
  if (mod(floor(pc.params.w / 64.0), 2.0) > 0.5) {
    vec3 away = ortho ? normalize(pc.lightDir.xyz) : normalize(p - pc.extra.xyz); float w = ortho ? pc.params.z : pc.params.y * length(p - pc.extra.xyz);
    p += normalize(inNormal) * w + away * max(w * 3.0, (ortho ? w * 40.0 : length(p - pc.extra.xyz) * 0.006));   // (well behind: the meshes' own depth bias not to bring it in front)
  }
  // (PDO only where the mesh faces the camera: its far side stays at its depth — never pulled through what is in front of it;
  // the side the polygon's own, not a smoothed normal's)
  float pdo = pc.extra.w;
  const vec3 side = dot(inGeo, inGeo) > 1e-12 ? inGeo : inNormal;
  if (pdo > 0.0 && dot(side, side) > 1e-12) {
    vec3 n = normalize(side), toEye = ortho ? -normalize(vec3(pc.viewProj[0][2], pc.viewProj[1][2], pc.viewProj[2][2])) : normalize(pc.extra.xyz - inPos);
    pdo *= smoothstep(-0.1, 0.1, dot(n, toEye));
  }
  if (pdo > 0.0 && !ortho) p = mix(p, pc.extra.xyz, pdo);
  gl_Position = pc.viewProj * vec4(p, 1.0);
  if (pdo > 0.0 && ortho) gl_Position.z -= pdo * 0.5 * gl_Position.w;
}