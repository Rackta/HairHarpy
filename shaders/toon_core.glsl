// HairHarpy Toon — the shading core. ONE text for every place the shader goes: HairHarpy (Vulkan GLSL), the website (WebGL2 GLSL ES),
// Unreal (a Custom node's HLSL), Unity (URP HLSL). Written in a neutral dialect — float3 / lerp / saturate / frac, every vector built with
// all its components — and made GLSL by a prelude of #defines (toon.frag, the web shader) or used as HLSL as it is. Nothing here samples
// a texture: the maps are read by the shader around it and handed in, so each engine keeps its own samplers and its own normals.
// P: the parameters, 32 float4 in scene/Toon.cpp's order (ubo()).

float3 hh_rgb2hsv(float3 c) {
  float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
  float4 p = lerp(float4(c.b, c.g, K.w, K.z), float4(c.g, c.b, K.x, K.y), step(c.b, c.g));
  float4 q = lerp(float4(p.x, p.y, p.w, c.r), float4(c.r, p.y, p.z, p.x), step(p.x, c.r));
  float d = q.x - min(q.w, q.y);
  return float3(abs(q.z + (q.w - q.y) / (6.0 * d + 1.0e-10)), d / (q.x + 1.0e-10), q.x);
}
float3 hh_hsv2rgb(float3 c) {
  float3 p = abs(frac(float3(c.x + 1.0, c.x + 2.0 / 3.0, c.x + 1.0 / 3.0)) * 6.0 - float3(3.0, 3.0, 3.0));
  return c.z * lerp(float3(1.0, 1.0, 1.0), saturate(p - float3(1.0, 1.0, 1.0)), c.y);
}

// base: the lit colour (linear) · n, L, V: the shading normal, to the light, to the eye (world, unit) · T: along the strands (unit;
// zero length — none) · shadeMap: the painted shade (a — where painted) · mask, specMask, rimMask: the gray maps (their plain values
// where there is no map) · emis: the Emission map · sdfA / sdfB: the face SDF read at the UV and at the mirrored U · idMap: the
// Material ID map's colour
float3 hh_toon(float3 base, float3 n, float3 L, float3 V, float3 T, float4 shadeMap, float mask, float specMask, float rimMask,
               float3 emis, float sdfA, float sdfB, float3 idMap, float4 P[32]) {
  // the light's place on the surface (half Lambert, 0 — away, 1 — facing it), moved by the Shadow mask
  float hl = saturate(dot(n, L) * 0.5 + 0.5 + (mask - 0.5) * P[5].w);
  // the face shadow from its SDF: the light's turn about the head (the locator) against the picture
  if (P[25].x > 0.5 && P[22].w > 0.5 && P[16].x > 0.5) {
    float3 F = P[22].xyz; float3 R = P[23].xyz; float3 U = P[24].xyz;
    float3 Lh = L - U * dot(L, U); float lh = length(Lh);
    if (lh > 1.0e-4) {
      Lh = Lh / lh;
      float front = dot(Lh, F), side = dot(Lh, R);
      float thr = acos(clamp(front, -1.0, 1.0)) / 3.14159265;           // (0 — straight in front, 1 — straight behind)
      float s = (side >= 0.0) != (P[25].z > 0.5) ? sdfA : sdfB;
      hl = smoothstep(thr - P[25].y, thr + P[25].y, s) * 0.9 + 0.05;
    }
  }
  float term = P[1].x; float shad = P[1].y; float sft = max(P[1].z, 1.0e-4);
  float litK = smoothstep(term - sft, term + sft, hl);
  float pen = smoothstep(shad - sft, shad + sft, hl);
  // the shade: made from the base colour (hue turned, saturation and value changed, tinted), the painted one over it where painted
  float3 h = hh_rgb2hsv(max(base, float3(0.0, 0.0, 0.0)));
  h.x = frac(h.x + P[5].x); h.y = saturate(h.y * P[5].y); h.z = h.z * P[5].z;
  float3 shade = hh_hsv2rgb(h) * lerp(float3(1.0, 1.0, 1.0), P[4].xyz, P[7].w);
  shade = lerp(shade, shadeMap.xyz, shadeMap.w);
  // the Material ID's zones: each tints the shadow its own way (the strongest of red, green, blue — or the rest)
  if (P[27].x > 0.5 && P[16].w > 0.5) {
    float3 zt = P[31].xyz; float best = 0.5;
    if (idMap.x > best) { best = idMap.x; zt = P[28].xyz; }
    if (idMap.y > best) { best = idMap.y; zt = P[29].xyz; }
    if (idMap.z > best) { best = idMap.z; zt = P[30].xyz; }
    shade = shade * zt;
  }
  float3 penC = lerp(shade, base, P[4].w);
  float3 col = lerp(shade, penC, pen);
  col = lerp(col, base, litK);
  // the terminator line
  if (P[3].y > 0.5) { float band = 1.0 - smoothstep(0.0, max(P[3].x, 1.0e-4), abs(hl - term)); col = lerp(col, P[2].xyz, band * P[2].w); }
  float3 H = normalize(L + V);
  // the highlight
  if (P[7].z > 0.5) {
    float th = 1.0 - P[7].x * 0.5; float ss = max(P[7].y, 1.0e-4);
    col = col + P[6].xyz * (smoothstep(th - ss, th + ss, max(dot(n, H), 0.0)) * specMask * P[6].w);
  }
  // the hair's band (Kajiya-Kay along the strands; a second band shifted along them)
  if (P[20].x > 0.5 && dot(T, T) > 1.0e-6) {
    float3 T1 = normalize(T + n * P[20].z); float3 T2 = normalize(T + n * (P[20].z + P[21].y));
    float th1 = dot(T1, H); float th2 = dot(T2, H);
    float k1 = pow(sqrt(max(1.0 - th1 * th1, 0.0)), P[20].y); float k2 = pow(sqrt(max(1.0 - th2 * th2, 0.0)), P[20].y * 0.5);
    float band = (k1 + k2 * P[20].w) * smoothstep(0.0, 0.2, dot(n, L));
    col = col + P[19].xyz * (band * specMask * P[19].w);
  }
  // the rim
  if (P[9].z > 0.5) {
    float fr = 1.0 - max(dot(n, V), 0.0); float th = 1.0 - P[9].x; float ss = max(P[9].y, 1.0e-4);
    float r = smoothstep(th - ss, th + ss, fr) * rimMask;
    if (P[9].w > 0.5) r = r * litK;
    col = col + P[8].xyz * (r * P[8].w);
  }
  // the light and the ambient; what glows by itself
  col = col * P[12].x + base * P[13].xyz * P[12].y;
  if (P[11].x > 0.5) col = col + emis * P[10].xyz * P[10].w;
  return col;
}

// the over-the-hair strength of a part (brows, lashes) by how squarely the face looks at the camera (the head locator; none — always)
float hh_overHair(float3 toEye, float mapValue, float4 P[32]) {
  float f = 1.0;
  if (P[22].w > 0.5) f = smoothstep(P[26].y, P[26].z, dot(P[22].xyz, toEye));
  return saturate(P[26].x * mapValue * f);
}
