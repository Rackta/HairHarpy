#version 450
// A texture set (the HTML version's lookMats): colour × colour map; opacity from the opacity map (its green channel, as
// three.js reads an alphaMap) and / or the colour map's alpha; see-through below 0.02 dropped (alphaTest). Lit as mesh.frag
// (rough standard material) or unlit (Flat — MeshBasicMaterial).
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vPos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D mapTex;
layout(set = 0, binding = 1) uniform sampler2D alphaTex;

layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 color;
  vec4 lightDir;
  vec4 params;
  vec4 extra;
} pc;

const vec3 SKY = vec3(1.0);
const vec3 GROUND = vec3(0.0423);
const float INV_PI = 0.31830988618;

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
  if (m < 0.5) {   // (a sculpting clay, as mesh.frag: wrapped light, darker towards the forms' edges, a broad soft highlight)
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
  int flags = int(pc.params.w + 0.5);
  vec4 tex = (flags & 1) != 0 ? texture(mapTex, vUV) : vec4(1.0);
  vec3 base = pc.color.rgb * tex.rgb;
  float a = pc.color.a;
  if ((flags & 4) != 0) a *= tex.a;
  if ((flags & 2) != 0) a *= texture(alphaTex, vUV).g;
  if ((flags & 8) != 0 && a < 0.02) discard;
  if ((flags & 8) == 0) a = 1.0;
  if (pc.params.x > 0.5) { outColor = vec4(base, a); return; }
  vec3 n = normalize(vNormal);
  if (!gl_FrontFacing) n = -n;
  outColor = vec4(shadeLit(base, n, vPos), a);
}
