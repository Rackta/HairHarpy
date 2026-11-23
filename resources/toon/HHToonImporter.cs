// HairHarpy Toon → Unity (URP): menu HairHarpy / Import Toon (hh_toon.json). Put the whole export folder inside Assets first. It sets the
// maps' import (colours sRGB, the rest linear), makes a material per object's UV set (HairHarpy/Toon with that part's parameters and
// maps) and gives them to the renderers in the open scene whose GameObject is named as HairHarpy's object and whose material slot is
// named as the UV set. Over-the-hair parts get a second material (drawn again over the hair where they were seen); hair and the rest
// get their stencil and queue (see HHToon.shader).
#if UNITY_EDITOR
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

public static class HHToonImporter
{
    [System.Serializable] class Part { public string @object; public int objectId; public string set; public string instance; public string role; public bool transparent; public string sdf; public float[] P; }
    [System.Serializable] class Lib { public string project; public string core; public string[] slots; public bool[] srgb; public Part[] parts; }

    static readonly string[] Props = { "_BaseColorTex", "_ShadeTex", "_ShadowMaskTex", "_SpecularTex", "_RimTex", "_EmissionTex", "_NormalTex", "_FlowTex", "_SdfTex", "_OutlineTex", "_OverHairTex", "_IdTex" };

    [MenuItem("HairHarpy/Import Toon (hh_toon.json)")]
    static void Import()
    {
        string json = EditorUtility.OpenFilePanel("HairHarpy Toon export", Application.dataPath, "json");
        if (string.IsNullOrEmpty(json)) return;
        if (!json.Replace('\\', '/').StartsWith(Application.dataPath)) { EditorUtility.DisplayDialog("HairHarpy Toon", "Put the export folder inside Assets first.", "OK"); return; }
        string root = Path.GetDirectoryName(json).Replace('\\', '/'); string rel = "Assets" + root.Substring(Application.dataPath.Length);
        var lib = JsonUtility.FromJson<Lib>(File.ReadAllText(json));
        var setMaps = ReadSetMaps(File.ReadAllText(json));
        var shader = Shader.Find("HairHarpy/Toon");
        if (shader == null) { EditorUtility.DisplayDialog("HairHarpy Toon", "HHToon.shader not found — the unity folder must be inside Assets.", "OK"); return; }
        string outDir = rel + "/Materials"; if (!AssetDatabase.IsValidFolder(outDir)) AssetDatabase.CreateFolder(rel, "Materials");
        // (the maps' import: colours sRGB, masks / normals / directions linear, not as Unity's normal maps — the shader unpacks them)
        for (int s = 0; s < lib.slots.Length; s++)
            foreach (var kv in setMaps) if (kv.Value.TryGetValue(lib.slots[s], out var f)) SetImport(rel + "/" + f, lib.srgb[s]);
        var made = new Dictionary<string, (Material mat, Material over, string role)>();
        foreach (var p in lib.parts)
        {
            var m = new Material(shader) { name = "HH_" + p.@object + "_" + p.set };
            for (int i = 0; i < 32; i++)
            {
                var v = new Vector4(p.P[i * 4], p.P[i * 4 + 1], p.P[i * 4 + 2], p.P[i * 4 + 3]);
                if (i >= 22 && i <= 24) v.x = -v.x;   // (the head's forward / right / up: Unity's FBX import mirrors X, the vectors with it)
                m.SetVector("_P" + i.ToString("00"), v);
            }
            setMaps.TryGetValue(p.set, out var maps);
            for (int s = 0; s < lib.slots.Length && s < Props.Length; s++)
            {
                string f = s == 8 ? p.sdf : (maps != null && maps.TryGetValue(lib.slots[s], out var mf) ? mf : null);
                if (string.IsNullOrEmpty(f)) continue;
                if (s == 8) SetImport(rel + "/" + f, false);
                var tex = AssetDatabase.LoadAssetAtPath<Texture2D>(rel + "/" + f); if (tex) m.SetTexture(Props[s], tex);
            }
            if (p.transparent)   // (a see-through texture set: blended by its map's alpha, drawn after the solid ones)
            {
                m.SetFloat("_UseAlpha", 1); m.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha); m.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha); m.SetFloat("_ZWrite", 0);
            }
            Material over = null;
            switch (p.role)
            {
                case "over": Stencil(m, 1, CompareFunction.Always, StencilOp.Replace); m.renderQueue = 2000;
                    over = new Material(m) { name = m.name + "_OverHair" }; over.SetFloat("_OverPass", 1); over.SetFloat("_ZTest", (float)CompareFunction.Always); over.SetFloat("_ZWrite", 0);
                    over.SetFloat("_SrcBlend", (float)BlendMode.SrcAlpha); over.SetFloat("_DstBlend", (float)BlendMode.OneMinusSrcAlpha);
                    Stencil(over, 3, CompareFunction.Equal, StencilOp.Keep); over.SetFloat("_StencilWrite", 0); over.SetFloat("_StencilRead", 3); over.renderQueue = 3100;
                    AssetDatabase.CreateAsset(over, outDir + "/" + Safe(over.name) + ".mat"); break;
                case "hair": Stencil(m, 2, CompareFunction.Always, StencilOp.Replace); m.SetFloat("_StencilWrite", 2); m.renderQueue = 2002; break;
                default: Stencil(m, 0, CompareFunction.Always, StencilOp.Replace); m.renderQueue = 2001; break;
            }
            if (p.transparent) m.renderQueue += 1000;   // (the transparent queue, the roles' order kept: over → the rest → hair)
            AssetDatabase.CreateAsset(m, outDir + "/" + Safe(m.name) + ".mat");
            made[p.@object + "|" + p.set] = (m, over, p.role);
        }
        int n = 0;
        foreach (var r in Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None))
        {
            var mats = r.sharedMaterials; var extra = new List<Material>(); bool changed = false;
            for (int i = 0; i < mats.Length; i++)
            {
                string slot = mats[i] ? mats[i].name.Replace(" (Instance)", "") : "";
                if (!made.TryGetValue(r.gameObject.name + "|" + slot, out var hit)) continue;
                mats[i] = hit.mat; changed = true; n++;
                if (hit.over != null) extra.Add(hit.over);   // (an extra material draws the last submesh again: the brows over the hair)
            }
            if (!changed) continue;
            var all = new List<Material>(mats); all.AddRange(extra); Undo.RecordObject(r, "HairHarpy Toon"); r.sharedMaterials = all.ToArray();
        }
        AssetDatabase.SaveAssets();
        EditorUtility.DisplayDialog("HairHarpy Toon", $"{made.Count} material(s) made in {outDir}, {n} slot(s) assigned in the open scene.", "OK");
    }

    static void Stencil(Material m, int r, CompareFunction c, StencilOp op) { m.SetFloat("_StencilRef", r); m.SetFloat("_StencilComp", (float)c); m.SetFloat("_StencilPass", (float)op); }
    static string Safe(string s) { foreach (char c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_'); return s; }
    static void SetImport(string path, bool srgb)
    {
        if (!(AssetImporter.GetAtPath(path) is TextureImporter ti)) return;
        if (ti.sRGBTexture == srgb && ti.textureType == TextureImporterType.Default) return;
        ti.textureType = TextureImporterType.Default; ti.sRGBTexture = srgb; ti.wrapMode = TextureWrapMode.Repeat; ti.SaveAndReimport();
    }
    // (hh_toon.json's "sets": { set: { maps: { slot: file } } } — a dictionary JsonUtility does not read: picked out by hand)
    static Dictionary<string, Dictionary<string, string>> ReadSetMaps(string text)
    {
        var res = new Dictionary<string, Dictionary<string, string>>();
        int i = text.IndexOf("\"sets\""); if (i < 0) return res; i = text.IndexOf('{', i);
        int depth = 0, end = i; for (; end < text.Length; end++) { if (text[end] == '{') depth++; else if (text[end] == '}' && --depth == 0) break; }
        string body = text.Substring(i + 1, end - i - 1); int k = 0;
        while (true)
        {
            int q = body.IndexOf('"', k); if (q < 0) break; int q2 = body.IndexOf('"', q + 1); string set = body.Substring(q + 1, q2 - q - 1);
            int mo = body.IndexOf("\"maps\"", q2); int b0 = body.IndexOf('{', mo), b1 = body.IndexOf('}', b0);
            var maps = new Dictionary<string, string>(); string mb = body.Substring(b0 + 1, b1 - b0 - 1);
            foreach (var pair in mb.Split(',')) { var kv = pair.Split(':'); if (kv.Length != 2) continue; maps[kv[0].Trim().Trim('"')] = kv[1].Trim().Trim('"'); }
            res[set] = maps; k = body.IndexOf('}', b1 + 1) + 1; if (k <= 0) break;
        }
        return res;
    }
}
#endif
