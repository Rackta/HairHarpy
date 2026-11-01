# Traces the room signs' coverage grids (text, 0..255) into smooth closed outlines and writes them as a C++ header:
# usage: py tools/trace_signs.py hair.txt topo.txt src/app/RoomSigns.h (each .txt: the PNG's coverage, one row a line;
# a pixel = alpha * brightness, 0..255). Marching squares at a coverage threshold (sub-pixel by linear interpolation), Chaikin smoothing, Douglas-Peucker thinning.
import sys

def load(path):
    rows = [[int(x) for x in ln.split()] for ln in open(path) if ln.strip()]
    return rows

def contours(g, thr):
    h, w = len(g), len(g[0])
    # padded with zeros: every outline closes
    G = [[0.0] * (w + 2)] + [[0.0] + [float(v) for v in r] + [0.0] for r in g] + [[0.0] * (w + 2)]
    H, W = h + 2, w + 2
    def lerp(a, b, va, vb):
        t = (thr - va) / (vb - va) if vb != va else 0.5
        return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
    segs = []
    for y in range(H - 1):
        for x in range(W - 1):
            p = [(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)]
            v = [G[y][x], G[y][x + 1], G[y + 1][x + 1], G[y + 1][x]]
            idx = sum((1 << i) for i in range(4) if v[i] >= thr)
            if idx in (0, 15):
                continue
            e = {}
            for i in range(4):
                j = (i + 1) % 4
                if (v[i] >= thr) != (v[j] >= thr):
                    e[i] = lerp(p[i], p[j], v[i], v[j])
            ks = sorted(e)
            if len(ks) == 2:
                segs.append((e[ks[0]], e[ks[1]]))
            else:  # saddle: split by the centre's value
                c = sum(v) / 4
                if (c >= thr) == (v[0] >= thr):
                    segs += [(e[0], e[1]), (e[2], e[3])]
                else:
                    segs += [(e[0], e[3]), (e[1], e[2])]
    # link the segments into loops
    key = lambda q: (round(q[0], 6), round(q[1], 6))
    adj = {}
    for a, b in segs:
        adj.setdefault(key(a), []).append(b)
        adj.setdefault(key(b), []).append(a)
    used = set(); loops = []
    for a, b in segs:
        ek = (key(a), key(b))
        if ek in used or (ek[1], ek[0]) in used:
            continue
        loop = [a]; prev, cur = a, b; used.add(ek)
        for _ in range(len(segs) + 2):
            loop.append(cur)
            nxt = None
            for n in adj.get(key(cur), []):
                k2 = (key(cur), key(n))
                if k2 in used or (k2[1], k2[0]) in used:
                    continue
                nxt = n; used.add(k2); break
            if nxt is None:
                break
            prev, cur = cur, nxt
            if key(cur) == key(a):
                break
        if len(loop) >= 4:
            loops.append([(px - 1, py - 1) for px, py in loop])
    return loops

def chaikin(pts, n):
    for _ in range(n):
        out = []
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            out.append((a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25))
            out.append((a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75))
        pts = out
    return pts

def dp(pts, eps):
    if len(pts) < 3:
        return pts
    a, b = pts[0], pts[-1]
    dx, dy = b[0] - a[0], b[1] - a[1]; L = (dx * dx + dy * dy) ** 0.5 or 1e-12
    best, bi = 0, 0
    for i in range(1, len(pts) - 1):
        d = abs((pts[i][0] - a[0]) * dy - (pts[i][1] - a[1]) * dx) / L
        if d > best:
            best, bi = d, i
    if best <= eps:
        return [a, b]
    return dp(pts[:bi + 1], eps)[:-1] + dp(pts[bi:], eps)

def area(p):
    return sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p))) / 2

out = ["// The two rooms' signs as outlines (traced from room_hair.png / room_topo.png by scratch tooling): x, y in 0..1 of",
       "// the picture's square, loops one after another (their counts in *Loops) — filled odd-even. Drawn at any size, sharp.",
       "#pragma once", "", "namespace hm::signs {"]
for name, path, thr in (("Hair", sys.argv[1], 128), ("Topo", sys.argv[2], 60)):
    g = load(path); n = len(g)
    loops = contours(g, thr)
    res = []
    for L in loops:
        if abs(area(L)) < 4:
            continue
        S = chaikin(L, 3)
        h = len(S) // 2   # (a closed loop thinned as two halves: its ends would meet in one point)
        S = dp(S[:h + 1], 0.04)[:-1] + dp(S[h:] + [S[0]], 0.04)[:-1]
        res.append(S)
    flat = []; counts = []
    for S in res:
        counts.append(len(S))
        for x, y in S:
            flat.append("%.4ff,%.4ff" % (x / n, y / n))
    out.append("inline constexpr int k%sLoops[] = {%s};" % (name, ", ".join(map(str, counts))))
    out.append("inline constexpr int k%sLoopCount = %d;" % (name, len(counts)))
    lines = []
    for i in range(0, len(flat), 8):
        lines.append("  " + ", ".join(flat[i:i + 8]) + ",")
    out.append("inline constexpr float k%s[] = {" % name)
    out += lines
    out.append("};")
    print(name, len(counts), "loops", len(flat), "points", file=sys.stderr)
out.append("}  // namespace hm::signs")
open(sys.argv[3], "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
