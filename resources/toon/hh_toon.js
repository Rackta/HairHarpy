// HairHarpy Toon for three.js (r160+, WebGL2) — the same shader as HairHarpy's shader mode: toon_core.glsl, the parameters of every
// object's UV set from hh_toon.json (packed: 32 vec4, P), the maps of each UV set. Written by HairHarpy's Materials & Shader → Export.
//
//   import * as THREE from 'three';
//   import { loadHHToon } from './hh_toon.js';
//   const renderer = new THREE.WebGLRenderer({ antialias: true, stencil: true });       // (stencil: the brows over the hair)
//   const hh = await loadHHToon(THREE, 'export-folder-url');      // a glTF model (Blender's export): loadHHToon(THREE, url, { gltf: true })
//   hh.applyTo(model);                         // a glTF / FBX scene: meshes named as HairHarpy's objects, materials named as the UV sets
//   hh.setLight(new THREE.Vector3(0.4, 0.8, 0.5));       // to the light, world space (or hh.followLight(directionalLight) every frame)
//   hh.setHead(headBone);                      // optional: the head's Object3D — its forward (+Z) / up (+Y) drive the face SDF and the
//                                              // brows over the hair; without it the export's head locator (the rest pose) is used
// Roles (hh_toon.json parts[].role): "over" — drawn again over the hair where it was seen (the stencil); "hair" — the hair.

const PRELUDE = `
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define lerp mix
#define frac fract
#define saturate(x) clamp((x), 0.0, 1.0)
`;

const VERT = `
#include <common>
#include <skinning_pars_vertex>
uniform float uFlipV;
varying vec3 vN; varying vec2 vUV; varying vec3 vPos;
void main() {
  #include <skinbase_vertex>
  #include <begin_vertex>
  #include <beginnormal_vertex>
  #include <skinnormal_vertex>
  #include <skinning_vertex>
  vec4 wp = modelMatrix * vec4(transformed, 1.0);
  vPos = wp.xyz; vN = normalize(mat3(modelMatrix) * objectNormal); vUV = vec2(uv.x, uFlipV > 0.5 ? 1.0 - uv.y : uv.y);   // (the maps' rows top-down, flipY off: an FBX's V turned, a glTF's as it is)
  gl_Position = projectionMatrix * viewMatrix * wp;
}`;

const OUTLINE_VERT = `
#include <common>
#include <skinning_pars_vertex>
uniform vec4 P[32]; uniform sampler2D tOutline; uniform vec2 uView; uniform float uFlipV;
varying vec2 vUV;
void main() {
  #include <skinbase_vertex>
  #include <begin_vertex>
  #include <beginnormal_vertex>
  #include <skinnormal_vertex>
  #include <skinning_vertex>
  vec3 pos = transformed, nrm = objectNormal;
  vUV = vec2(uv.x, uFlipV > 0.5 ? 1.0 - uv.y : uv.y);
  float w = P[17].w; if (P[16].y > 0.5) w *= textureLod(tOutline, vUV, 0.0).r;
  vec4 c = projectionMatrix * modelViewMatrix * vec4(pos, 1.0);
  vec4 cn = projectionMatrix * modelViewMatrix * vec4(pos + normalize(nrm) * 0.01, 1.0);
  vec2 d = cn.xy / cn.w - c.xy / c.w; d = dot(d, d) > 1e-14 ? normalize(d) : vec2(0.0);
  if (P[18].x < 0.5) w = 0.0;
  c.xy += d * w * 2.0 / max(uView, vec2(1.0)) * c.w;
  gl_Position = c;
}`;
const OUTLINE_FRAG = `uniform vec4 P[32]; void main() { gl_FragColor = vec4(P[17].rgb, 1.0); }`;

function fragment(core) {
  return PRELUDE + `
uniform vec4 P[32]; uniform vec3 uLight; uniform float uOverPass; uniform float uUseAlpha; uniform vec3 uBaseColor;
uniform sampler2D tBase, tShade, tMask, tSpec, tRim, tEmis, tNormal, tFlow, tSdf, tOutline, tOver, tId;
varying vec3 vN; varying vec2 vUV; varying vec3 vPos;
` + core + `
void main() {
  vec4 h14 = P[14], h15 = P[15], h16 = P[16];
  vec4 tex = h14.x > 0.5 ? texture(tBase, vUV) : vec4(uBaseColor, 1.0);
  // (the map's alpha only on a see-through part, as HairHarpy: a solid part's map is 0 outside the shells — no holes from its mips)
  vec3 base = tex.rgb * P[0].rgb; float a = uUseAlpha > 0.5 ? tex.a : 1.0; if (a < 0.02) discard;
  vec3 N = normalize(vN); if (!gl_FrontFacing) N = -N;
  vec3 V = normalize(cameraPosition - vPos);
  vec3 dp1 = dFdx(vPos), dp2 = dFdy(vPos); vec2 du1 = dFdx(vUV), du2 = dFdy(vUV);
  vec3 p2 = cross(dp2, N), p1 = cross(N, dp1);
  vec3 Tu = p2 * du1.x + p1 * du2.x, Bd = p2 * du1.y + p1 * du2.y;
  float im = inversesqrt(max(max(dot(Tu, Tu), dot(Bd, Bd)), 1e-20)); Tu *= im; Bd *= im;
  vec3 n = N;
  if (h15.z > 0.5 && P[11].y > 0.5) { vec3 t = texture(tNormal, vUV).xyz * 2.0 - 1.0; t.xy *= P[11].z; n = normalize(Tu * t.x - Bd * t.y + N * max(t.z, 1e-3)); }
  vec3 T = vec3(0.0); int axis = int(P[21].x + 0.5);
  if (P[20].x > 0.5) {
    if (axis == 0 && h15.w > 0.5) { vec4 f = texture(tFlow, vUV); vec2 d = f.xy * 2.0 - 1.0; T = (f.a > 0.05 && dot(d, d) > 1e-4) ? Tu * d.x + Bd * d.y : Bd; }
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
  vec3 col = hh_toon(base, n, normalize(uLight), V, T, shadeMap, mask, specMask, rimMask, emis, sdfA, sdfB, idMap, P);
  if (uOverPass > 0.5) { a *= hh_overHair(V, h16.z > 0.5 ? texture(tOver, vUV).r : 1.0, P); if (a < 0.01) discard; }
  gl_FragColor = vec4(col, a);
  #include <colorspace_fragment>
}`;
}

export async function loadHHToon(THREE, baseUrl, options = {}) {
  const lib = await (await fetch(baseUrl + '/hh_toon.json')).json();
  const core = await (await fetch(baseUrl + '/' + (lib.core || 'toon_core.glsl'))).text();
  return new HHToon(THREE, lib, core, baseUrl, options);
}

export class HHToon {
  constructor(THREE, lib, core, baseUrl, options = {}) {
    this.flipV = options.gltf ? 0 : 1;   // (a glTF keeps its V top-down already; an FBX / OBJ has it bottom-up)
    this.THREE = THREE; this.lib = lib; this.core = core; this.base = baseUrl;
    this.loader = new THREE.TextureLoader(); this.tex = new Map(); this.mats = [];
    this.light = new THREE.Vector3(0.4, 0.8, 0.5).normalize(); this.view = new THREE.Vector2(1280, 720);
    const white = new Uint8Array([255, 255, 255, 255]); this.white = new THREE.DataTexture(white, 1, 1); this.white.needsUpdate = true;
  }
  texture(file, srgb) {
    if (!file) return this.white;
    if (!this.tex.has(file)) { const t = this.loader.load(this.base + '/' + file); t.flipY = false; t.wrapS = t.wrapT = this.THREE.RepeatWrapping;
      t.colorSpace = srgb ? this.THREE.SRGBColorSpace : this.THREE.NoColorSpace; this.tex.set(file, t); }
    return this.tex.get(file);
  }
  part(objectName, set) { return this.lib.parts.find(p => p.object === objectName && p.set === set) || this.lib.parts.find(p => p.set === set); }
  uniforms(part) {
    const T = this.THREE, maps = (this.lib.sets[part.set] || {}).maps || {}, slots = this.lib.slots, srgb = this.lib.srgb;
    const P = []; for (let i = 0; i < 32; i++) P.push(new T.Vector4(part.P[i * 4], part.P[i * 4 + 1], part.P[i * 4 + 2], part.P[i * 4 + 3]));
    const names = ['tBase', 'tShade', 'tMask', 'tSpec', 'tRim', 'tEmis', 'tNormal', 'tFlow', 'tSdf', 'tOutline', 'tOver', 'tId'];
    const u = { P: { value: P }, uLight: { value: this.light }, uOverPass: { value: 0 }, uUseAlpha: { value: part.transparent ? 1 : 0 }, uBaseColor: { value: new T.Color(1, 1, 1) }, uView: { value: this.view }, uFlipV: { value: this.flipV } };
    names.forEach((n, i) => { const f = i === 8 ? part.sdf : maps[slots[i]]; u[n] = { value: this.texture(f, srgb[i]) }; });
    return u;
  }
  material(part, overPass = false) {
    const T = this.THREE;
    const m = new T.ShaderMaterial({ glslVersion: T.GLSL3, uniforms: this.uniforms(part), vertexShader: VERT, fragmentShader: fragment(this.core), side: T.DoubleSide, transparent: overPass || !!part.transparent });
    m.uniforms.uOverPass.value = overPass ? 1 : 0;
    if (part.role === 'over' && !overPass) { m.stencilWrite = true; m.stencilRef = 1; m.stencilFunc = T.AlwaysStencilFunc; m.stencilZPass = T.ReplaceStencilOp; }
    else if (part.role === 'hair' && !overPass) { m.stencilWrite = true; m.stencilRef = 2; m.stencilWriteMask = 2; m.stencilFunc = T.AlwaysStencilFunc; m.stencilZPass = T.ReplaceStencilOp; }
    else if (!overPass) { m.stencilWrite = true; m.stencilRef = 0; m.stencilFunc = T.AlwaysStencilFunc; m.stencilZPass = T.ReplaceStencilOp; }
    // (bit 0 — the over part seen, bit 1 — hair in front of it: drawn again only where both are)
    if (overPass) { m.depthTest = false; m.depthWrite = false; m.stencilWrite = true; m.stencilRef = 3; m.stencilFuncMask = 3; m.stencilFunc = T.EqualStencilFunc; m.stencilZPass = T.KeepStencilOp; m.stencilWriteMask = 0; }
    this.mats.push(m); return m;
  }
  outline(part) {
    const T = this.THREE, u = this.uniforms(part);
    const m = new T.ShaderMaterial({ glslVersion: T.GLSL3, uniforms: u, vertexShader: OUTLINE_VERT, fragmentShader: OUTLINE_FRAG, side: T.BackSide });
    this.mats.push(m); return m;
  }
  // every mesh of the model: its material (by the UV set's name) made a HairHarpy Toon material; the outline and the over-the-hair copy added
  applyTo(root) {
    const extra = [];
    root.traverse(o => {
      if (!o.isMesh) return;
      const list = Array.isArray(o.material) ? o.material : [o.material];
      const made = list.map(m => { const part = this.part(o.name, m && m.name); return part ? { part, mat: this.material(part) } : { part: null, mat: m }; });
      o.material = Array.isArray(o.material) ? made.map(x => x.mat) : made[0].mat;
      const order = { over: 0, '': 1, hair: 2 };
      const first = made.find(x => x.part); if (!first) return;
      o.renderOrder = order[first.part.role || ''] ?? 1;
      if (first.part.P[18 * 4] > 0.5) extra.push([o, this.outline(first.part), 3]);
      if (first.part.role === 'over') extra.push([o, this.material(first.part, true), 10]);
    });
    const T = this.THREE;
    for (const [o, mat, order] of extra) {
      let c;
      if (o.isSkinnedMesh) { c = new T.SkinnedMesh(o.geometry, mat); c.bind(o.skeleton, o.bindMatrix); c.position.copy(o.position); c.quaternion.copy(o.quaternion); c.scale.copy(o.scale); o.parent.add(c); }
      else { c = new T.Mesh(o.geometry, mat); o.add(c); }
      c.renderOrder = order; c.frustumCulled = o.frustumCulled;
    }
  }
  setLight(dir) { this.light.copy(dir).normalize(); }
  followLight(directional) { this.light.copy(directional.position).sub(directional.target.position).normalize(); }
  setViewSize(w, h) { this.view.set(w, h); }
  // the head's axes from a bone / an object: forward +Z, up +Y (as HairHarpy's locator)
  setHead(obj) {
    const T = this.THREE, q = obj.getWorldQuaternion(new T.Quaternion());
    const f = new T.Vector3(0, 0, 1).applyQuaternion(q), u = new T.Vector3(0, 1, 0).applyQuaternion(q), r = new T.Vector3().crossVectors(u, f);
    for (const m of this.mats) { const P = m.uniforms.P.value; P[22].set(f.x, f.y, f.z, 1); P[23].set(r.x, r.y, r.z, 0); P[24].set(u.x, u.y, u.z, 0); }
  }
}
