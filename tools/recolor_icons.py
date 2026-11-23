# Recolours the tool icons by the kind of tool (the colour code fixed 2026-11-22):
#   selection — orange, drawing — lilac, modifiers / symmetry — teal, modelling — green, UV — dark orange (as Send to Rizom),
#   transform — blue, inspector checks — yellow, hair strands — pink, layers / files / view — grey (no colour).
# Only the accent is changed (pixels of the old accent hue: orange or lilac); other colours in an icon (a teal badge, a blue part) stay.
# An icon of a coloured group with no accent at all has its light strokes tinted instead.
# Idempotent. Run after any tools\make_*_icons.ps1:   py tools\recolor_icons.py
import colorsys, os, sys, collections
from PIL import Image

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'resources', 'images')

SEL, DRAW, TEAL = '#f08000', '#9466be', '#28d0c0'
MODEL, UV, XFORM, INSP, HAIR, NEUTRAL = '#58b85c', '#e84000', '#3d8cf0', '#e8c22c', '#e0609a', '#8e8e94'

def names(*a): return list(a)
GROUPS = {
  # selection: the existing tsel_* / rc_sel_* are kept as they are (the author: "already right")
  SEL: names('top_grow', 'top_shrink', 'top_invert', 'top_selmirror', 'top_selset'),
  TEAL: names('tt_subdiv', 'tt_vsymgroup', 'tt_modgroups', 'tt_mirror', 'topo_sym', 'pt_toposym', 'tool_curvemods', 'tool_shellmods'),
  MODEL: names('tt_alignnormals', 'tt_bevel', 'tt_bridge', 'tt_collapse', 'tt_delanchor', 'tt_edgechain', 'tt_fillhole', 'tt_fixmesh',
               'tt_identical', 'tt_inset', 'tt_knife', 'tt_loop', 'tt_normals', 'tt_optimize', 'tt_patch', 'tt_planecut', 'tt_polypen',
               'tt_relax', 'tt_reversenormals', 'tt_shaper', 'tt_slide', 'tt_snapgeom', 'tt_subdivide', 'tt_weld', 'tt_scissors',
               'top_detach', 'top_melt', 'top_split', 'top_splitpg', 'top_mirror', 'top_hard', 'top_soft',
               'topo_snap', 'topo_snap_v2', 'topo_snapsettings', 'topo_reveal'),
  XFORM: names('tool_move', 'tool_rotate', 'tool_scale', 'rc_move', 'rc_move_all', 'rc_move_layer', 'rc_transform', 'rc_xf_reset',
               'rc_flip_h', 'rc_flip_v', 'rc_offset_reset', 'uv_rotate90', 'uv_flipu', 'uv_flipv', 'uv_fit', 'uv_shell90', 'uv_shell180'),
  INSP: [],   # every insp_* (filled below)
  HAIR: names('tool_braid', 'tool_crown', 'tool_lockex', 'tool_lockfix', 'tool_cocoon', 'tool_draw', 'tool_brush', 'tool_cut', 'tool_edge'),
  NEUTRAL: names('rc_layer_up', 'rc_layer_down', 'rc_layer_dup', 'rc_layer_new', 'rc_merge', 'rc_delete', 'rc_lock', 'rc_front', 'rc_behind',
                 'rc_canvas_copy', 'rc_library', 'rc_group_new', 'rc_set_new', 'rc_brush_import', 'rc_brush_new',
                 'pt_eye', 'pt_view_roll', 'pt_view_reset', 'pt_psd', 'pt_paste', 'pt_rename', 'pt_smartname', 'pt_keep', 'pt_ok', 'pt_cancel',
                 'pt_from_layer', 'pt_transfer', 'pm_undo', 'pm_redo', 'pm_vis', 'pm_float', 'pm_model', 'pm_tex2d'),
  UV: [],     # every uv_* not above, topo_uv, topo_keepuv (filled below)
}
# the hues an old accent may have: orange (the Topology / UV accent) and lilac (the Paint accent)
SRC = [(5, 50), (250, 305)]

def hsv(c): return colorsys.rgb_to_hsv(*(v / 255 for v in c[:3]))
def target(hx): return hsv(tuple(int(hx[i:i + 2], 16) for i in (1, 3, 5)) + (255,))

def recolor(path, hx):
    im = Image.open(path).convert('RGBA'); px = im.load(); W, H = im.size
    th, ts, tv = target(hx); grey = hx == NEUTRAL
    def is_accent(c):   # an old accent — or the group's own colour already (so a second run changes nothing)
        if c[3] < 40: return False
        h, s, v = hsv(c); h *= 360
        own = min(abs(h - th * 360), 360 - abs(h - th * 360)) < 20
        return s > 0.3 and v > 0.3 and (own or any(a <= h < b for a, b in SRC))
    acc = collections.Counter((r // 4, g // 4, b // 4) for y in range(H) for x in range(W) for (r, g, b, a) in [px[x, y]] if is_accent((r, g, b, a)))
    if acc:
        rs, rv = hsv(tuple(v * 4 for v in acc.most_common(1)[0][0]) + (255,))[1:]
        for y in range(H):
            for x in range(W):
                c = px[x, y]
                if not is_accent(c): continue
                _, s, v = hsv(c)
                ns = 0 if grey else min(1, ts * s / max(rs, 1e-3)); nv = min(1, tv * v / max(rv, 1e-3))
                r, g, b = colorsys.hsv_to_rgb(th, ns, nv); px[x, y] = (round(r * 255), round(g * 255), round(b * 255), c[3])
        im.save(path); return 'accent'
    if grey: return 'none'
    # no accent: the light strokes take the group's colour (their lightness kept)
    for y in range(H):
        for x in range(W):
            c = px[x, y]
            if c[3] < 10: continue
            h, s, v = hsv(c)
            if s < 0.15 and v > 0.55:
                k = (v - 0.55) / 0.45
                r, g, b = colorsys.hsv_to_rgb(th, ts * (0.55 + 0.45 * k), min(1, tv * (0.75 + 0.25 * k) * v / 0.8)); px[x, y] = (round(r * 255), round(g * 255), round(b * 255), c[3])
    im.save(path); return 'tinted'

def main():
    files = {f[:-4] for f in os.listdir(HERE) if f.endswith('.png')}
    taken = {n for l in GROUPS.values() for n in l}
    GROUPS[INSP] += sorted(n for n in files if n.startswith('insp_'))
    GROUPS[UV] += sorted(n for n in files if (n.startswith('uv_') and n not in taken) or n in ('topo_uv', 'topo_keepuv'))
    only = set(sys.argv[1:])
    for hx, lst in GROUPS.items():
        for n in lst:
            if only and n not in only: continue
            if n not in files: print('missing', n); continue
            print(hx, n, recolor(os.path.join(HERE, n + '.png'), hx))

if __name__ == '__main__': main()
