#version 450
#extension GL_GOOGLE_include_directive : enable
// HairHarpy Toon (the shader mode): toon_core.glsl around HairHarpy's own reads. Set 0: the mesh's own colour / opacity (as
// mesh_tex.frag); set 1: the UV set's toon maps (each optional — P[14..16] say which are there) and the instance's parameters.
// pc.params.y > 0.5: the over-the-hair pass — the brows / lashes drawn again over the hair, only where they were seen (the stencil)
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vPos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D mapTex;
layout(set = 0, binding = 1) uniform sampler2D alphaTex;
layout(set = 1, binding = 0) uniform sampler2D tBase;
layout(set = 1, binding = 1) uniform sampler2D tShade;
layout(set = 1, binding = 2) uniform sampler2D tMask;
layout(set = 1, binding = 3) uniform sampler2D tSpec;
layout(set = 1, binding = 4) uniform sampler2D tRim;
layout(set = 1, binding = 5) uniform sampler2D tEmis;
layout(set = 1, binding = 6) uniform sampler2D tNormal;
layout(set = 1, binding = 7) uniform sampler2D tFlow;
layout(set = 1, binding = 8) uniform sampler2D tSdf;
layout(set = 1, binding = 9) uniform sampler2D tOutline;
layout(set = 1, binding = 10) uniform sampler2D tOver;
layout(set = 1, binding = 11) uniform sampler2D tId;
layout(std140, set = 1, binding = 12) uniform Params { vec4 v[32]; } PB;

layout(push_constant) uniform PC {
  mat4 viewProj;
  vec4 color;
  vec4 lightDir;
  vec4 params;
  vec4 extra;
} pc;

#define float2 vec2
#define float3 vec3
#define float4 vec4
#define lerp mix
#define frac fract
#define saturate(x) clamp((x), 0.0, 1.0)
#include "toon_core.glsl"

void main() {
  int flags = int(pc.params.w + 0.5);
  vec4 h14 = PB.v[14], h15 = PB.v[15], h16 = PB.v[16];
  bool hasBase = h14.x > 0.5;
  // the base colour and its opacity: the painted Base colour where the set has one, else the mesh's own (as mesh_tex.frag)
  vec4 tex = hasBase ? texture(tBase, vUV) : ((flags & 1) != 0 ? texture(mapTex, vUV) : vec4(1.0));
  vec3 base = (hasBase ? tex.rgb : pc.color.rgb * tex.rgb) * PB.v[0].rgb;
  float a = pc.color.a;
  if ((flags & 4) != 0) a *= tex.a;
  if ((flags & 2) != 0 && !hasBase) a *= texture(alphaTex, vUV).g;
  if ((flags & 8) != 0 && a < 0.02) discard;
  if ((flags & 8) == 0) a = 1.0;

  vec3 N = normalize(vNormal); if (!gl_FrontFacing) N = -N;
  vec3 V = normalize(pc.extra.xyz - vPos);
  // the UV's directions on the surface (the cotangent frame: from the derivatives — no tangents stored): Tu along U, Bd down the picture
  vec3 dp1 = dFdx(vPos), dp2 = dFdy(vPos); vec2 du1 = dFdx(vUV), du2 = dFdy(vUV);
  vec3 p2 = cross(dp2, N), p1 = cross(N, dp1);
  vec3 Tu = p2 * du1.x + p1 * du2.x, Bd = p2 * du1.y + p1 * du2.y;
  float im = inversesqrt(max(max(dot(Tu, Tu), dot(Bd, Bd)), 1e-20)); Tu *= im; Bd *= im;
  // the normal map (OpenGL's: green toward the picture's top)
  vec3 n = N;
  if (h15.z > 0.5 && PB.v[11].y > 0.5) {
    vec3 t = texture(tNormal, vUV).xyz * 2.0 - 1.0; t.xy *= PB.v[11].z;
    n = normalize(Tu * t.x - Bd * t.y + N * max(t.z, 1e-3));
  }
  // the strands' direction: the Flow map where painted, else along V or U
  vec3 T = vec3(0.0);
  int axis = int(PB.v[21].x + 0.5);
  if (PB.v[20].x > 0.5) {
    if (axis == 0 && h15.w > 0.5) { vec4 f = texture(tFlow, vUV); vec2 d = f.xy * 2.0 - 1.0; if (f.a > 0.05 && dot(d, d) > 1e-4) T = Tu * d.x + Bd * d.y; else T = Bd; }
    else T = axis == 2 ? Tu : Bd;
    T = T - n * dot(T, n); if (dot(T, T) > 1e-8) T = normalize(T);
  }
  vec4 shadeMap = h14.y > 0.5 ? texture(tShade, vUV) : vec4(0.0);
  float mask = h14.z > 0.5 ? texture(tMask, vUV).r : 0.5;
  float specMask = h14.w > 0.5 ? texture(tSpec, vUV).r : 1.0;
  float rimMask = h15.x > 0.5 ? texture(tRim, vUV).r : 1.0;
  vec3 emis = h15.y > 0.5 ? texture(tEmis, vUV).rgb : vec3(0.0);
  float sdfA = h16.x > 0.5 ? texture(tSdf, vUV).r : 0.5, sdfB = h16.x > 0.5 ? texture(tSdf, vec2(1.0 - vUV.x, vUV.y)).r : 0.5;
  vec3 idMap = h16.w > 0.5 ? texture(tId, vUV).rgb : vec3(0.0);
  vec3 L = normalize(pc.lightDir.xyz);
  vec3 col = hh_toon(base, n, L, V, T, shadeMap, mask, specMask, rimMask, emis, sdfA, sdfB, idMap, PB.v);
  if (pc.params.y > 0.5) {                                       // (over the hair: this much)
    float om = h16.z > 0.5 ? texture(tOver, vUV).r : 1.0;
    a *= hh_overHair(V, om, PB.v); if (a < 0.01) discard;
  }
  outColor = vec4(col, a);
}
