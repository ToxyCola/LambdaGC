"""Render skinned NPCs straight from an .hgm (same math as the engine) to check models and poses."""
import struct, sys
import numpy as np
from PIL import Image, ImageDraw

def load(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('>I', d, 8)[0]
    ch = {}
    for i in range(n):
        tag, off, sz = struct.unpack_from('>4sII', d, 16 + i * 12)
        ch[tag] = (off, sz)
    return d, ch

def qm(x, y, z, w):
    return np.array([[1-2*(y*y+z*z), 2*(x*y-w*z), 2*(x*z+w*y)], [2*(x*y+w*z), 1-2*(x*x+z*z), 2*(y*z-w*x)],
                     [2*(x*z-w*y), 2*(y*z+w*x), 1-2*(x*x+y*y)]])

def pose(d, ch, skel, seq_local, frame):
    o, _ = ch[b'NSKL']
    nb, ns, fb, fs = struct.unpack_from('>HHII', d, o + 4 + 20 * skel)
    o, _ = ch[b'NBON']
    bones = [struct.unpack_from('>i12f', d, o + 52 * (fb + k)) for k in range(nb)]
    o, _ = ch[b'NSEQ']
    name, F, fl, fps, spd, ff, nbs = struct.unpack_from('>IHHffII', d, o + 28 * (fs + seq_local))
    frame = min(frame, F - 1)
    o2, _ = ch[b'NFRM']
    rec = np.frombuffer(d, dtype='>i2', count=nbs * 6, offset=o2 + 12 * (ff + frame * nbs)).reshape(nbs, 6).astype(float)
    q = rec[:, :3] / 32767
    w = np.sqrt(np.clip(1 - (q ** 2).sum(1), 0, 1))
    p = rec[:, 3:] / 64
    W, S = [None] * nb, []
    for i in range(nb):
        L = np.eye(4); L[:3, :3] = qm(*q[i], w[i]); L[:3, 3] = p[i]
        W[i] = W[bones[i][0]] @ L if bones[i][0] >= 0 else L
        P = np.eye(4); P[:3, :] = np.array(bones[i][1:]).reshape(3, 4)
        S.append(W[i] @ P)
    return S, F

def tris(d, ch, mdl, S):
    o, _ = ch[b'NMDL']
    skel, nbat, fbat = struct.unpack_from('>HHI', d, o + 4 + 24 * mdl)
    ob, _ = ch[b'NBAT']; ov, _ = ch[b'NVTX']; oi, _ = ch[b'NIDX']
    out = []
    for b in range(nbat):
        r = ob + 32 * (fbat + b)
        fv, nv, mat, fi, ni, nbn = struct.unpack_from('>IHHIIB', d, r)
        bl = d[r + 17:r + 27]
        V = []
        for v in range(nv):
            x, y, z = struct.unpack_from('>3f', d, ov + 20 * (fv + v)); m = d[ov + 20 * (fv + v) + 16]
            V.append((S[bl[m // 3]] @ np.array([x, y, z, 1]))[:3])
        V = np.array(V)
        idx = np.frombuffer(d, dtype='>u2', count=ni, offset=oi + 2 * fi).reshape(-1, 3)
        out.append(V[idx])
    return np.concatenate(out)

def draw(img, T, ox, oy, scale=3.0, yaw=0.0):
    c, s = np.cos(yaw), np.sin(yaw)
    R = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
    T = T @ R.T
    dr = ImageDraw.Draw(img)
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    light = np.array([0.5, -0.6, 0.6]); light /= np.linalg.norm(light)
    order = np.argsort(T[:, :, 0].mean(1))[::-1]  # view from +x: far (large x) first... draw near last
    for t in order:
        sh = int(60 + 180 * abs(n[t] @ light))
        pts = [(ox - v[1] * scale, oy - v[2] * scale) for v in T[t]]
        dr.polygon(pts, fill=(sh, sh, sh))

if __name__ == '__main__':
    d, ch = load(sys.argv[1])
    o, _ = ch[b'NMDL']; nm = struct.unpack_from('>I', d, o)[0]
    models = [int(x) for x in sys.argv[3:]] or list(range(min(nm, 6)))
    img = Image.new('RGB', (230 * 3, 240 * len(models)), (40, 44, 52))
    for row, mdl in enumerate(models):
        skel = struct.unpack_from('>H', d, o + 4 + 24 * mdl)[0]
        so, _ = ch[b'NSKL']
        idle, walk, run = struct.unpack_from('>hhh', d, so + 4 + 20 * skel + 12)
        for col, (sq, fr) in enumerate([(idle, 0), (walk, 4), (run, 3)]):
            if sq < 0: continue
            S, F = pose(d, ch, skel, sq, fr)
            draw(img, tris(d, ch, mdl, S), 115 + 230 * col, 225 + 240 * row, 3.0, yaw=-1.2)
    img.save(sys.argv[2])
