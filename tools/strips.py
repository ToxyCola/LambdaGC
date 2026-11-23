"""Triangle lists -> one triangle strip (joined with degenerate triangles).

The GameCube transforms every vertex it is sent (no post-transform cache), so a list costs 3 vertices a
triangle and a strip about 1.2-1.6. Nothing in hl2gc culls model triangles by winding, so strips are
joined without caring about their parity.
"""
import numpy as np


def stripify(indices):
    """indices: flat triangle list. Returns the strip (np.uint16 array), or None if it isn't shorter."""
    tris = np.asarray(indices, dtype=np.int64).reshape(-1, 3)
    n = len(tris)
    if n < 2:
        return None
    edge_tris = {}
    for t, (a, b, c) in enumerate(tris.tolist()):
        if a == b or b == c or a == c:
            continue
        for e in ((a, b), (b, c), (c, a)):
            edge_tris.setdefault((min(e), max(e)), []).append(t)
    used = np.zeros(n, dtype=bool)
    for t, (a, b, c) in enumerate(tris.tolist()):
        if a == b or b == c or a == c:
            used[t] = True  # (degenerate already: drop)
    tl = tris.tolist()

    def unused_neighbours(t):
        a, b, c = tl[t]
        k = 0
        for e in ((a, b), (b, c), (c, a)):
            for o in edge_tris.get((min(e), max(e)), ()):
                if o != t and not used[o]:
                    k += 1
        return k

    def next_tri(x, y):
        for o in edge_tris.get((min(x, y), max(x, y)), ()):
            if not used[o]:
                return o
        return -1

    strips = []
    # start from triangles with few free neighbours (corners and edges first): longer strips
    order = sorted(range(n), key=unused_neighbours)
    for start in order:
        if used[start]:
            continue
        used[start] = True
        a, b, c = tl[start]
        # pick the rotation whose last edge leads on
        best = (a, b, c)
        for r in ((a, b, c), (b, c, a), (c, a, b)):
            if next_tri(r[1], r[2]) >= 0:
                best = r
                break
        s = list(best)
        while True:
            o = next_tri(s[-2], s[-1])
            if o < 0:
                break
            used[o] = True
            x, y = s[-2], s[-1]
            z = [v for v in tl[o] if v != x and v != y]
            if not z:
                break
            s.append(z[0])
        strips.append(s)
    out = []
    for s in strips:
        if out:
            out += [out[-1], s[0]]
        out += s
    if len(out) >= len(tris) * 3 or len(out) > 0xFFFF:
        return None
    return np.asarray(out, dtype=np.uint16)
