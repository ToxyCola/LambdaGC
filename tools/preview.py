#!/usr/bin/env python3
"""
Software preview of an .hgm pack: reads the pack the way the GameCube engine does
(big-endian structs, GX-tiled textures, BSP leaf lookup, PVS) and renders a view to PNG.

usage: preview.py <pack.hgm> <out.png> [--pos x y z] [--ang pitch yaw] [--w 640 --h 480] [--novis]
"""
import argparse
import math
import struct

import numpy as np
from PIL import Image

GX_TF_RGB565, GX_TF_RGB5A3, GX_TF_CMPR = 0x4, 0x5, 0xE
MAT_TRANSLUCENT, MAT_ADDITIVE, MAT_WATER, MAT_BLEND, MAT_ALPHATEST = 2, 3, 4, 7, 1


def load(path):
    d = open(path, "rb").read()
    assert d[:4] == b"HGM1"
    n = struct.unpack_from(">I", d, 8)[0]
    ch = {}
    for i in range(n):
        tag, off, size = struct.unpack_from(">4sII", d, 16 + i * 12)
        ch[tag.decode()] = d[off: off + size]
    return ch


def c565(c):
    c = c.astype(np.int32)
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def decode_tex(fmt, w, h, data):
    if fmt == GX_TF_CMPR:
        b = np.frombuffer(data[: w * h // 2], np.uint8).reshape(h // 8, w // 8, 2, 2, 8)
        b = b.transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, 8)
        c0 = (b[..., 0].astype(np.int32) << 8) | b[..., 1]
        c1 = (b[..., 2].astype(np.int32) << 8) | b[..., 3]
        p0, p1 = c565(c0), c565(c1)
        four = c0 > c1
        p2 = np.where(four[..., None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
        p3 = np.where(four[..., None], (p0 + 2 * p1) // 3, 0)
        pal = np.stack([p0, p1, p2, p3], 2)
        a = np.stack([np.full(four.shape, 255), np.full(four.shape, 255), np.full(four.shape, 255),
                      np.where(four, 255, 0)], 2)
        rows = b[..., 4:8].astype(np.int32)
        idx = np.stack([(rows >> s) & 3 for s in (6, 4, 2, 0)], -1).reshape(h // 4, w // 4, 16)
        rgb = np.take_along_axis(pal, idx[..., None], 2)
        al = np.take_along_axis(a, idx, 2)
        px = np.concatenate([rgb, al[..., None]], -1)
        return px.reshape(h // 4, w // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4).astype(np.uint8)
    v = np.frombuffer(data[: w * h * 2], ">u2").reshape(h // 4, w // 4, 4, 4).transpose(0, 2, 1, 3).reshape(h, w)
    v = v.astype(np.int32)
    if fmt == GX_TF_RGB565:
        return np.dstack([c565(v), np.full((h, w), 255)]).astype(np.uint8)
    op = (v & 0x8000) != 0
    r = np.where(op, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
    g = np.where(op, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
    bb = np.where(op, (v & 31) * 255 // 31, (v & 15) * 17)
    a = np.where(op, 255, ((v >> 12) & 7) * 255 // 7)
    return np.dstack([r, g, bb, a]).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("out")
    ap.add_argument("--pos", type=float, nargs=3)
    ap.add_argument("--ang", type=float, nargs=2)
    ap.add_argument("--w", type=int, default=640)
    ap.add_argument("--h", type=int, default=480)
    ap.add_argument("--novis", action="store_true")
    ap.add_argument("--probe", type=int, nargs=2, help="print which face/material writes this pixel")
    a = ap.parse_args()
    ch = load(a.pack)

    ntex = struct.unpack_from(">I", ch["TEXI"])[0]
    texi = [struct.unpack_from(">HHBBBBII", ch["TEXI"], 4 + i * 16) for i in range(ntex)]
    texd = ch["TEXD"]
    cache = {}

    def tex(i):
        if i not in cache:
            w, h, fmt, lv, cl, _, off, size = texi[i]
            cache[i] = decode_tex(fmt, w, h, texd[off: off + size]).astype(np.float32) / 255.0
        return cache[i]

    nm = struct.unpack_from(">I", ch["MATS"])[0]
    mats = [struct.unpack_from(">hhBBBB", ch["MATS"], 4 + i * 8) for i in range(nm)]
    V = np.frombuffer(ch["VERT"], dtype=[("p", ">f4", 3), ("uv", ">f4", 2), ("lm", ">f4", 2), ("c", "u1", 4)])
    nf = struct.unpack_from(">I", ch["FACE"])[0]
    F = np.frombuffer(ch["FACE"][4:4 + nf * 12], dtype=[("first", ">u4"), ("n", ">u2"), ("mat", ">u2"),
                                                       ("lm", ">u2"), ("prim", "u1"), ("flags", "u1")])
    planes = np.frombuffer(ch["PLAN"], ">f4").reshape(-1, 4)
    nodes = np.frombuffer(ch["NODE"], ">i4").reshape(-1, 3)
    leafs = np.frombuffer(ch["LEAF"], dtype=[("cluster", ">i2"), ("num", ">u2"), ("first", ">u4"), ("fbrush", ">u4"),
                                             ("nbrush", ">u2"), ("pad", ">u2"), ("mins", ">i2", 3), ("maxs", ">i2", 3)])
    lfac = np.frombuffer(ch["LFAC"], ">u2")
    sp = struct.unpack_from(">3f3f6h", ch["SPWN"])
    eye = np.array(a.pos if a.pos else sp[0:3], np.float64)
    pitch, yaw = (a.ang if a.ang else sp[3:5])

    # BSP leaf lookup + PVS (same algorithm as world.c)
    node = 0
    while node >= 0:
        pl = planes[nodes[node][0]]
        node = nodes[node][1] if pl[:3] @ eye - pl[3] >= 0 else nodes[node][2]
    leaf = -1 - node
    cluster = leafs[leaf]["cluster"]
    vis = None
    if "VISD" in ch and not a.novis and cluster >= 0:
        vd = ch["VISD"]
        n = struct.unpack_from(">I", vd)[0]
        offs = np.frombuffer(vd[4:4 + 4 * n], ">u4")
        raw = vd[4 + 4 * n:]
        vis = np.zeros(n, bool)
        p, c = offs[cluster], 0
        while c < n:
            if raw[p] == 0:
                c += 8 * raw[p + 1]
                p += 2
                continue
            for bit in range(8):
                if c + bit < n and raw[p] >> bit & 1:
                    vis[c + bit] = True
            c += 8
            p += 1
    faces = set()
    for l in leafs:
        if l["num"] and (vis is None or (l["cluster"] >= 0 and vis[l["cluster"]])):
            for f in lfac[l["first"]: l["first"] + l["num"]]:
                if f != 0xFFFF:
                    faces.add(int(f))
    if False:
        md = ch["MODL"]
        nmod = struct.unpack_from(">I", md)[0]
        mf = np.frombuffer(md[4 + nmod * 20:], ">u2")
        for i in range(nmod):
            first, num = struct.unpack_from(">II", md, 4 + i * 20)
            faces.update(int(x) for x in mf[first: first + num])
    print(f"leaf {leaf} cluster {cluster}: {len(faces)} faces in PVS"
          f"{'' if vis is None else f', {vis.sum()}/{len(vis)} clusters visible'}")

    # camera
    p_, y_ = math.radians(pitch), math.radians(yaw)
    fwd = np.array([math.cos(p_) * math.cos(y_), math.cos(p_) * math.sin(y_), -math.sin(p_)])
    right = np.array([math.sin(y_), -math.cos(y_), 0.0])
    up = np.cross(right, fwd)
    W, H = a.w, a.h
    fy = 1 / math.tan(math.radians(59 / 2))
    fx = fy / (4 / 3)
    color = np.zeros((H, W, 3), np.float32)
    color[:] = np.array([0x30, 0x38, 0x48]) / 255
    depth = np.full((H, W), np.inf, np.float32)

    # sky: cube-map lookup per pixel using the quake/source side layout
    sky = sp[6:12]
    if all(s >= 0 for s in sky):
        ys, xs = np.mgrid[0:H, 0:W]
        dx = (xs + 0.5) / W * 2 - 1
        dy = 1 - (ys + 0.5) / H * 2
        d = fwd[None, None] + right[None, None] * (dx / fx)[..., None] + up[None, None] * (dy / fy)[..., None]
        # same face frames as render.c: (forward, right, texture-up) per face in pack order rt lf bk ft up dn
        frames = [((1, 0, 0), (0, -1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 1, 0), (0, 0, 1)),
                  ((0, 1, 0), (1, 0, 0), (0, 0, 1)), ((0, -1, 0), (-1, 0, 0), (0, 0, 1)),
                  ((0, 0, 1), (0, -1, 0), (-1, 0, 0)), ((0, 0, -1), (0, 1, 0), (-1, 0, 0))]
        for side, (f_, r_, u_) in enumerate(frames):
            dep = d @ np.array(f_, float)
            mask = dep > 0
            sdep = np.where(mask, dep, 1)
            s = (d @ np.array(r_, float)) / sdep
            t = (d @ np.array(u_, float)) / sdep
            mask &= (np.abs(s) <= 1) & (np.abs(t) <= 1)
            if not mask.any():
                continue
            img = tex(sky[side])
            th, tw = img.shape[:2]
            u = np.clip(((s + 1) / 2 * tw).astype(int), 0, tw - 1)
            v = np.clip(((1 - t) / 2 * th).astype(int), 0, th - 1)
            color[mask] = img[v[mask], u[mask], :3]

    def project(P):
        rel = P - eye
        z = rel @ fwd
        x = rel @ right
        y = rel @ up
        return x, y, z

    # ---- props: static (SPRP) and entity models (ENTS with _mdl), drawn as extra triangle lists
    prop_tris = []  # (P (n,3) world, uv (n,2), tri index array, tex index, tint rgb 0..1, kind)
    if "MDLS" in ch:

        nmd = struct.unpack_from(">I", ch["MDLS"])[0]
        mdls = [struct.unpack_from(">IIIIII3f3f", ch["MDLS"], 4 + i * 48) for i in range(nmd)]
        mesh = [struct.unpack_from(">IIHH", ch["MMSH"], i * 12) for i in range(len(ch["MMSH"]) // 12)]
        if "WLIT" in ch:  # 14-byte vertices: s16 positions in 1/2^frac (frac in the mesh records)
            mv16 = np.frombuffer(ch["MVTX"], dtype=[("p", ">i2", 3), ("uv", ">i2", 2), ("n", "i1", 3), ("pad", "u1")])
            mv = np.zeros(len(mv16), dtype=[("p", ">f4", 3), ("uv", ">i2", 2), ("n", "i1", 3), ("pad", "u1")])
            mv["uv"], mv["n"] = mv16["uv"], mv16["n"]
            for (fm_, nm2, fv_, nv_, _a, _b, *_r) in mdls:
                frac = mesh[fm_][3] if nm2 else 0
                mv["p"][fv_:fv_ + nv_] = mv16["p"][fv_:fv_ + nv_].astype(np.float64) / (1 << frac)
        elif "LCUB" in ch:
            mv = np.frombuffer(ch["MVTX"], dtype=[("p", ">f4", 3), ("uv", ">i2", 2), ("n", "i1", 3), ("pad", "u1")])
        else:  # (packs from before the model normals)
            mv0 = np.frombuffer(ch["MVTX"], dtype=[("p", ">f4", 3), ("uv", ">i2", 2)])
            mv = np.zeros(len(mv0), dtype=[("p", ">f4", 3), ("uv", ">i2", 2), ("n", "i1", 3), ("pad", "u1")])
            mv["p"], mv["uv"] = mv0["p"], mv0["uv"]
        spcb = np.frombuffer(ch["SPCB"], "u1").reshape(-1, 6, 3) if "SPCB" in ch else None
        lcub = np.frombuffer(ch["LCUB"][4:], "u1").reshape(-1, 6, 3) if "LCUB" in ch else None
        mi = np.frombuffer(ch["MIDX"], ">u2")

        def amat(pi, ya, ro):
            sp_, cp_ = math.sin(math.radians(pi)), math.cos(math.radians(pi))
            sy_, cy_ = math.sin(math.radians(ya)), math.cos(math.radians(ya))
            sr_, cr_ = math.sin(math.radians(ro)), math.cos(math.radians(ro))
            return np.array([[cp_ * cy_, sr_ * sp_ * cy_ - cr_ * sy_, cr_ * sp_ * cy_ + sr_ * sy_],
                             [cp_ * sy_, sr_ * sp_ * sy_ + cr_ * cy_, cr_ * sp_ * sy_ - sr_ * cy_],
                             [-sp_, sr_ * cp_, cr_ * cp_]])

        def add(mid, org, ang, tint, cube=None):
            fm, nm_, fv, nv, _fh, _nh = mdls[mid][:6]
            R = amat(*ang)
            vv_ = mv[fv:fv + nv]
            P = vv_["p"].astype(np.float64) @ R.T + np.array(org)
            UV = vv_["uv"].astype(np.float64) / 1024
            if cube is not None and cube.any():
                # the engine's lighting: Source's ambient cube, sum of (N.axis)^2 x face colour (x2 in the TEV)
                N = vv_["n"].astype(np.float64) / 64 @ R.T
                pos, neg = np.maximum(N, 0) ** 2, np.maximum(-N, 0) ** 2
                C = cube.astype(np.float64) / 255 * 2
                col = pos[:, 0:1] * C[0] + neg[:, 0:1] * C[1] + pos[:, 1:2] * C[2] + neg[:, 1:2] * C[3] + \
                    pos[:, 2:3] * C[4] + neg[:, 2:3] * C[5]
            else:
                col = np.tile(np.array(tint, np.float64) / 255 * 2, (len(P), 1))
            for k in range(fm, fm + nm_):
                fi, ni, mat_, _ = mesh[k]
                prop_tris.append((P, UV, mi[fi:fi + ni].astype(np.int64), mat_, col))

        nsp = struct.unpack_from(">I", ch["SPRP"])[0]
        for i in range(nsp):
            mid, solid, _f, ox, oy, oz, ax, ay, az, r, g, bb, _a, cl, _p = struct.unpack_from(">HBB3f3f4Bhh", ch["SPRP"], 4 + i * 36)
            if vis is None or (cl >= 0 and vis[cl]):
                add(mid, (ox, oy, oz), (ax, ay, az), (r, g, bb), spcb[i] if spcb is not None else None)
        ne = struct.unpack_from(">I", ch["ENTS"])[0]
        estr = ch["ESTR"]
        gs = lambda o: estr[o:estr.index(b"\0", o)].decode("latin-1")
        for i in range(ne):
            f0, n0 = struct.unpack_from(">II", ch["ENTS"], 4 + i * 8)
            kv = {}
            for k in range(f0, f0 + n0):
                ko, vo = struct.unpack_from(">II", ch["EKVS"], k * 8)
                kv.setdefault(gs(ko), gs(vo))
            if "_mdl" in kv:
                org = [float(x) for x in kv.get("origin", "0 0 0").split()]
                ang = [float(x) for x in kv.get("angles", "0 0 0").split()]
                add(int(kv["_mdl"]), org, ang, [int(x) for x in kv["_tint"].split()])
        print(f"props drawn: {len(prop_tris)} meshes")

    near = 4.0
    tris_drawn = 0
    passes = [[], []]
    for fi in faces:
        f = F[fi]
        kind = mats[f["mat"]][2]
        passes[1 if kind in (MAT_TRANSLUCENT, MAT_ADDITIVE, MAT_WATER) else 0].append(fi)

    def raster_mesh(P, UV, tri_idx, tex_i, tint):
        nonlocal tris_drawn
        base = tex(tex_i) if tex_i >= 0 else None
        x, y, z = project(P)
        kind = mats[tex_i_to_mat.get(tex_i, 0)][2] if False else 0
        for t in range(0, len(tri_idx) - 2, 3):
            i0, i1, i2 = tri_idx[t], tri_idx[t + 1], tri_idx[t + 2]
            zz = z[[i0, i1, i2]]
            if (zz < near).any():
                continue
            sx = W / 2 + x[[i0, i1, i2]] / zz * fx * W / 2
            sy = H / 2 - y[[i0, i1, i2]] / zz * fy * H / 2
            x0, x1 = int(max(0, math.floor(sx.min()))), int(min(W - 1, math.ceil(sx.max())))
            y0, y1 = int(max(0, math.floor(sy.min()))), int(min(H - 1, math.ceil(sy.max())))
            if x0 > x1 or y0 > y1:
                continue
            gy, gx = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
            den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
            if abs(den) < 1e-9:
                continue
            w0 = ((sy[1] - sy[2]) * (gx - sx[2]) + (sx[2] - sx[1]) * (gy - sy[2])) / den
            w1 = ((sy[2] - sy[0]) * (gx - sx[2]) + (sx[0] - sx[2]) * (gy - sy[2])) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            pz = 1 / (w0 / zz[0] + w1 / zz[1] + w2 / zz[2])
            dsub = depth[y0:y1 + 1, x0:x1 + 1]
            inside &= pz < dsub
            if not inside.any():
                continue
            uvp = ((w0 / zz[0])[..., None] * UV[i0] + (w1 / zz[1])[..., None] * UV[i1]
                   + (w2 / zz[2])[..., None] * UV[i2]) * pz[..., None]
            if base is not None:
                th, tw = base.shape[:2]
                c = base[(np.floor(uvp[..., 1] * th).astype(int)) % th, (np.floor(uvp[..., 0] * tw).astype(int)) % tw]
                inside &= c[..., 3] > 0.5
                tt = tint if np.ndim(tint) == 1 else tint[[i0, i1, i2]].mean(0)
                rgb = np.clip(c[..., :3] * tt, 0, 1)
            else:
                rgb = np.ones(inside.shape + (3,)) * 0.5
            csub = color[y0:y1 + 1, x0:x1 + 1]
            csub[inside] = rgb[inside]
            dsub[inside] = pz[inside]
            tris_drawn += 1

    tex_i_to_mat = {}
    for pas, flist in enumerate(passes):
        if pas == 1:
            for P, UV, tri_idx, mat_i, tint in prop_tris:
                raster_mesh(P, UV, tri_idx, mats[mat_i][0], tint)
        for fi in flist:
            f = F[fi]
            vv = V[f["first"]: f["first"] + f["n"]]
            if f["prim"] == 0:
                tri = [(0, i, i + 1) for i in range(1, f["n"] - 1)]
            else:
                tri = [(i, i + 1, i + 2) for i in range(0, f["n"], 3)]
            tex_i, tex2_i, kind, r, g, b = mats[f["mat"]]
            base = tex(tex_i) if tex_i >= 0 else None
            base2 = tex(tex2_i) if tex2_i >= 0 else None
            lm = tex(f["lm"]) if f["lm"] != 0xFFFF else None
            P = vv["p"].astype(np.float64)
            x, y, z = project(P)
            for (i0, i1, i2) in tri:
                zz = z[[i0, i1, i2]]
                if (zz < near).any():
                    continue  # (preview only: skip near-clipped triangles)
                sx = W / 2 + x[[i0, i1, i2]] / zz * fx * W / 2
                sy = H / 2 - y[[i0, i1, i2]] / zz * fy * H / 2
                x0, x1 = int(max(0, math.floor(sx.min()))), int(min(W - 1, math.ceil(sx.max())))
                y0, y1 = int(max(0, math.floor(sy.min()))), int(min(H - 1, math.ceil(sy.max())))
                if x0 > x1 or y0 > y1:
                    continue
                gy, gx = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
                den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
                if abs(den) < 1e-9:
                    continue
                w0 = ((sy[1] - sy[2]) * (gx - sx[2]) + (sx[2] - sx[1]) * (gy - sy[2])) / den
                w1 = ((sy[2] - sy[0]) * (gx - sx[2]) + (sx[0] - sx[2]) * (gy - sy[2])) / den
                w2 = 1 - w0 - w1
                inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
                if not inside.any():
                    continue
                iz = w0 / zz[0] + w1 / zz[1] + w2 / zz[2]
                pz = 1 / iz
                dsub = depth[y0:y1 + 1, x0:x1 + 1]
                inside &= pz < dsub
                if not inside.any():
                    continue

                def attr(arr):
                    a0, a1, a2 = arr[i0], arr[i1], arr[i2]
                    return ((w0 / zz[0])[..., None] * a0 + (w1 / zz[1])[..., None] * a1
                            + (w2 / zz[2])[..., None] * a2) * pz[..., None]

                if base is not None:
                    uv = attr(vv["uv"].astype(np.float64))
                    th, tw = base.shape[:2]
                    tu = (np.floor(uv[..., 0] * tw).astype(int)) % tw
                    tv = (np.floor(uv[..., 1] * th).astype(int)) % th
                    c = base[tv, tu]
                    if kind == MAT_BLEND and base2 is not None:
                        al = attr(vv["c"][:, 3:4].astype(np.float64) / 255)
                        th2, tw2 = base2.shape[:2]
                        c2 = base2[(np.floor(uv[..., 1] * th2).astype(int)) % th2,
                                   (np.floor(uv[..., 0] * tw2).astype(int)) % tw2]
                        c = c * (1 - al) + c2 * al
                else:
                    c = np.zeros(inside.shape + (4,), np.float32)
                    c[...] = (r / 255, g / 255, b / 255, 170 / 255)
                if lm is not None and base is not None:
                    lmuv = attr(vv["lm"].astype(np.float64))
                    lh, lw = lm.shape[:2]
                    lu = np.clip((lmuv[..., 0] * lw).astype(int), 0, lw - 1)
                    lv = np.clip((lmuv[..., 1] * lh).astype(int), 0, lh - 1)
                    rgb = np.clip(c[..., :3] * lm[lv, lu, :3] * 2, 0, 1)
                elif base is not None:
                    rgb = c[..., :3] * (128 / 255) * 2  # neutral lightmap
                else:
                    rgb = c[..., :3]
                alpha = c[..., 3]
                csub = color[y0:y1 + 1, x0:x1 + 1]
                if kind == MAT_ALPHATEST:
                    inside &= alpha > 0.5
                if a.probe and y0 <= a.probe[1] <= y1 and x0 <= a.probe[0] <= x1 and \
                        inside[a.probe[1] - y0, a.probe[0] - x0]:
                    py_, px_ = a.probe[1] - y0, a.probe[0] - x0
                    print(f"probe: face {fi} mat {f['mat']} kind {kind} tex {tex_i} lm {f['lm']} "
                          f"base {c[py_, px_].round(3).tolist()} out {rgb[py_, px_].round(3).tolist()} z {pz[py_, px_]:.1f}")
                if pas == 0:
                    csub[inside] = rgb[inside]
                    dsub[inside] = pz[inside]
                elif kind == MAT_ADDITIVE:
                    csub[inside] = np.clip(csub[inside] + rgb[inside] * alpha[inside, None], 0, 1)
                else:
                    csub[inside] = csub[inside] * (1 - alpha[inside, None]) + rgb[inside] * alpha[inside, None]
                tris_drawn += 1
    print(f"drew {tris_drawn} triangles; eye {eye.round(1).tolist()} pitch {pitch} yaw {yaw}")
    Image.fromarray((np.clip(color, 0, 1) * 255).astype(np.uint8)).save(a.out)


if __name__ == "__main__":
    main()
