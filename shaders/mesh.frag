#version 450
// The HTML version's look: MeshStandardMaterial (rough) under a hemisphere light (sky white, ground #3a3a3a) and a
// directional light that follows the camera. Lambert only (the faint specular of roughness 0.75 is left out), linear
// colour into an sRGB swapchain. Both sides lit (DoubleSide). Two-tone (the Topology room's active mesh in a component
// mode): purple in front (#9e6bc4), orange behind (#e38530) — the polygon's own side.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vTint;
layout(location = 2) in vec3 vPos;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 color;
  vec4 lightDir;
  vec4 params;
  vec4 extra;
} pc;

const vec3 SKY = vec3(1.0);
const vec3 GROUND = vec3(0.0423);   // #3a3a3a in linear
const float INV_PI = 0.31830988618;
const vec3 TFRONT = vec3(0.3419, 0.1470, 0.5520), TBACK = vec3(0.7681, 0.2346, 0.0296);

// the material (the Topology room's mesh settings: lightDir.w = 1 + roughness·255 + metal·255·256 + specular·255·65536,
// 0 — none: the plain rough look): GGX highlights from the camera's light, metal tinting them and dimming the diffuse,
// the sky / ground reflected a little
vec3 shadeLit(vec3 base, vec3 n, vec3 pos) {
  vec3 L = normalize(pc.lightDir.xyz);
  vec3 hemi = mix(GROUND, SKY, 0.5 * n.y + 0.5) * pc.params.y;
  float ndl = max(dot(n, L), 0.0);
  if (pc.lightDir.w < 0.5) return base * INV_PI * (hemi + vec3(ndl * pc.params.z));
  float code = pc.lightDir.w - 1.0;
  float r = mod(code, 256.0) / 255.0, m = mod(floor(code / 256.0), 256.0) / 255.0, s = floor(code / 65536.0) / 255.0;
  vec3 v = normalize(pc.extra.xyz - pos); float ndv = max(dot(n, v), 1e-3);
  if (m < 0.5) {
    // a sculpting clay (not a metal): the light wrapped round the forms, the forms darker towards their edges (the folds
    // and crevices read as in a sculpting program), a broad soft highlight, a faint rim
    float wrap = clamp((dot(n, L) + 0.4) / 1.4, 0.0, 1.0);
    vec3 hemiC = mix(vec3(0.030, 0.026, 0.022), vec3(1.0, 0.97, 0.92), 0.5 * n.y + 0.5) * pc.params.y;
    float edge = mix(0.5, 1.0, pow(ndv, 0.55));
    vec3 hh = normalize(L + v);
    float sp = pow(max(dot(n, hh), 0.0), mix(6.0, 48.0, 1.0 - r)) * s * 0.30 * pc.params.z;
    float rim = pow(1.0 - ndv, 3.0) * 0.06 * pc.params.y;
    return base * INV_PI * (hemiC * 0.55 + vec3(wrap * wrap * 1.35 * pc.params.z)) * edge + vec3(sp) + base * rim;
  }
  vec3 h = normalize(L + v); float ndh = max(dot(n, h), 0.0);
  float a = max(r * r, 0.002), a2 = a * a, d = ndh * ndh * (a2 - 1.0) + 1.0, D = a2 / (3.14159265 * d * d);
  vec3 F0 = mix(vec3(0.04 * s), base, m);
  vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(h, v), 0.0), 5.0);
  float k = (r + 1.0) * (r + 1.0) / 8.0, G = (ndl / (ndl * (1.0 - k) + k)) * (ndv / (ndv * (1.0 - k) + k));
  vec3 spec = D * F * G / (4.0 * max(ndl, 1e-3) * ndv) * ndl * pc.params.z * (m > 0.0 ? 1.0 : s);
  vec3 R = reflect(-v, n);
  vec3 env = (F0 + (1.0 - F0) * pow(1.0 - ndv, 5.0) * (1.0 - r)) * mix(GROUND, SKY, 0.5 * R.y + 0.5) * pc.params.y * (0.15 + 0.35 * (1.0 - r));
  return base * (1.0 - m) * INV_PI * (hemi + vec3(ndl * pc.params.z)) + spec + env;
}

void main() {
  vec3 base = pc.color.rgb * vTint;
  if (mod(pc.params.w, 2.0) > 0.5) base = gl_FrontFacing ? TFRONT : TBACK;
  if (pc.params.x > 0.5) { outColor = vec4(base, pc.color.a); return; }
  vec3 n = normalize(vNormal);
  if (!gl_FrontFacing) n = -n;
  outColor = vec4(shadeLit(base, n, vPos), pc.color.a);   // (three r160, physical lights: BRDF_Lambert × irradiance)
}