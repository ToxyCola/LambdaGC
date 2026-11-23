#!/usr/bin/env python3
"""
hl2gc map converter.

Reads a map and its materials from the user's own Half-Life 2 install and writes a
GameCube-ready map pack (.hgm, big-endian) for the hl2gc engine. No Valve content is
shipped with hl2gc; every pack is generated locally from the user's purchased copy.

usage: convert.py <Half-Life 2/hl2 folder> <map name> <out.hgm> [--tex-budget-kb N] [--tex-cap N]
"""
import argparse
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bsp import (BSP, MASK_PLAYERSOLID, SURF_BUMPLIGHT, SURF_HINT, SURF_NODRAW, SURF_NOLIGHT, SURF_SKIP, SURF_SKY, SURF_SKY2D,  # noqa: E402
                 SURF_TRIGGER, SURF_WARP)
from gxtex import (GX_TF_IA8, encode_ia8, GX_TF_CMPR, GX_TF_RGB5A3, dxt1_to_cmpr, encode_cmpr,  # noqa: E402
                   encode_rgb5a3, pot_floor, resize_box)
from material import (KIND_ADDITIVE, KIND_ALPHATEST, KIND_BLEND, KIND_EYE, KIND_NODRAW, KIND_SKY,  # noqa: E402
                      KIND_TRANSLUCENT, KIND_WATER, KIND_CAMERA, load_material)
from props import Props  # noqa: E402
from npcs import NPCs  # noqa: E402
import lightstyles  # noqa: E402
import matfx  # noqa: E402
import shared_data  # noqa: E402
from vpk import GameFS  # noqa: E402
from vtf import FMT_DXT1, FMT_DXT1_ONEBITALPHA, VTF  # noqa: E402

PACK_VERSION = 6
LM_PAGE = 1024
VERT_FMT = ">3f2f2f4B"  # pos, base uv, lightmap uv, rgba (alpha = blend weight)
VERT_SIZE = struct.calcsize(VERT_FMT)
assert VERT_SIZE == 32

PRIM_FAN = 0
PRIM_TRIS = 1
FACEF_SKY = 1
FACEF_WATER = 2


def align(n, a=32):
    return (n + a - 1) // a * a


# ---------------------------------------------------------------- textures

class TexOut:
    def __init__(self, w, h, fmt, levels, data, clamp=False):
        self.w, self.h, self.fmt, self.levels, self.data, self.clamp = w, h, fmt, levels, data, clamp


def _mip_chain(rgba, w, h, encode, min_dim):
    out = []
    levels = 0
    img = rgba
    while True:
        out.append(encode(resize_box(img, w, h)))
        levels += 1
        if w <= min_dim or h <= min_dim:
            break
        img = resize_box(img, w, h)
        w, h = w // 2, h // 2
    return b"".join(out), levels


def build_texture(vtf: VTF, mode, cap, frame=0):
    """mode: 'opaque' | 'punch' | 'alpha'. Returns TexOut. frame: which frame of an animated VTF."""
    W, H = pot_floor(vtf.width), pot_floor(vtf.height)
    scale = max(1, max(W, H) // cap)
    w, h = max(8, W // scale), max(8, H // scale)
    if mode == "opaque" and vtf.fmt in (FMT_DXT1, FMT_DXT1_ONEBITALPHA) and vtf.width == W and vtf.height == H:
        # lossless path: pull the existing DXT1 mips straight out of the VTF
        level = int(math.log2(scale))
        chain, levels = [], 0
        while level < vtf.mips:
            mw, mh = vtf.mip_dims(level)
            if mw < 8 or mh < 8:
                break
            chain.append(dxt1_to_cmpr(vtf.raw_mip(level, frame), mw, mh))
            levels += 1
            level += 1
        if levels:
            return TexOut(w, h, GX_TF_CMPR, levels, b"".join(chain))
    # choose the smallest source mip that is still >= target to keep decoding cheap
    lvl = 0
    while lvl + 1 < vtf.mips and vtf.mip_dims(lvl + 1)[0] >= w and vtf.mip_dims(lvl + 1)[1] >= h:
        lvl += 1
    rgba = vtf.rgba(lvl, frame)
    if mode == "alpha":
        w, h = max(4, w // 2), max(4, h // 2)  # 16bpp format: spend half the resolution
        data, levels = _mip_chain(rgba, w, h, encode_rgb5a3, 4)
        return TexOut(w, h, GX_TF_RGB5A3, levels, data)
    enc = (lambda im: encode_cmpr(im, punch_alpha=True)) if mode == "punch" else encode_cmpr
    data, levels = _mip_chain(rgba, w, h, enc, 8)
    return TexOut(w, h, GX_TF_CMPR, levels, data)


class ImposterTex:
    """WindowImposter's view out of a window: its cubemap sampled along each direction through the glass
    (90 degrees across, as the face UVs project it), as Source's shader looks the cubemap up along the view."""
    SIZE = 64

    def __init__(self, vtf, out, right, up):
        from vtf import decode
        n = vtf.mip_dims(0)[0]
        faces = [decode(vtf.fmt, vtf.raw_mip(0, 0, k), n, n) for k in range(6)]
        S = self.SIZE
        t = (np.arange(S) + 0.5) / S * 2 - 1
        sx, ty = np.meshgrid(t, t)
        d = out[None, None, :] + right[None, None, :] * sx[..., None] - up[None, None, :] * ty[..., None]
        self.img = cube_sample(faces, d)
        self.width = self.height = S
        self.mips, self.frames, self.fmt = 1, 1, -1

    def mip_dims(self, level):
        return max(1, self.SIZE >> level), max(1, self.SIZE >> level)

    def rgba(self, level, frame=0):
        return self.img if level == 0 else self.img[:: 1 << level, :: 1 << level]


def cube_sample(faces, d):
    """faces: 6 (h,w,4) images in VTF order (+x,-x,+y,-y,+z,-z); d: (...,3) Source-axis directions, looked up
    as Source's shaders do (texCUBE with the world vector: D3D's cube face conventions)"""
    x, y, z = d[..., 0], d[..., 1], d[..., 2]
    ax, ay, az = np.abs(x), np.abs(y), np.abs(z)
    h, w = faces[0].shape[:2]
    out = np.zeros(d.shape[:-1] + (4,), np.uint8)
    conds = [(ax >= ay) & (ax >= az) & (x > 0), (ax >= ay) & (ax >= az) & (x <= 0),
             (ay > ax) & (ay >= az) & (y > 0), (ay > ax) & (ay >= az) & (y <= 0),
             (az > ax) & (az > ay) & (z > 0), (az > ax) & (az > ay) & (z <= 0)]
    for k, c in enumerate(conds):
        if not c.any():
            continue
        xs, ys, zs = x[c], y[c], z[c]
        sc, tc, ma = [(-zs, -ys, xs), (zs, -ys, -xs), (xs, zs, ys), (xs, -zs, -ys), (xs, -ys, zs), (-xs, -ys, -zs)][k]
        iu = np.clip((((sc / ma) + 1) * 0.5 * w).astype(int), 0, w - 1)
        iv = np.clip((((tc / ma) + 1) * 0.5 * h).astype(int), 0, h - 1)
        out[c] = faces[k][iv, iu]
    out[..., 3] = 255
    return out


def checker():
    a = np.zeros((8, 8, 4), np.uint8)
    a[...] = (255, 0, 255, 255)
    a[:4, :4] = a[4:, 4:] = (0, 0, 0, 255)
    return TexOut(8, 8, GX_TF_CMPR, 1, encode_cmpr(a))


# ---------------------------------------------------------------- lightmaps

def rgbexp_to_rgb(raw):
    """ColorRGBExp32 -> 8-bit gamma-space colour at half intensity (TEV doubles it back)."""
    a = np.frombuffer(raw, np.uint8).reshape(-1, 4)
    exp = a[:, 3].astype(np.int8).astype(np.float32)
    lin = a[:, :3].astype(np.float32) * np.exp2(exp)[:, None] / 255.0
    g = np.power(np.clip(lin, 0, None), 1 / 2.2)
    return np.clip(g * 0.5 * 255 + 0.5, 0, 255).astype(np.uint8)


def pack_lightmaps(blocks, size=LM_PAGE, first=None):
    """Shelf-pack lightmap blocks (sorted tallest first) into pages.

    Each block gets a 1-luxel edge border and is padded out to a 4x4 grid so that no CMPR
    block ever straddles two faces. Returns (pages, placements) where placements[i] =
    (page, x, y) of block i's first real luxel.
    """
    first = first or set()
    order = sorted(range(len(blocks)), key=lambda i: (i not in first, -blocks[i].shape[0], -blocks[i].shape[1]))
    pages = []
    place = [None] * len(blocks)
    x = y = shelf = 0
    page = None
    for i in order:
        h, w, _ = blocks[i].shape
        bw, bh = (w + 2 + 3) & ~3, (h + 2 + 3) & ~3
        if page is None or x + bw > size:
            x, y, shelf = 0, y + shelf, 0
        if page is None or y + bh > size:
            page = np.full((size, size, 3), 128, np.uint8)
            pages.append([page, 0])
            x = y = shelf = 0
        cell = np.pad(blocks[i], ((1, bh - h - 1), (1, bw - w - 1), (0, 0)), mode="edge")
        page[y : y + bh, x : x + bw] = cell
        place[i] = (len(pages) - 1, x + 1, y + 1)
        x += bw
        shelf = max(shelf, bh)
        pages[-1][1] = y + shelf
    return pages, place


# ---------------------------------------------------------------- geometry

def face_polygon(b, f):
    idx = []
    for i in range(f["firstedge"], f["firstedge"] + f["numedges"]):
        se = b.surfedges[i]
        e = b.edges[abs(se)]
        idx.append(e[0] if se >= 0 else e[1])
    return b.vertexes[idx].astype(np.float64)


AIN_HULLS = 10            # NUM_HULLS in ai_hull.h
AIN_HULL_HUMAN = 0
NODE_GROUND, NODE_AIR, NODE_CLIMB = 2, 3, 4


def nav_chunks(fs, map_name, chunk, log=print):
    """The map's compiled AI node graph (maps/graphs/<map>.ain, CAI_NetworkManager's format, version 37):
    ground nodes and the links a human-sized hull can walk, for NPC pathfinding.
    AINN: u32 count, then per node f32 pos[3], u8 type, u8 pad, u16 firstLink, u16 numLinks, u16 pad
    AINL: per link end u16 node, u8 moves (bit 0 walk, 1 jump, 2 fly, 3 climb), u8 pad"""
    d = fs.read(f"maps/graphs/{map_name}.ain")
    if not d or len(d) < 12:
        return
    ver, _mapver, n = struct.unpack_from("<iii", d, 0)
    if ver != 37:
        log(f"  node graph: version {ver}, skipped")
        return
    o = 12
    nodes = []
    for _ in range(n):
        x, y, z, _yaw = struct.unpack_from("<4f", d, o)
        offs = struct.unpack_from(f"<{AIN_HULLS}f", d, o + 16)
        ntype = d[o + 16 + 4 * AIN_HULLS]
        nodes.append(((x, y, z + offs[AIN_HULL_HUMAN]), ntype))
        o += 16 + 4 * AIN_HULLS + 1 + 2 + 2
    (nl,) = struct.unpack_from("<i", d, o)
    o += 4
    adj = [[] for _ in range(n)]
    for _ in range(nl):
        a, b_ = struct.unpack_from("<hh", d, o)
        moves = d[o + 4 + AIN_HULL_HUMAN]
        o += 4 + AIN_HULLS
        if 0 <= a < n and 0 <= b_ < n and moves:
            adj[a].append((b_, moves))
            adj[b_].append((a, moves))
    out_n, out_l = [], []
    for (p, t), links in zip(nodes, adj):
        out_n.append(struct.pack(">3fBBHHH", *p, t, 0, len(out_l), len(links), 0))
        out_l.extend(struct.pack(">HBB", b_, m, 0) for b_, m in links)
    chunk(b"AINN", struct.pack(">I", n) + b"".join(out_n))
    chunk(b"AINL", b"".join(out_l))
    log(f"  node graph: {n} nodes, {len(out_l) // 2} links")


def tex_uv(ti, p, tw, th):
    t = ti["tex"]
    s = p @ t[0, :3] + t[0, 3]
    u = p @ t[1, :3] + t[1, 3]
    return s / max(tw, 1), u / max(th, 1)


def lm_st(ti, f, p):
    l = ti["lm"]
    s = p @ l[0, :3] + l[0, 3] - f["lmMins"][0]
    t = p @ l[1, :3] + l[1, 3] - f["lmMins"][1]
    return s, t


def displacement(b, f, d, poly):
    """Tessellate a displacement.

    Returns (positions, base positions, alpha, triangle indices, grid params). Grid params (N,2) hold
    (column, row) in 0..1; Source maps them onto the face's lightmap as (col * lmSize[0], row * lmSize[1])
    (see CCoreDispSurface::CalcLuxelCoords in the SDK's public/builddisp.cpp).
    """
    start = d["startPos"].astype(np.float64)
    c = int(np.argmin(((poly - start) ** 2).sum(1)))
    pts = np.roll(poly, -c, axis=0)
    n = (1 << d["power"]) + 1
    t = np.linspace(0, 1, n)
    e0 = pts[0] + (pts[1] - pts[0]) * t[:, None]
    e1 = pts[3] + (pts[2] - pts[3]) * t[:, None]
    base = e0[:, None, :] + (e1 - e0)[:, None, :] * t[None, :, None]  # (i, j, 3)
    base = base.reshape(-1, 3)
    dv = b.dispverts[d["dispVertStart"] : d["dispVertStart"] + n * n]
    pos = base + dv["vec"].astype(np.float64) * dv["dist"][:, None]
    alpha = np.clip(dv["alpha"], 0, 255)
    grid = np.stack(np.meshgrid(t, t, indexing="ij"), -1).reshape(-1, 2)[:, ::-1]  # (col, row)
    tris = []
    for i in range(n - 1):
        for j in range(n - 1):
            a, bb, cc, dd = i * n + j, i * n + j + 1, (i + 1) * n + j, (i + 1) * n + j + 1
            if (i + j) & 1:
                tris += [a, cc, bb, bb, cc, dd]
            else:
                tris += [a, cc, dd, a, dd, bb]
    return pos, base, alpha, tris, grid


def leaves_touching_box(b, mins, maxs):
    out = []
    stack = [0]
    while stack:
        node = stack.pop()
        if node < 0:
            leaf = -1 - node
            if b.leafs[leaf]["cluster"] >= 0:
                out.append(leaf)
            continue
        n = b.nodes[node]
        pl = b.planes[n["planenum"]]
        nrm, dist = pl["normal"], pl["dist"]
        near = np.where(nrm >= 0, mins, maxs)  # corner furthest behind the plane
        far = np.where(nrm >= 0, maxs, mins)   # corner furthest in front
        if far @ nrm - dist >= 0:
            stack.append(int(n["children"][0]))
        if near @ nrm - dist < 0:
            stack.append(int(n["children"][1]))
    return out


# ---------------------------------------------------------------- main

def convert(game_dir, map_name, out_path, tex_budget_kb=6144, tex_cap=256, log=print, shared=None, far_lods=True):
    fs = GameFS(game_dir)
    with open(os.path.join(game_dir, "maps", map_name + ".bsp"), "rb") as fh:
        b = BSP(fh.read())
    fs.pak = b.pakfile()
    log(f"{map_name}: BSP v{b.version}, {len(b.faces)} faces, {len(b.texnames)} materials")
    # the vehicle the player brings along from the previous level (Source carries it across the change):
    # every map of the airboat and buggy chapters gets one waiting, hidden, for the engine to place
    carried = None
    if map_name.startswith("d1_canals_") and map_name >= "d1_canals_05" or map_name == "d1_eli_01":
        carried = ("prop_vehicle_airboat", "models/airboat.mdl")
    elif map_name.startswith("d2_coast_"):
        carried = ("prop_vehicle_jeep", "models/buggy.mdl")
    if carried:
        pairs = [("classname", carried[0]), ("targetname", "_carried_vehicle"), ("model", carried[1]),
                 ("origin", "0 0 -30000"), ("angles", "0 0 0"), ("_carried", "1")]
        b.entities.append(dict(pairs, _pairs=pairs))

    # ---- materials and textures
    mats = [load_material(fs, n) for n in b.texnames]
    props = Props(b, fs, mats, log, far_lods)  # appends model materials to mats
    if shared is None:
        shared = shared_data.Shared(game_dir, os.path.dirname(os.path.dirname(os.path.abspath(out_path))), fs)
    npcs = NPCs(b, fs, mats, map_name, shared.scripts, shared.captions, shared.scenes, shared.bank, log)
    worldspawn = next((e for e in b.entities if e.get("classname") == "worldspawn"), {})
    skyname = worldspawn.get("skyname", "sky_day01_01")

    # ---- WindowImposter windows: the view out of each is its cubemap's face in the direction it looks out,
    # projected through the window from a point inside the room (the PC shader looks the cubemap up along the
    # view direction; this is that, for a viewer a few metres back from the glass)
    imposter = {}  # material index -> (face k, eye, out, right, up)
    imposter_dirs = {}  # texture path -> (out, right, up)
    for mi, m in enumerate(mats[:len(b.texnames)]):
        if m.shader != "windowimposter" or not m.envmap:
            continue
        fis = [fi for fi, f in enumerate(b.faces) if b.texinfo[f["texinfo"]]["texdata"] == mi]
        if not fis:
            continue
        nsum = np.zeros(3)
        pts = []
        for fi in fis:
            f = b.faces[fi]
            n = np.array(b.planes[f["planenum"]][0], dtype=np.float64)
            if f["side"]:
                n = -n
            poly = face_polygon(b, f)
            # (the panes, not the frame's bevels: each face's normal weighted by its area squared)
            area = 0.5 * float(np.linalg.norm(np.cross(poly[1:-1] - poly[0], poly[2:] - poly[0]).sum(0))) if len(poly) >= 3 else 0.0
            nsum += n * area * area
            pts.extend(poly.tolist())
        if np.linalg.norm(nsum) < 1e-6:
            continue
        n = nsum / np.linalg.norm(nsum)
        out = -n  # the glass faces into the room: looking out is against its normal
        pts = np.array(pts)
        centre = (pts.min(0) + pts.max(0)) * 0.5
        ax = int(np.argmax(np.abs(out)))
        k = ax * 2 + (0 if out[ax] > 0 else 1)  # VTF cubemap faces: +x, -x, +y, -y, +z, -z
        up = np.array([0.0, 0.0, 1.0]) if ax != 2 else np.array([1.0, 0.0, 0.0])
        right = np.cross(out, up)
        right /= max(np.linalg.norm(right), 1e-9)
        up = np.cross(right, out)
        imposter[mi] = (k, centre + n * 160.0, out, right, up)
        m.basetexture = f"{m.envmap}@{mi}"
        imposter_dirs[m.basetexture] = (out, right, up)

    tex_requests = {}  # path -> mode
    eye_tex = set()  # iris textures: clamped, the iris is projected and must not repeat

    def want(path, mode):
        if not path:
            return
        prev = tex_requests.get(path)
        rank = {"opaque": 0, "punch": 1, "alpha": 2}
        if prev is None or rank[mode] > rank[prev]:
            tex_requests[path] = mode

    for m in mats:
        mode = {KIND_ALPHATEST: "punch", KIND_TRANSLUCENT: "alpha", KIND_ADDITIVE: "alpha"}.get(m.kind, "opaque")
        if m.kind in (KIND_NODRAW, KIND_SKY, KIND_CAMERA):
            continue
        want(m.basetexture, mode)
        if m.kind == KIND_BLEND:
            want(m.basetexture2, "opaque")
        if m.kind == KIND_EYE:
            want(m.basetexture2, "alpha")
            eye_tex.add(m.basetexture2)

    # material effects (matfx.py): proxies, unlit / two-texture / self-illum shaders, animated frames
    fx_compiled = {}

    def vtf_frames(name):
        raw = fs.read(f"materials/{name}.vtf")
        return struct.unpack_from("<H", raw, 24)[0] if raw and raw[:4] == b"VTF\0" else 1

    for mi, m in enumerate(mats):
        if m.kind in (KIND_NODRAW, KIND_SKY, KIND_WATER, KIND_EYE) or m.missing:
            continue
        raw = fs.read(f"materials/{m.name}.vmt")
        if not raw:
            continue
        r = matfx.compile_material(raw.decode("latin-1", "replace"), m.shader, vtf_frames)
        if r is None:
            continue
        prog, nframes, tex2 = r
        mode = {KIND_ALPHATEST: "punch", KIND_TRANSLUCENT: "alpha", KIND_ADDITIVE: "alpha"}.get(m.kind, "opaque")
        if nframes > 1 and m.basetexture:
            for k in range(nframes):
                want(f"{m.basetexture}#{k}", mode)
        if tex2:
            want(tex2, "alpha")
        if prog.flags & matfx.FX_SELFILLUM and m.basetexture:
            want(m.basetexture, "alpha")  # (its alpha is the glow mask)
        fx_compiled[mi] = (prog, nframes, tex2)
    sky_paths = []
    for side in ("rt", "lf", "bk", "ft", "up", "dn"):
        sm = load_material(fs, f"skybox/{skyname}{side}")
        sky_paths.append(sm.basetexture)
        want(sm.basetexture, "opaque")

    # env_sprite / env_lightglow materials (sprites/*.vmt)
    sprite_base = {}  # entity index -> basetexture
    sprite_beam = {}  # point_spotlight beams
    for ei, e in enumerate(b.entities):
        cls = e.get("classname", "")
        if cls not in ("env_sprite", "env_sprite_oriented", "env_glow", "env_lightglow", "point_spotlight"):
            continue
        if cls == "point_spotlight":
            # CPointSpotlight: a beam (sprites/glow_test02) with a halo (sprites/light_glow03)
            beam = load_material(fs, "sprites/glow_test02")
            if beam.basetexture:
                sprite_beam[ei] = beam.basetexture
                want(beam.basetexture, "alpha")
        mdl = e.get("model", "") if cls != "point_spotlight" else ""
        mdl = mdl or ("sprites/light_glow03.vmt" if cls in ("env_lightglow", "point_spotlight") else "")
        mdl = mdl.lower().replace("\\", "/")
        if mdl.startswith("materials/"):
            mdl = mdl[len("materials/"):]
        mdl = os.path.splitext(mdl)[0]
        if not mdl:
            continue
        sm = load_material(fs, mdl)
        if sm.basetexture:
            sprite_base[ei] = sm.basetexture
            want(sm.basetexture, "alpha")

    vtfs = {}
    frame_of = {}  # "name#k": frame k of an animated texture (matfx.py)
    for path in tex_requests:
        src = path
        face = -1
        if "@" in path:
            src, k = path.rsplit("@", 1)
            face = 0
        if "#" in src:
            src, k = src.rsplit("#", 1)
            frame_of[path] = int(k)
        raw = fs.read(f"materials/{src}.vtf")
        if raw is None:
            continue
        try:
            v = VTF(raw)
            vtfs[path] = ImposterTex(v, *imposter_dirs[path]) if face >= 0 and v.faces >= 6 and path in imposter_dirs else v
        except Exception as ex:  # noqa: BLE001
            log(f"  warn: {path}: {ex}")

    # fit the texture budget: halve whichever texture currently costs the most, one at a time
    caps = {path: tex_cap for path in vtfs}
    textures = {path: build_texture(v, tex_requests[path], tex_cap, frame_of.get(path, 0)) for path, v in vtfs.items()}
    total = sum(len(t.data) for t in textures.values())
    while total > tex_budget_kb * 1024:
        path = max((p for p in textures if caps[p] > 32), key=lambda p: len(textures[p].data), default=None)
        if path is None:
            break
        caps[path] //= 2
        old = len(textures[path].data)
        textures[path] = build_texture(vtfs[path], tex_requests[path], caps[path], frame_of.get(path, 0))
        total += len(textures[path].data) - old
    reduced = sum(1 for p in caps if caps[p] < tex_cap)
    log(f"  textures: {len(textures)} -> {total / 1024:.0f} KB (cap {tex_cap}px, {reduced} reduced to fit "
        f"{tex_budget_kb} KB)")

    tex_list = [checker()]
    tex_index = {}
    for path, t in textures.items():
        tex_index[path] = len(tex_list)
        tex_list.append(t)
    for i in list(sky_paths) + list(eye_tex):
        if i in tex_index:
            tex_list[tex_index[i]].clamp = True

    def tidx(path):
        if not path:
            return -1
        return tex_index.get(path, 0)

    props.sprite_keys = {}
    for ei, base in sprite_base.items():
        v = vtfs.get(base)
        if v is not None:
            props.sprite_keys[ei] = [("_tex", str(tidx(base))), ("_size", "%d %d" % (v.width, v.height))]
            if ei in sprite_beam and sprite_beam[ei] in vtfs:
                props.sprite_keys[ei].append(("_tex2", str(tidx(sprite_beam[ei]))))

    fx_records = []
    for mi, (prog, nframes, tex2) in fx_compiled.items():
        m = mats[mi]
        f0 = -1
        if nframes > 1 and m.basetexture:
            idx = [tex_index.get(f"{m.basetexture}#{k}", -1) for k in range(nframes)]
            if all(i == idx[0] + k for k, i in enumerate(idx)) and idx[0] > 0:
                f0 = idx[0]
            else:
                nframes = 1
        fx_records.append((mi, prog, f0, nframes, tidx(tex2) if tex2 else -1))
    if fx_records:
        log(f"  material effects: {len(fx_records)} materials "
            f"({sum(1 for r in fx_records if r[3] > 1)} animated, {sum(len(r[1].ops) for r in fx_records)} ops)")

    mat_records = []
    for m in mats:
        mat_records.append((tidx(m.basetexture) if m.kind not in (KIND_NODRAW, KIND_SKY) else -1,
                            tidx(m.basetexture2) if m.kind in (KIND_BLEND, KIND_EYE) else -1,
                            m.kind, *m.color))

    # ---- pick drawable faces and pack their lightmaps
    face_remap = np.full(len(b.faces), 0xFFFF, np.int32)
    disp_by_face = {int(d["mapFace"]): d for d in b.dispinfo}
    drawable = []
    lm_blocks = []
    lm_of_face = {}
    lm_layers = {}  # lightmap block -> [(style, delta)]: switchable / flickering lights (lightstyles.py)
    skipped = 0
    for fi, f in enumerate(b.faces):
        ti = b.texinfo[f["texinfo"]]
        flags = int(ti["flags"])
        mat = mats[ti["texdata"]]
        if flags & (SURF_NODRAW | SURF_SKIP | SURF_HINT | SURF_TRIGGER) or mat.kind == KIND_NODRAW:
            skipped += 1
            continue
        if flags & (SURF_SKY | SURF_SKY2D) or mat.kind == KIND_SKY:
            skipped += 1  # the engine draws the skybox cube behind everything instead
            continue
        drawable.append(fi)
        if f["lightofs"] >= 0 and not (flags & SURF_NOLIGHT):
            lw, lh = int(f["lmSize"][0]) + 1, int(f["lmSize"][1]) + 1
            raw = b.lighting[f["lightofs"] : f["lightofs"] + lw * lh * 4]
            lm_of_face[fi] = len(lm_blocks)
            if f["styles"][1] != 255:
                ls = lightstyles.face_layers(b.lighting, f, bool(flags & SURF_BUMPLIGHT))
                if ls:
                    lm_layers[len(lm_blocks)] = ls
            lm_blocks.append(rgbexp_to_rgb(raw).reshape(lh, lw, 3))

    # (faces with light styles first: they end up together at the top of the first page, so their
    # layer pages stay small)
    pages, placement = pack_lightmaps(lm_blocks, first=set(lm_layers))
    lmpage_base = len(tex_list)
    page_h = []
    for p, (img, used) in enumerate(pages):
        hgt = (used + 7) & ~7
        page_h.append(hgt)
        rgba = np.dstack([img[:hgt], np.full((hgt, LM_PAGE), 255, np.uint8)])
        tex_list.append(TexOut(LM_PAGE, hgt, GX_TF_CMPR, 1, encode_cmpr(rgba), clamp=True))
        log(f"  lightmap page {p}: {LM_PAGE}x{hgt} ({len(lm_blocks)} blocks)")

    def _style_tex(rgba, w, h):
        tex_list.append(TexOut(w, h, GX_TF_CMPR, 1, encode_cmpr(rgba), clamp=True))
        return len(tex_list) - 1
    style_pg, style_where = lightstyles.style_pages(lm_layers, placement, page_h, LM_PAGE, _style_tex)
    if style_pg:
        log(f"  light styles: {len(lm_layers)} faces, {len(style_pg)} layer pages "
            f"({sum(LM_PAGE * (page_h[0] / v) / 2 for _, _, v in style_pg) / 1024:.0f} KB)")

    # ---- monitor screens: one camera picture across each screen, however many faces it's made of
    # (Source maps _rt_Camera over the whole surface; a curved screen is many thin faces)
    cam_range = {}
    cam_faces = [fi for fi in drawable if mats[b.texinfo[b.faces[fi]["texinfo"]]["texdata"]].kind == KIND_CAMERA
                 and b.faces[fi]["dispinfo"] < 0]
    if cam_faces:
        parent = {fi: fi for fi in cam_faces}

        def root(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        owner = {}
        polys = {}
        for fi in cam_faces:
            poly = face_polygon(b, b.faces[fi])
            polys[fi] = poly
            td_i = b.texinfo[b.faces[fi]["texinfo"]]["texdata"]
            for p in poly:
                key = (td_i, round(float(p[0])), round(float(p[1])), round(float(p[2])))
                if key in owner:
                    ra, rb = root(owner[key]), root(fi)
                    if ra != rb:
                        parent[ra] = rb
                else:
                    owner[key] = fi
        groups = {}
        for fi in cam_faces:
            ti = b.texinfo[b.faces[fi]["texinfo"]]
            td = b.texdata[ti["texdata"]]
            u, v = tex_uv(ti, polys[fi], td["w"], td["h"])
            g = groups.setdefault(root(fi), [1e30, -1e30, 1e30, -1e30])
            g[0], g[1] = min(g[0], float(u.min())), max(g[1], float(u.max()))
            g[2], g[3] = min(g[2], float(v.min())), max(g[3], float(v.max()))
        for fi in cam_faces:
            cam_range[fi] = groups[root(fi)]
        log(f"  monitor screens: {len(groups)} ({len(cam_faces)} faces)")

    # ---- geometry
    verts = []
    faces_out = []
    disp_bounds = []  # (out face, mins, maxs): displacements aren't in the BSP's leaf lists
    for fi in drawable:
        f = b.faces[fi]
        ti = b.texinfo[f["texinfo"]]
        flags = int(ti["flags"])
        td = b.texdata[ti["texdata"]]
        mat = mats[ti["texdata"]]
        poly = face_polygon(b, f)
        page, ox, oy = placement[lm_of_face[fi]] if fi in lm_of_face else (-1, 0, 0)

        def lm_uv(p):
            if page < 0:
                return np.full(len(p), 0.5), np.full(len(p), 0.5)
            s, t = lm_st(ti, f, p)
            return (ox + s + 0.5) / LM_PAGE, (oy + t + 0.5) / page_h[page]

        first = len(verts)
        fflags = FACEF_WATER if (mat.kind == KIND_WATER or flags & SURF_WARP) else 0
        if f["dispinfo"] >= 0 and fi in disp_by_face and len(poly) == 4:
            pos, base, alpha, tris, gp = displacement(b, f, disp_by_face[fi], poly)
            u, v = tex_uv(ti, base, td["w"], td["h"])
            if page >= 0:
                lu = (ox + gp[:, 0] * f["lmSize"][0] + 0.5) / LM_PAGE
                lv = (oy + gp[:, 1] * f["lmSize"][1] + 0.5) / page_h[page]
            else:
                lu, lv = lm_uv(base)
            grid = [(pos[k], u[k], v[k], lu[k], lv[k], alpha[k]) for k in range(len(pos))]
            for k in tris:
                p, uu, vv, a, bb, al = grid[k]
                verts.append((*p, uu, vv, a, bb, 255, 255, 255, int(al)))
            prim = PRIM_TRIS
        else:
            u, v = tex_uv(ti, poly, td["w"], td["h"])
            if int(ti["texdata"]) in imposter:
                # the cubemap face, looked at from the room through this point of the glass
                _k, eye, out, right, up = imposter[int(ti["texdata"])]
                d = poly - eye
                fwd = np.maximum(d @ out, 1.0)
                u = 0.5 + 0.5 * (d @ right) / fwd
                v = 0.5 - 0.5 * (d @ up) / fwd
            if mat.kind == KIND_CAMERA and fi in cam_range:
                # Source draws _rt_Camera with the face's own texture coordinates, wrapping: a curved
                # screen's strips each take their slice (u .31-.35, .35-.39...). Keep them near zero so
                # the s16 texture coordinates don't overflow; the engine's camera texture repeats.
                u = u - math.floor(float(u.min()))
                v = v - math.floor(float(v.min()))
            lu, lv = lm_uv(poly)
            for k in range(len(poly)):
                verts.append((*poly[k], u[k], v[k], lu[k], lv[k], 255, 255, 255, 255))
            prim = PRIM_FAN
        if prim == PRIM_TRIS:
            disp_bounds.append((len(faces_out), pos.min(0), pos.max(0)))
        face_remap[fi] = len(faces_out)
        lmtex = lmpage_base + page if page >= 0 else 0xFFFF
        faces_out.append((first, len(verts) - first, int(ti["texdata"]), lmtex, prim, fflags))

    log(f"  faces: {len(faces_out)} drawn, {skipped} skipped, {len(verts)} vertices")
    if len(faces_out) >= 0xFFFF:
        raise SystemExit("map has more than 65534 drawable faces; the pack format uses 16-bit face ids")

    # ---- every brush model (index = the BSP's model number, 0 = world): faces + collision subtree.
    # Entities pick theirs by "model" "*N"; the engine moves/hides/toggles them at runtime.
    bmodels, bmfaces = [], []
    for mi in range(len(b.models)):
        m = b.models[mi]
        ids = [] if mi == 0 else [int(face_remap[i]) for i in range(m["firstface"], m["firstface"] + m["numfaces"])
                                  if face_remap[i] != 0xFFFF]
        bmodels.append(struct.pack(">i3f3fII", int(m["headnode"]), *m["mins"], *m["maxs"], len(bmfaces), len(ids)))
        bmfaces += ids

    # Source spawns the player at the info_player_start flagged "Master" when there are several
    starts = [e for e in b.entities if e.get("classname") == "info_player_start"]
    spawn = next((e for e in starts if int(e.get("spawnflags", "0") or 0) & 1), starts[0] if starts else None)
    if spawn is None:
        spawn = next((e for e in b.entities if e.get("classname", "").startswith("info_player")), {})
    sp_o = [float(x) for x in spawn.get("origin", "0 0 0").split()]
    sp_a = [float(x) for x in spawn.get("angles", "0 0 0").split()]
    # (v2: feet position; the engine adds eye height)

    # ---- write pack
    chunks = []

    def chunk(tag, payload):
        chunks.append((tag, payload))

    # ---- water: per water material, its fog/tint and a distortion texture made from its normal map
    # (the engine perturbs its refraction lookup with it, Source's water look on GX indirect texturing)
    water_recs = []
    dudv_index = {}
    for mi, m in enumerate(mats):
        if m.kind != KIND_WATER or not m.water:
            continue
        w = m.water
        ti = -1
        if w["normal"] in dudv_index:
            ti = dudv_index[w["normal"]]
        else:
            raw = fs.read(f"materials/{w['normal']}.vtf") or fs.read("materials/dev/water_normal.vtf")
            if raw:
                try:
                    v = VTF(raw)
                    lvl = 0
                    while lvl + 1 < v.mips and v.mip_dims(lvl + 1)[0] >= 64:
                        lvl += 1
                    rgba = resize_box(v.rgba(lvl), 64, 64)
                    t_ = TexOut(64, 64, GX_TF_IA8, 1, encode_ia8(rgba[..., 0], rgba[..., 1]))
                    t_.clamp = False
                    ti = len(tex_list)
                    tex_list.append(t_)
                except Exception as ex:  # noqa: BLE001
                    log(f"  warn: water normal {w['normal']}: {ex}")
            dudv_index[w["normal"]] = ti
        water_recs.append(struct.pack(">Hh3B3BBxff", mi, ti, *w["fog"], *w["refracttint"],
                                      int(w["reflect"] * 255), w["fogstart"], w["fogend"]))
    if water_recs:
        log(f"  water materials: {len(water_recs)}")

    texdata = bytearray()
    texinfo = bytearray(struct.pack(">I", len(tex_list)))
    tex_offsets = []
    for t in tex_list:
        tex_offsets.append(len(texdata))
        texdata += t.data
        texdata += b"\0" * (align(len(texdata)) - len(texdata))
    for t, off in zip(tex_list, tex_offsets):
        texinfo += struct.pack(">HHBBBBII", t.w, t.h, t.fmt, t.levels, 1 if t.clamp else 0, 0, off, len(t.data))
    chunk(b"TEXI", bytes(texinfo))
    chunk(b"TEXD", bytes(texdata))
    chunk(b"MATS", struct.pack(">I", len(mat_records)) + b"".join(struct.pack(">hhBBBB", *r) for r in mat_records))
    chunk(b"WATR", struct.pack(">I", len(water_recs)) + b"".join(water_recs))
    # footsteps: each material's surface (scripts/surfaceproperties*.txt), an index into weapons.hgm's STEP
    from surfaces import Surfaces
    surf = Surfaces(fs)
    chunk(b"MSTP", bytes(min(255, surf.id_of(m.surfaceprop)) for m in mats))
    va = np.array(verts, dtype=np.float64).reshape(-1, 11)
    vb = np.zeros(len(va), dtype=[("p", ">f4", 3), ("uv", ">f4", 2), ("lm", ">f4", 2), ("c", "u1", 4)])
    vb["p"], vb["uv"], vb["lm"] = va[:, 0:3], va[:, 3:5], va[:, 5:7]
    vb["c"] = np.clip(va[:, 7:11], 0, 255).astype(np.uint8)
    chunk(b"VERT", vb.tobytes())
    matfx.chunks(chunk, fx_records)
    chunk(b"FACE", struct.pack(">I", len(faces_out)) + b"".join(struct.pack(">IHHHBB", *f) for f in faces_out))
    # light styles: the layers each styled face adds, and the styles' patterns and initial state
    style_recs = []
    for fi, bi in lm_of_face.items():
        if bi in lm_layers and face_remap[fi] != 0xFFFF:
            for slot, (sty, _) in enumerate(lm_layers[bi]):
                style_recs.append((int(face_remap[fi]), sty, style_where[(bi, slot)]))
    lightstyles.chunks_for(chunk, b.entities, style_pg, style_recs)
    chunk(b"PLAN", b"".join(struct.pack(">4f", *p["normal"], p["dist"]) for p in b.planes))
    chunk(b"NODE", b"".join(struct.pack(">iii", n["planenum"], *n["children"]) for n in b.nodes))
    # leaf face lists: the BSP's own lists plus displacements, which Source links to leaves at load time
    extra = [[] for _ in range(len(b.leafs))]
    disp_box = {f: (lo, hi) for f, lo, hi in disp_bounds}
    for out_face, mins, maxs in disp_bounds:
        for leaf in leaves_touching_box(b, mins - 1, maxs + 1):
            extra[leaf].append(out_face)
    leaf_recs, lf = [], []
    for li, l in enumerate(b.leafs):
        ids = [int(x) for x in face_remap[b.leaffaces[l["firstleafface"]: l["firstleafface"] + l["numleaffaces"]]]
               if x != 0xFFFF] + extra[li]
        mins, maxs = l["mins"].astype(np.int32), l["maxs"].astype(np.int32)
        for out_face in extra[li]:
            dmins, dmaxs = disp_box[out_face]
            mins = np.minimum(mins, np.floor(dmins)).astype(np.int32)  # grow bounds for frustum culling
            maxs = np.maximum(maxs, np.ceil(dmaxs)).astype(np.int32)
        mins, maxs = np.clip(mins, -32768, 32767), np.clip(maxs, -32768, 32767)
        leaf_recs.append(struct.pack(">hHIIHH3h3h", l["cluster"], len(ids), len(lf), int(l["firstleafbrush"]),
                                     int(l["numleafbrushes"]), 0, *mins, *maxs))
        lf += ids
    chunk(b"LEAF", b"".join(leaf_recs))
    chunk(b"LFAC", np.array(lf, ">u2").tobytes())

    # ---- collision: brushes (player-solid ones only matter), sides, leaf brush lists
    chunk(b"BRSH", b"".join(struct.pack(">IIi", int(br["firstside"]), int(br["numsides"]), int(br["contents"]))
                            for br in b.brushes))
    sides = np.zeros(len(b.brushsides), dtype=[("plane", ">u2"), ("bevel", ">u2")])
    # bevel flag in bit 0, the side's surface (footsteps; tools/surfaces.py) in the high byte, 255 = none
    from surfaces import Surfaces
    surf_ids = Surfaces(fs)
    td_surf = [min(253, surf_ids.id_of(m.surfaceprop)) for m in mats[:len(b.texnames)]]
    ti = b.brushsides["texinfo"].astype(np.int64)
    side_surf = np.full(len(ti), 255, np.int64)
    ok = (ti >= 0) & (ti < len(b.texinfo))
    tdi = np.where(ok, b.texinfo["texdata"][np.clip(ti, 0, len(b.texinfo) - 1)], -1)
    ok &= (tdi >= 0) & (tdi < len(td_surf))
    side_surf[ok] = np.array(td_surf, np.int64)[tdi[ok]]
    # sky faces get 254 (SURF_SKY in the engine): sunlight checks look for it, and bullets make no sound on it
    tflags = np.where(ti >= 0, b.texinfo["flags"][np.clip(ti, 0, len(b.texinfo) - 1)], 0)
    side_surf[(ti >= 0) & ((tflags & (SURF_SKY | SURF_SKY2D)) != 0)] = 254
    sides["plane"] = b.brushsides["planenum"]
    sides["bevel"] = (b.brushsides["bevel"] != 0).astype(np.int64) | (side_surf << 8)
    chunk(b"BSID", sides.tobytes())
    chunk(b"LBRS", b.leafbrushes.astype(">u2").tobytes())

    chunk(b"BMDL", struct.pack(">I", len(bmodels)) + b"".join(bmodels))
    chunk(b"BMFC", np.array(bmfaces, ">u2").tobytes())

    # level transitions: touchable trigger_changelevel volumes + every info_landmark
    marks = {e.get("targetname", "").lower(): [float(x) for x in e.get("origin", "0 0 0").split()[:3]]
             for e in b.entities if e.get("classname") == "info_landmark"}
    trig = []
    for e in b.entities:
        if e.get("classname") != "trigger_changelevel" or int(e.get("spawnflags", "0") or 0) & 2:
            continue
        mdl = e.get("model", "")
        if not mdl.startswith("*") or not e.get("map"):
            continue
        m = b.models[int(mdl[1:])]
        org = np.array([float(x) for x in e.get("origin", "0 0 0").split()[:3]])
        trig.append(struct.pack(">3f3f32s32s", *(m["mins"] + org), *(m["maxs"] + org),
                                e["map"].lower().encode()[:31], e.get("landmark", "").lower().encode()[:31]))
    chunk(b"TRIG", struct.pack(">I", len(trig)) + b"".join(trig))
    chunk(b"LMRK", struct.pack(">I", len(marks)) + b"".join(struct.pack(">32s3f", k.encode()[:31], *v)
                                                           for k, v in marks.items()))
    if b.vis:
        n, raw, offs = b.vis
        hdr = struct.pack(">I", n) + offs[:, 0].astype(">u4").tobytes()
        chunk(b"VISD", hdr + raw)
    props.chunks(chunk)
    nav_chunks(fs, map_name, chunk, log)
    props.entity_chunks(chunk, npcs, shared)
    shared.bank.save()
    sky = [tidx(p) if p in tex_index else -1 for p in sky_paths]
    chunk(b"SPWN", struct.pack(">3f3f6h", *sp_o[:3], *sp_a[:3], *sky))

    header_size = align(16 + 12 * len(chunks))
    out = bytearray(header_size)
    struct.pack_into(">4sII", out, 0, b"HGM1", PACK_VERSION, len(chunks))
    for i, (tag, payload) in enumerate(chunks):
        off = len(out)
        struct.pack_into(">4sII", out, 16 + i * 12, tag, off, len(payload))
        out += payload
        out += b"\0" * (align(len(out)) - len(out))
    with open(out_path, "wb") as fh:
        fh.write(out)
    sizes = {tag.decode(): len(p) for tag, p in chunks}
    log(f"  wrote {out_path}: {len(out) / 1048576:.2f} MB  " +
        ", ".join(f"{k} {v / 1024:.0f}K" for k, v in sizes.items()))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game_dir", help="path to 'Half-Life 2/hl2'")
    ap.add_argument("map")
    ap.add_argument("out")
    ap.add_argument("--tex-budget-kb", type=int, default=6144)
    ap.add_argument("--tex-cap", type=int, default=256)
    a = ap.parse_args()
    convert(a.game_dir, a.map, a.out, a.tex_budget_kb, a.tex_cap)


if __name__ == "__main__":
    main()
