"""HairHarpy Toon -> Unreal Engine 5 (editor Python). Run in the editor: Tools > Execute Python Script... > this file
(Python Editor Script Plugin on). It reads hh_toon.json beside its folder (the export of HairHarpy's Materials & Shader), and makes:

  /Game/HairHarpyToon/<project>/Textures/  every UV set's maps (sRGB for colours; masks, normals, directions linear)
  M_HHToon             the master material: Unlit, Opaque; one Custom node holding toon_core.glsl (the same shading as HairHarpy)
  M_HHToon_Translucent the same for the see-through texture sets (an eye's shadow, a glare): blended by the map's alpha
  M_HHToon_OverHair    the over-the-hair pass (brows, lashes): Translucent, no depth test, shown only where the Custom Stencil is 1
  M_HHToon_Outline     the inverted hull for the Overlay Material slot: pushed out along the normal, back faces only
  MI_<object>_<set>    an instance per object's UV set: its maps, its parameters P00...P31 (packed as HairHarpy packs them)

Then, for the selected actors (or every actor in the level with a matching mesh), each mesh slot named as a UV set gets its instance;
outline instances go into the Overlay Material. The light: the Sky Atmosphere's first directional light (SkyAtmosphereLightDirection).
The over-the-hair parts need Custom Depth-Stencil (Project Settings > Rendering > Custom Depth-Stencil Pass: Enabled with Stencil):
the script sets the brows' components to write stencil 1 and the head's 2 (see README.md).
"""
import json
import os
import struct
import zlib

import unreal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SLOT_PARAM = ["BaseColorTex", "ShadeTex", "ShadowMaskTex", "SpecularTex", "RimTex", "EmissionTex", "NormalTex", "FlowTex", "SdfTex",
              "OutlineTex", "OverHairTex", "IdTex"]

MEL = unreal.MaterialEditingLibrary
ATL = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary


def inputs(names):
    """The Custom node's inputs (a struct built field by field: UE 5.7's CustomInput takes no keyword arguments)."""
    out = []
    for n in names:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        out.append(ci)
    return out


def head_axes_ue(P):
    """HairHarpy is Y up (right-handed); Unreal's FBX import makes it Z up (left-handed): (x, y, z) -> (x, z, y). The head locator's
    forward / right / up (P22..P24) go the same way, so they stay the same directions on the model."""
    P = list(P)
    for i in (22, 23, 24):
        x, y, z = P[i * 4], P[i * 4 + 1], P[i * 4 + 2]
        P[i * 4], P[i * 4 + 1], P[i * 4 + 2] = x, z, y
    return P


def white_linear(dest):
    """A white texture read linear: the default of the mask / normal / direction slots (a linear sampler wants a linear texture)."""
    path = dest + "/T_HHToon_WhiteLinear"
    if EAL.does_asset_exist(path):
        return unreal.load_asset(path)
    raw = b"".join(b"\x00" + b"\xff" * 16 for _ in range(4))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    f = os.path.join(unreal.Paths.project_intermediate_dir(), "T_HHToon_WhiteLinear.png")
    with open(f, "wb") as o:
        o.write(png)
    t = unreal.AssetImportTask(); t.filename = f; t.destination_path = dest; t.destination_name = "T_HHToon_WhiteLinear"
    t.automated = True; t.replace_existing = True; t.save = True
    ATL.import_asset_tasks([t])
    tex = unreal.load_asset(path)
    tex.set_editor_property("srgb", False)
    EAL.save_loaded_asset(tex)
    return tex


def load_json():
    with open(os.path.join(ROOT, "hh_toon.json"), "r", encoding="utf-8") as f:
        return json.load(f)


def core_text(lib):
    with open(os.path.join(ROOT, lib.get("core", "toon_core.glsl")), "r", encoding="utf-8") as f:
        return f.read()


def safe(name):
    return "".join(c if c.isalnum() or c in "_-" else "_" for c in name)


def import_textures(lib, dest):
    """Every map file of every set, once; colour maps sRGB, the rest linear (Normal not as a normal map: unpacked by the shader)."""
    out = {}
    srgb = dict(zip(lib["slots"], lib["srgb"]))
    files = []
    for set_name, s in lib["sets"].items():
        for slot, rel in s.get("maps", {}).items():
            files.append((rel, srgb.get(slot, False)))
    for p in lib["parts"]:
        if p.get("sdf"):
            files.append((p["sdf"], False))
    tasks = []
    for rel, is_srgb in dict(files).items():
        t = unreal.AssetImportTask()
        t.filename = os.path.join(ROOT, rel)
        t.destination_path = dest
        t.destination_name = safe(os.path.splitext(os.path.basename(rel))[0])
        t.automated = True
        t.replace_existing = True
        t.save = True
        tasks.append((t, rel, is_srgb))
    ATL.import_asset_tasks([t for t, _, _ in tasks])
    for t, rel, is_srgb in tasks:
        path = dest + "/" + t.destination_name
        tex = unreal.load_asset(path)
        if not tex:
            continue
        tex.set_editor_property("srgb", bool(is_srgb))
        if not is_srgb:
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
        EAL.save_loaded_asset(tex)
        out[rel] = tex
    return out


def wrapper_code(core):
    # the Custom node: the core as a struct's functions (a Custom node holds one function body), the maps read as toon.frag reads them
    return (
        "struct HHToonCore {\n" + core + "\n};\n"
        "HHToonCore C;\n"
        "float4 P[32] = {P00,P01,P02,P03,P04,P05,P06,P07,P08,P09,P10,P11,P12,P13,P14,P15,P16,P17,P18,P19,P20,P21,P22,P23,P24,P25,P26,P27,P28,P29,P30,P31};\n"
        "float2 uv = UV;\n"
        "float3 N = normalize(Nw); float3 Tt = normalize(Tw); float3 Bb = normalize(cross(N, Tt));\n"
        "float4 tex = Texture2DSample(BaseColorTex, BaseColorTexSampler, uv);\n"
        "float3 base = tex.rgb * P[0].xyz;\n"
        "float3 n = N;\n"
        "if (P[15].z > 0.5 && P[11].y > 0.5) { float3 t = Texture2DSample(NormalTex, NormalTexSampler, uv).xyz * 2.0 - 1.0; t.xy *= P[11].z;"
        " n = normalize(Tt * t.x + Bb * t.y * FlipGreen + N * max(t.z, 1e-3)); }\n"
        "float3 T = float3(0,0,0); int axis = (int)(P[21].x + 0.5);\n"
        "if (P[20].x > 0.5) { if (axis == 0 && P[15].w > 0.5) { float4 f = Texture2DSample(FlowTex, FlowTexSampler, uv); float2 d = f.xy * 2.0 - 1.0;"
        " T = (f.a > 0.05 && dot(d, d) > 1e-4) ? Tt * d.x - Bb * d.y * FlipGreen : -Bb * FlipGreen; } else T = axis == 2 ? Tt : -Bb * FlipGreen;"
        " T = T - n * dot(T, n); if (dot(T, T) > 1e-8) T = normalize(T); }\n"
        "float4 shadeMap = P[14].y > 0.5 ? Texture2DSample(ShadeTex, ShadeTexSampler, uv) : float4(0,0,0,0);\n"
        "float mask = P[14].z > 0.5 ? Texture2DSample(ShadowMaskTex, ShadowMaskTexSampler, uv).r : 0.5;\n"
        "float specMask = P[14].w > 0.5 ? Texture2DSample(SpecularTex, SpecularTexSampler, uv).r : 1.0;\n"
        "float rimMask = P[15].x > 0.5 ? Texture2DSample(RimTex, RimTexSampler, uv).r : 1.0;\n"
        "float3 emis = P[15].y > 0.5 ? Texture2DSample(EmissionTex, EmissionTexSampler, uv).rgb : float3(0,0,0);\n"
        "float sdfA = P[16].x > 0.5 ? Texture2DSample(SdfTex, SdfTexSampler, uv).r : 0.5;\n"
        "float sdfB = P[16].x > 0.5 ? Texture2DSample(SdfTex, SdfTexSampler, float2(1.0 - uv.x, uv.y)).r : 0.5;\n"
        "float3 idMap = P[16].w > 0.5 ? Texture2DSample(IdTex, IdTexSampler, uv).rgb : float3(0,0,0);\n"
        "float3 col = C.hh_toon(base, n, normalize(Lw), normalize(Vw), T, shadeMap, mask, specMask, rimMask, emis, sdfA, sdfB, idMap, P);\n"
        "float over = OverPass > 0.5 ? C.hh_overHair(normalize(Vw), P[16].z > 0.5 ? Texture2DSample(OverHairTex, OverHairTexSampler, uv).r : 1.0, P) : 1.0;\n"
        "return float4(col, (UseAlpha > 0.5 ? tex.a : 1.0) * over);\n"
    )


def expr(mat, cls, x, y):
    return MEL.create_material_expression(mat, cls, x, y)


def mat_prop(name):
    """A material output by name across engine versions (UE 5.5's Python has no MP_WORLD_POSITION_OFFSET member under that name)."""
    e = unreal.MaterialProperty
    if hasattr(e, "MP_" + name):
        return getattr(e, "MP_" + name)
    key = name.replace("_", "").lower()
    for attr in dir(e):
        if attr.startswith("MP_") and attr[3:].replace("_", "").lower() == key:
            return getattr(e, attr)
    raise AttributeError("MaterialProperty " + name + ": " + ", ".join(a for a in dir(e) if a.startswith("MP_")))


def wire_rgba(mat, v, target, name):
    """A Vector Parameter into a Custom node's input as float4: its default output is RGB only (float3) — the RGBA pin, or RGB + A
    appended where the pin is missing."""
    if MEL.connect_material_expressions(v, "RGBA", target, name):
        return
    ap = expr(mat, unreal.MaterialExpressionAppendVector, -1500, 0)
    MEL.connect_material_expressions(v, "", ap, "A"); MEL.connect_material_expressions(v, "A", ap, "B")
    MEL.connect_material_expressions(ap, "", target, name)


def make_master(path, core, mode, white_lin):
    """mode: "opaque" (a solid part: its map's alpha unused, as HairHarpy draws it), "translucent" (a see-through texture set: blended by
    the map's alpha), "over" (the over-the-hair pass). UseAlpha (an instance's parameter): the map's alpha counts — see-through parts only."""
    over_pass = mode == "over"
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    folder, name = path.rsplit("/", 1)
    mat = ATL.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    if over_pass:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        mat.set_editor_property("disable_depth_test", True)
    elif mode == "translucent":
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    else:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    custom = expr(mat, unreal.MaterialExpressionCustom, -400, 0)
    custom.set_editor_property("code", wrapper_code(core))
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    names = ["UV", "Nw", "Tw", "Lw", "Vw", "FlipGreen", "OverPass", "UseAlpha"] + SLOT_PARAM + ["P%02d" % i for i in range(32)]
    custom.set_editor_property("inputs", inputs(names))
    y = 0
    def wire(e, name):
        MEL.connect_material_expressions(e, "", custom, name)
    wire(expr(mat, unreal.MaterialExpressionTextureCoordinate, -900, y), "UV")
    wire(expr(mat, unreal.MaterialExpressionVertexNormalWS, -900, y + 60), "Nw")
    wire(expr(mat, unreal.MaterialExpressionVertexTangentWS, -900, y + 120), "Tw")
    wire(expr(mat, unreal.MaterialExpressionSkyAtmosphereLightDirection, -900, y + 180), "Lw")
    wire(expr(mat, unreal.MaterialExpressionCameraVectorWS, -900, y + 240), "Vw")
    fg = expr(mat, unreal.MaterialExpressionScalarParameter, -900, y + 300); fg.set_editor_property("parameter_name", "FlipGreen"); fg.set_editor_property("default_value", 1.0); wire(fg, "FlipGreen")
    op = expr(mat, unreal.MaterialExpressionConstant, -900, y + 360); op.set_editor_property("r", 1.0 if over_pass else 0.0); wire(op, "OverPass")
    ua = expr(mat, unreal.MaterialExpressionScalarParameter, -900, y + 420); ua.set_editor_property("parameter_name", "UseAlpha"); wire(ua, "UseAlpha")
    white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    for i, n in enumerate(SLOT_PARAM):
        t = expr(mat, unreal.MaterialExpressionTextureObjectParameter, -1300, i * 70)
        t.set_editor_property("parameter_name", n)
        if n in ("BaseColorTex", "ShadeTex", "EmissionTex"):
            t.set_editor_property("texture", white)
        else:
            t.set_editor_property("texture", white_lin)
            t.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        wire(t, n)
    for i in range(32):
        v = expr(mat, unreal.MaterialExpressionVectorParameter, -1700, i * 60)
        v.set_editor_property("parameter_name", "P%02d" % i); wire_rgba(mat, v, custom, "P%02d" % i)
    rgb = expr(mat, unreal.MaterialExpressionComponentMask, -150, 0)
    rgb.set_editor_property("r", True); rgb.set_editor_property("g", True); rgb.set_editor_property("b", True); rgb.set_editor_property("a", False)
    MEL.connect_material_expressions(custom, "", rgb, "")
    MEL.connect_material_property(rgb, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    a = expr(mat, unreal.MaterialExpressionComponentMask, -150, 120)   # (the alpha alone: R / G / B set off by hand — they are on by default)
    for ch, on in (("r", False), ("g", False), ("b", False), ("a", True)):
        a.set_editor_property(ch, on)
    MEL.connect_material_expressions(custom, "", a, "")
    if over_pass:
        # (only where the brows are the nearest of the Custom Depth objects: stencil 1)
        st = expr(mat, unreal.MaterialExpressionCustom, -400, 300)
        # (the brows the nearest of the Custom Depth objects — stencil 1 — and something solid in front of them: the hair)
        st.set_editor_property("code", "return (abs(Stencil.r - 1.0) < 0.5 && SceneD.r < CustomD.r - 0.5) ? A : 0.0;")
        st.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
        st.set_editor_property("inputs", inputs(["A", "Stencil", "SceneD", "CustomD"]))
        sc = expr(mat, unreal.MaterialExpressionSceneTexture, -700, 340); sc.set_editor_property("scene_texture_id", unreal.SceneTextureId.PPI_CUSTOM_STENCIL)
        sd = expr(mat, unreal.MaterialExpressionSceneTexture, -700, 420); sd.set_editor_property("scene_texture_id", unreal.SceneTextureId.PPI_SCENE_DEPTH)
        cd = expr(mat, unreal.MaterialExpressionSceneTexture, -700, 500); cd.set_editor_property("scene_texture_id", unreal.SceneTextureId.PPI_CUSTOM_DEPTH)
        MEL.connect_material_expressions(a, "", st, "A"); MEL.connect_material_expressions(sc, "Color", st, "Stencil")
        MEL.connect_material_expressions(sd, "Color", st, "SceneD"); MEL.connect_material_expressions(cd, "Color", st, "CustomD")
        MEL.connect_material_property(st, "", unreal.MaterialProperty.MP_OPACITY)
    elif mode == "translucent":
        MEL.connect_material_property(a, "", unreal.MaterialProperty.MP_OPACITY)
    MEL.recompile_material(mat); EAL.save_loaded_asset(mat)
    return mat


def make_outline(path, white_lin):
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    folder, name = path.rsplit("/", 1)
    mat = ATL.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property("two_sided", True)
    # (pushed out along the normal by Width — screen pixels as HairHarpy, near enough: centimetres per pixel at the vertex's distance —
    #  back faces only: TwoSidedSign)
    c = expr(mat, unreal.MaterialExpressionCustom, -400, 0)
    c.set_editor_property("code", "float px = P17.w * (P16.y > 0.5 ? W : 1.0) * (P18.x > 0.5 ? 1.0 : 0.0);\n"
                                  "float dist = length(Pos - Cam); return N * px * dist * PixelAngle;")
    c.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    c.set_editor_property("inputs", inputs(["N", "Pos", "Cam", "W", "PixelAngle", "P16", "P17", "P18"]))
    MEL.connect_material_expressions(expr(mat, unreal.MaterialExpressionVertexNormalWS, -800, 0), "", c, "N")
    MEL.connect_material_expressions(expr(mat, unreal.MaterialExpressionWorldPosition, -800, 60), "", c, "Pos")
    MEL.connect_material_expressions(expr(mat, unreal.MaterialExpressionCameraPositionWS, -800, 120), "", c, "Cam")
    pa = expr(mat, unreal.MaterialExpressionScalarParameter, -800, 180); pa.set_editor_property("parameter_name", "PixelAngle"); pa.set_editor_property("default_value", 0.0012)
    MEL.connect_material_expressions(pa, "", c, "PixelAngle")
    tw = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -800, 240); tw.set_editor_property("parameter_name", "OutlineTex")
    tw.set_editor_property("texture", white_lin)
    tw.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    MEL.connect_material_expressions(tw, "R", c, "W")
    for i, n in enumerate(["P16", "P17", "P18"]):
        v = expr(mat, unreal.MaterialExpressionVectorParameter, -800, 320 + i * 60); v.set_editor_property("parameter_name", n)
        wire_rgba(mat, v, c, n)
    try:
        MEL.connect_material_property(c, "", mat_prop("WORLD_POSITION_OFFSET"))
    except AttributeError:
        # (UE 5.5's Python has no World Position Offset output to wire: the material is made whole, one wire left by hand — till then the
        #  outline only lies under the model, unseen)
        unreal.log_warning("HairHarpy Toon: open " + path + " and connect the Custom node (the outline's push) to World Position Offset — "
                           "this engine's Python can't wire that output")
    col = expr(mat, unreal.MaterialExpressionComponentMask, -300, 300); col.set_editor_property("r", True); col.set_editor_property("g", True); col.set_editor_property("b", True); col.set_editor_property("a", False)
    cp = expr(mat, unreal.MaterialExpressionVectorParameter, -500, 420); cp.set_editor_property("parameter_name", "P17")   # (the colour: P17's rgb — the same parameter, read again)
    MEL.connect_material_expressions(cp, "", col, "")
    MEL.connect_material_property(col, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    back = expr(mat, unreal.MaterialExpressionCustom, -300, 500)
    back.set_editor_property("code", "return S < 0.0 ? 1.0 : 0.0;"); back.set_editor_property("inputs", inputs(["S"]))
    MEL.connect_material_expressions(expr(mat, unreal.MaterialExpressionTwoSidedSign, -600, 500), "", back, "S")
    MEL.connect_material_property(back, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.recompile_material(mat); EAL.save_loaded_asset(mat)
    return mat


def make_instance(path, parent, part, textures, lib):
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    folder, name = path.rsplit("/", 1)
    mi = ATL.create_asset(name, folder, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi, parent)
    P = head_axes_ue(part["P"])
    for i in range(32):
        MEL.set_material_instance_vector_parameter_value(mi, "P%02d" % i, unreal.LinearColor(P[i * 4], P[i * 4 + 1], P[i * 4 + 2], P[i * 4 + 3]))
    maps = lib["sets"].get(part["set"], {}).get("maps", {})
    for slot, param in zip(lib["slots"], SLOT_PARAM):
        rel = part.get("sdf") if param == "SdfTex" else maps.get(slot)
        if rel and rel in textures:
            MEL.set_material_instance_texture_parameter_value(mi, param, textures[rel])
    MEL.set_material_instance_scalar_parameter_value(mi, "UseAlpha", 1.0 if part.get("transparent") else 0.0)
    EAL.save_loaded_asset(mi)
    return mi


def make_hidden(path):
    """Nothing drawn: the slots of an over-the-hair copy that are not over the hair."""
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    folder, name = path.rsplit("/", 1)
    mat = ATL.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    z = expr(mat, unreal.MaterialExpressionConstant, -300, 0); z.set_editor_property("r", 0.0)
    MEL.connect_material_property(z, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.recompile_material(mat); EAL.save_loaded_asset(mat)
    return mat


OVER_SUFFIX = "_HHOverHair"


def over_copy(eas, actor, comp, mats):
    """The over-the-hair pass: the same mesh again, attached to the actor, its over slots in the over material (drawn last, no depth
    test, only where the Custom Stencil says the brows were hidden by the hair), the rest hidden. A skeletal mesh follows the original's
    pose (leader pose)."""
    t = comp.get_world_transform()
    if isinstance(comp, unreal.SkeletalMeshComponent):
        a2 = eas.spawn_actor_from_class(unreal.SkeletalMeshActor, t.translation, t.rotation.rotator())
        c2 = a2.skeletal_mesh_component
        c2.set_skeletal_mesh_asset(comp.get_skeletal_mesh_asset()) if hasattr(c2, "set_skeletal_mesh_asset") else c2.set_skeletal_mesh(comp.skeletal_mesh)
        c2.set_leader_pose_component(comp)
    elif isinstance(comp, unreal.StaticMeshComponent):
        a2 = eas.spawn_actor_from_class(unreal.StaticMeshActor, t.translation, t.rotation.rotator())
        c2 = a2.static_mesh_component
        c2.set_static_mesh(comp.static_mesh)
    else:
        return None
    a2.set_actor_scale3d(t.scale3d)
    a2.attach_to_actor(actor, "", unreal.AttachmentRule.KEEP_WORLD, unreal.AttachmentRule.KEEP_WORLD, unreal.AttachmentRule.KEEP_WORLD, False)
    a2.set_actor_label(actor.get_actor_label() + OVER_SUFFIX)
    for i, m in enumerate(mats):
        c2.set_material(i, m)
    c2.set_render_custom_depth(False)
    c2.set_editor_property("cast_shadow", False)
    c2.set_editor_property("translucency_sort_priority", 100)
    return a2


def apply_to_level(lib, made, hidden):
    """Each static / skeletal mesh component of the selected actors (else all): a slot named as a UV set gets that set's instance."""
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_selected_level_actors() or eas.get_all_level_actors()
    for a in list(eas.get_all_level_actors()):     # (the over copies of an earlier run: made again)
        if a.get_actor_label().endswith(OVER_SUFFIX):
            eas.destroy_actor(a)
    actors = [a for a in actors if a and not a.get_actor_label().endswith(OVER_SUFFIX)]
    n = 0
    for a in actors:
        for comp in a.get_components_by_class(unreal.MeshComponent):
            names = comp.get_material_slot_names()
            over_mats, any_over = [], False
            for i, slot in enumerate(names):
                key = (a.get_actor_label(), str(slot))
                hit = made.get(key) or next((v for k, v in made.items() if k[1] == str(slot)), None)
                over_mats.append(hit["over"] if hit and hit.get("over") else hidden)
                if not hit:
                    continue
                comp.set_material(i, hit["mi"])
                if hit["role"] == "over":
                    comp.set_render_custom_depth(True); comp.set_custom_depth_stencil_value(1); any_over = True
                elif hit["role"] == "hair":
                    comp.set_render_custom_depth(False)
                else:
                    comp.set_render_custom_depth(True); comp.set_custom_depth_stencil_value(2)
                if hit.get("outline") and hasattr(comp, "set_overlay_material"):
                    comp.set_overlay_material(hit["outline"])
                n += 1
            if any_over:
                over_copy(eas, a, comp, over_mats)
    return n


def main():
    lib = load_json()
    base = "/Game/HairHarpyToon/" + safe(lib.get("project") or "Project")
    textures = import_textures(lib, base + "/Textures")
    core = core_text(lib)
    white_lin = white_linear(base + "/Textures")
    master = make_master(base + "/M_HHToon", core, "opaque", white_lin)
    master_tr = make_master(base + "/M_HHToon_Translucent", core, "translucent", white_lin)
    over = make_master(base + "/M_HHToon_OverHair", core, "over", white_lin)
    outline = make_outline(base + "/M_HHToon_Outline", white_lin)
    hidden = make_hidden(base + "/M_HHToon_Hidden")
    made = {}
    for part in lib["parts"]:
        tag = safe(part["object"] + "_" + part["set"])
        mi = make_instance(base + "/MI_" + tag, master_tr if part.get("transparent") else master, part, textures, lib)
        rec = {"mi": mi, "role": part.get("role", "")}
        if part["P"][18 * 4] > 0.5:
            mo = make_instance(base + "/MIO_" + tag, outline, part, textures, lib); rec["outline"] = mo
        if part.get("role") == "over":
            rec["over"] = make_instance(base + "/MIH_" + tag, over, part, textures, lib)
        made[(part["object"], part["set"])] = rec
    n = apply_to_level(lib, made, hidden)
    unreal.log("HairHarpy Toon: %d instance(s) made in %s, %d slot(s) assigned" % (len(made), base, n))


main()
