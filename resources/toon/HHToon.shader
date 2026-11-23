// HairHarpy Toon for Unity (URP 14+) — the same shader as HairHarpy's shader mode: hh_toon_core.hlsl (toon_core.glsl, unchanged),
// the parameters packed as HairHarpy packs them (_P00…_P31), the maps of the UV set. Made by Editor/HHToonImporter.cs from hh_toon.json.
// Roles: Over the hair — writes stencil 1 (queue 2000) and gets a second material (queue 3100, no depth test, drawn where the stencil is 3:
// the part seen and hair in front of it); hair — sets the stencil's bit 2 (queue 2002); the rest — writes 0 (queue 2001).
Shader "HairHarpy/Toon"
{
  Properties
  {
    _BaseColorTex ("Base colour", 2D) = "white" {}
    _ShadeTex ("Shade colour", 2D) = "black" {}
    _ShadowMaskTex ("Shadow mask", 2D) = "gray" {}
    _SpecularTex ("Highlight mask", 2D) = "white" {}
    _RimTex ("Rim mask", 2D) = "white" {}
    _EmissionTex ("Emission", 2D) = "black" {}
    _NormalTex ("Normal (OpenGL, linear)", 2D) = "bump" {}
    _FlowTex ("Flow", 2D) = "gray" {}
    _SdfTex ("Face SDF", 2D) = "gray" {}
    _OutlineTex ("Outline width", 2D) = "white" {}
    _OverHairTex ("Over hair", 2D) = "white" {}
    _IdTex ("Material ID", 2D) = "black" {}
    _P00 ("P00", Vector) = (1,1,1,0) _P01 ("P01", Vector) = (0.55,0.35,0.015,0) _P02 ("P02", Vector) = (0,0,0,0) _P03 ("P03", Vector) = (0,0,0,0)
    _P04 ("P04", Vector) = (0,0,0,0) _P05 ("P05", Vector) = (0,1,1,0.5) _P06 ("P06", Vector) = (0,0,0,0) _P07 ("P07", Vector) = (0,0,0,0)
    _P08 ("P08", Vector) = (0,0,0,0) _P09 ("P09", Vector) = (0,0,0,0) _P10 ("P10", Vector) = (0,0,0,0) _P11 ("P11", Vector) = (0,0,0,0)
    _P12 ("P12", Vector) = (1,0,0,0) _P13 ("P13", Vector) = (0,0,0,0) _P14 ("P14", Vector) = (0,0,0,0) _P15 ("P15", Vector) = (0,0,0,0)
    _P16 ("P16", Vector) = (0,0,0,0) _P17 ("P17", Vector) = (0,0,0,1.5) _P18 ("P18", Vector) = (0,0,0,0) _P19 ("P19", Vector) = (0,0,0,0)
    _P20 ("P20", Vector) = (0,0,0,0) _P21 ("P21", Vector) = (0,0,0,0) _P22 ("P22", Vector) = (0,0,1,0) _P23 ("P23", Vector) = (1,0,0,0)
    _P24 ("P24", Vector) = (0,1,0,0) _P25 ("P25", Vector) = (0,0,0,0) _P26 ("P26", Vector) = (1,0.15,0.55,0) _P27 ("P27", Vector) = (0,0,0,0)
    _P28 ("P28", Vector) = (1,1,1,0) _P29 ("P29", Vector) = (1,1,1,0) _P30 ("P30", Vector) = (1,1,1,0) _P31 ("P31", Vector) = (1,1,1,0)
    [HideInInspector] _OverPass ("Over pass", Float) = 0
    [HideInInspector] _UseAlpha ("Use the map's alpha (see-through sets)", Float) = 0
    [HideInInspector] _StencilRef ("Stencil ref", Float) = 0
    [HideInInspector] _StencilComp ("Stencil comp", Float) = 8
    [HideInInspector] _StencilPass ("Stencil pass", Float) = 2
    [HideInInspector] _StencilWrite ("Stencil write mask", Float) = 255
    [HideInInspector] _StencilRead ("Stencil read mask", Float) = 255
    [HideInInspector] _ZTest ("ZTest", Float) = 4
    [HideInInspector] _ZWrite ("ZWrite", Float) = 1
    [HideInInspector] _SrcBlend ("Src", Float) = 1
    [HideInInspector] _DstBlend ("Dst", Float) = 0
  }

  HLSLINCLUDE
  #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
  #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Lighting.hlsl"
  TEXTURE2D(_BaseColorTex); TEXTURE2D(_ShadeTex); TEXTURE2D(_ShadowMaskTex); TEXTURE2D(_SpecularTex); TEXTURE2D(_RimTex); TEXTURE2D(_EmissionTex);
  TEXTURE2D(_NormalTex); TEXTURE2D(_FlowTex); TEXTURE2D(_SdfTex); TEXTURE2D(_OutlineTex); TEXTURE2D(_OverHairTex); TEXTURE2D(_IdTex);
  SAMPLER(sampler_linear_repeat);
  CBUFFER_START(UnityPerMaterial)
    float4 _P00,_P01,_P02,_P03,_P04,_P05,_P06,_P07,_P08,_P09,_P10,_P11,_P12,_P13,_P14,_P15,_P16,_P17,_P18,_P19,_P20,_P21,_P22,_P23,_P24,_P25,_P26,_P27,_P28,_P29,_P30,_P31;
    float _OverPass;
    float _UseAlpha;
  CBUFFER_END
  #define HH_P float4 P[32] = {_P00,_P01,_P02,_P03,_P04,_P05,_P06,_P07,_P08,_P09,_P10,_P11,_P12,_P13,_P14,_P15,_P16,_P17,_P18,_P19,_P20,_P21,_P22,_P23,_P24,_P25,_P26,_P27,_P28,_P29,_P30,_P31};
  #include "hh_toon_core.hlsl"
  // (the maps as imported: Unity's v runs up the picture as the model's UV does — sampled at the mesh's UV as it is; the shader works with
  //  the picture's directions: the flow's y runs down the picture, the normal map's green up it — OpenGL's, as Unity's own)
  float2 hhUV(float2 uv) { return uv; }
  #define HHS(t, uv) SAMPLE_TEXTURE2D(t, sampler_linear_repeat, uv)
  ENDHLSL

  SubShader
  {
    Tags { "RenderType"="Opaque" "RenderPipeline"="UniversalPipeline" "Queue"="Geometry" }

    Pass
    {
      Name "HHToon"
      Tags { "LightMode"="UniversalForward" }
      Cull Off ZWrite [_ZWrite] ZTest [_ZTest] Blend [_SrcBlend] [_DstBlend]
      Stencil { Ref [_StencilRef] Comp [_StencilComp] Pass [_StencilPass] WriteMask [_StencilWrite] ReadMask [_StencilRead] }
      HLSLPROGRAM
      #pragma vertex vert
      #pragma fragment frag
      struct A { float4 pos : POSITION; float3 n : NORMAL; float4 t : TANGENT; float2 uv : TEXCOORD0; };
      struct V { float4 cs : SV_POSITION; float2 uv : TEXCOORD0; float3 ws : TEXCOORD1; float3 n : TEXCOORD2; float4 t : TEXCOORD3; };
      V vert(A a) { V o; VertexPositionInputs p = GetVertexPositionInputs(a.pos.xyz); VertexNormalInputs nn = GetVertexNormalInputs(a.n, a.t);
        o.cs = p.positionCS; o.ws = p.positionWS; o.n = nn.normalWS; o.t = float4(nn.tangentWS, a.t.w * GetOddNegativeScale()); o.uv = hhUV(a.uv); return o; }
      half4 frag(V i, bool front : SV_IsFrontFace) : SV_Target {
        HH_P
        float2 uv = i.uv;
        float4 tex = HHS(_BaseColorTex, uv); float3 base = tex.rgb * P[0].xyz; float a = _UseAlpha > 0.5 ? tex.a : 1.0;   // (the map's alpha on see-through sets only, as HairHarpy)
        clip(a - 0.02);
        float3 N = normalize(i.n); if (!front) N = -N;
        float3 Tt = normalize(i.t.xyz); float3 Bu = cross(N, Tt) * i.t.w;        // (Unity's: the bitangent along the UV's v — up)
        float3 V = normalize(GetWorldSpaceViewDir(i.ws));
        float3 n = N;
        if (P[15].z > 0.5 && P[11].y > 0.5) { float3 t = HHS(_NormalTex, uv).xyz * 2.0 - 1.0; t.xy *= P[11].z; n = normalize(Tt * t.x + Bu * t.y + N * max(t.z, 1e-3)); }
        float3 T = float3(0, 0, 0); int axis = (int)(P[21].x + 0.5);
        if (P[20].x > 0.5) {
          if (axis == 0 && P[15].w > 0.5) { float4 f = HHS(_FlowTex, uv); float2 d = f.xy * 2.0 - 1.0; T = (f.a > 0.05 && dot(d, d) > 1e-4) ? Tt * d.x - Bu * d.y : Bu; }
          else T = axis == 2 ? Tt : Bu;
          T = T - n * dot(T, n); if (dot(T, T) > 1e-8) T = normalize(T);
        }
        float4 shadeMap = P[14].y > 0.5 ? HHS(_ShadeTex, uv) : float4(0, 0, 0, 0);
        float mask = P[14].z > 0.5 ? HHS(_ShadowMaskTex, uv).r : 0.5;
        float specMask = P[14].w > 0.5 ? HHS(_SpecularTex, uv).r : 1.0;
        float rimMask = P[15].x > 0.5 ? HHS(_RimTex, uv).r : 1.0;
        float3 emis = P[15].y > 0.5 ? HHS(_EmissionTex, uv).rgb : float3(0, 0, 0);
        float sdfA = P[16].x > 0.5 ? HHS(_SdfTex, uv).r : 0.5, sdfB = P[16].x > 0.5 ? HHS(_SdfTex, float2(1.0 - uv.x, uv.y)).r : 0.5;
        float3 idMap = P[16].w > 0.5 ? HHS(_IdTex, uv).rgb : float3(0, 0, 0);
        Light L = GetMainLight();
        float3 col = hh_toon(base, n, normalize(L.direction), V, T, shadeMap, mask, specMask, rimMask, emis, sdfA, sdfB, idMap, P) * L.color;
        if (_OverPass > 0.5) { a *= hh_overHair(V, P[16].z > 0.5 ? HHS(_OverHairTex, uv).r : 1.0, P); clip(a - 0.01); }
        return half4(col, a);
      }
      ENDHLSL
    }

    Pass
    {
      Name "HHToonOutline"
      Tags { "LightMode"="SRPDefaultUnlit" }
      Cull Front ZWrite On
      HLSLPROGRAM
      #pragma vertex vert
      #pragma fragment frag
      struct A { float4 pos : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
      struct V { float4 cs : SV_POSITION; };
      V vert(A a) {
        HH_P
        V o; float w = P[17].w; if (P[16].y > 0.5) w *= SAMPLE_TEXTURE2D_LOD(_OutlineTex, sampler_linear_repeat, hhUV(a.uv), 0).r;
        if (P[18].x < 0.5 || _OverPass > 0.5) w = 0.0;
        float4 c = TransformObjectToHClip(a.pos.xyz); float4 cn = TransformObjectToHClip(a.pos.xyz + normalize(a.n) * 0.01);
        float2 d = cn.xy / cn.w - c.xy / c.w; d = dot(d, d) > 1e-14 ? normalize(d) : float2(0, 0);
        c.xy += d * w * 2.0 / _ScreenParams.xy * c.w; o.cs = c; return o; }
      half4 frag(V i) : SV_Target { return half4(_P17.rgb, 1.0); }
      ENDHLSL
    }
  }
}
