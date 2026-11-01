# The Crown tool's Bangs preset: its curves taken from the bangs model (an OBJ from Cinema 4D: a quarter of a sphere —
# its pole on top, its lower edge round the front, its back cut with a small chamfer inwards). The parting is the pole
# (a short line across there, every curve ending on its middle); the side cuts run from it down the chamfered back edges
# to the lower corners; next to them the sphere's back edge (the chamfer's outer edge, so the chamfer is a wall of its
# own); between them the sphere's meridians down to the lower edge, evenly (every other one). The lower connector: the
# back arch from its top down one side, the lower edge, up the other side to the top again — the arch unbroken over the
# top, the chamfer's curves fanning across it from the pole. Scaled and moved onto the crown reference's head (scene/Crown.cpp TPART /
# TRAD): the pole on the top of the head, the lower edge at the brows.
# Usage: py -I bangs_template.py <bangs.obj> → the C++ constants for Crown.cpp (BPART, BCORN, BLOW, BRAD).
import collections, math, sys

V, F = [], []
for l in open(sys.argv[1]):
    p = l.split()
    if p and p[0] == 'v': V.append(tuple(map(float, p[1:4])))
    if p and p[0] == 'f': F.append([int(x.split('/')[0]) - 1 for x in p[1:]])
E = collections.Counter(); adj = collections.defaultdict(set)
for f in F:
    for i in range(len(f)):
        a, b = f[i], f[(i + 1) % len(f)]; E[(min(a, b), max(a, b))] += 1; adj[a].add(b); adj[b].add(a)
bd = collections.defaultdict(list)
for (a, b), c in E.items():
    if c == 1: bd[a].append(b); bd[b].append(a)
pole = max(adj, key=lambda v: len(adj[v]))
# the boundary loop, from its top (the back cut's middle)
top = max(bd, key=lambda v: V[v][1]); loop = [top]; prev = None
while True:
    n = [x for x in bd[loop[-1]] if x != prev][0]
    if n == top: break
    prev = loop[-1]; loop.append(n)
corners = [v for v in loop if len(adj[v]) == 2]
c0, c1 = sorted(corners, key=lambda v: -V[v][0])           # (+X first)
i0, i1 = loop.index(c0), loop.index(c1)
if i0 > i1: loop = [loop[0]] + loop[1:][::-1]; i0, i1 = loop.index(c0), loop.index(c1)   # (the other way round, still from the top)
lower = loop[i0:i1 + 1]                                     # the lower edge, corner to corner (+X → −X)
backP, backN = list(reversed(loop[1:i0 + 1])), loop[i1:]    # the back edges: corner → top, +X and −X (their top end next to the pole)

def dist(a, b): return math.dist(a, b)
def at(path, t):
    L = [0]
    for i in range(1, len(path)): L.append(L[-1] + dist(path[i], path[i - 1]))
    d = t * L[-1]
    for i in range(1, len(path)):
        if L[i] >= d: s = L[i] - L[i - 1]; f = (d - L[i - 1]) / s if s else 0; return tuple(path[i - 1][j] + (path[i][j] - path[i - 1][j]) * f for j in range(3))
    return path[-1]
def meridian(b):                                            # from a lower vertex up the sphere to the pole
    ang = math.atan2(V[b][0], V[b][2]); path = [b]
    while path[-1] != pole:
        if pole in adj[path[-1]]: path.append(pole); break
        up = [n for n in adj[path[-1]] if V[n][1] > V[path[-1]][1] + 1e-6]
        path.append(min(up, key=lambda n: abs(math.atan2(V[n][0], V[n][2]) - ang)))
    return [V[v] for v in path]
N = 5
S = 6.8 / max(abs(v[0]) for v in V)                         # (its width — the reference head's at the brows)
def place(p): return (p[0] * S, 145.45 + (p[1] - V[pole][1]) * S + V[pole][1] * S, -6.15 + p[2] * S)
inner = lower[1:-1]; mids = inner[2:-2:2]                    # (every other meridian, the middle one among them; not the ones next to the corners)
mids = [inner[0]] + mids + [inner[-1]]                      # (and the sphere's edge at the back: the chamfer's outer edge)
# the back arch (the chamfer's inner edge) goes on over the top, past the pole — not into it: the lower connector starts
# and ends at its top (an anchor on either side of the middle), down the arch, along the lower edge and up the other
# side. The chamfer — a wall between the arch and the sphere's back edge, held by curves from the pole across it: to the
# arch's top, its middle and the lower corner
def lerp(a, b, t): return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))
def arcT(path, k):                                          # the arc fraction of path[k]
    L = [0]
    for i in range(1, len(path)): L.append(L[-1] + dist(path[i], path[i - 1]))
    return L[k] / L[-1]
eps = 0.5
topP, topN = (V[top][0] + eps, V[top][1], V[top][2]), (V[top][0] - eps, V[top][1], V[top][2])
def chamfer(arch, outer):                                   # arch: top → corner (vertices); outer: the pole → the sphere's lower edge
    I = [V[v] for v in arch]; out = []
    for k in (len(arch) // 2, len(arch) - 1):
        tk = arcT(I, k)
        path = [lerp(at(outer, s_ * tk), at(I, s_ * tk), s_) for s_ in [j / 40 for j in range(41)]]
        path[-1] = I[k]; out.append(list(reversed(path)))   # (from the arch up to the pole)
    return out
archP, archN = loop[0:i0 + 1], [top] + loop[i1:][::-1]
outP, outN = meridian(inner[0])[::-1], meridian(inner[-1])[::-1]
chP, chN = chamfer(archP, outP), chamfer(archN, outN)
curves = [[topP, V[pole]]] + chP + [meridian(b) for b in mids] + chN[::-1] + [[topN, V[pole]]]   # (in the lower connector's order)
BRAD = [[place(at(c, k / (N - 1))) for k in range(N)] for c in curves]
BLOW = [place(topP)] + [place(V[v]) for v in loop[1:]] + [place(topN)]
P = place(V[pole]); w = 4 * S
BPART = [(P[0] + w, P[1], P[2]), (P[0] - w, P[1], P[2])]
f = lambda p: "{%.3f, %.3f, %.3f}" % p
print("const double BPART[][3] = {" + ", ".join(f(p) for p in BPART) + "};")
BCORN = [1 + loop[1:].index(c0), 1 + loop[1:].index(c1)]      # (the lower corners in BLOW: hard points)
print("const int BCORN[] = {%d, %d};" % tuple(BCORN))
print("const std::vector<Vec3>& BLOW() {\n  static const std::vector<Vec3> L{" + ", ".join(f(p) for p in BLOW) + "};\n  return L;\n}")
print("const std::vector<std::vector<Vec3>>& BRAD() {\n  static const std::vector<std::vector<Vec3>> R{")
print(",\n".join("    {" + ", ".join(f(p) for p in c) + "}" for c in BRAD) + "};\n  return R;\n}")
