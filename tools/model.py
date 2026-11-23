"""Source model reader: MDL (v44-48) + VVD + VTX meshes, and PHY collision hulls."""
import re
import struct

import numpy as np

VERTEX_SIZE = 48  # mstudiovertex_t


def _cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("latin-1")


class Mesh:
    def __init__(self, material, positions, normals, uvs, indices, bones=None):
        self.bones = bones        # per-vertex dominant bone index (rigid skinning), or None
        self.material = material  # texture index into Model.textures (after skin lookup)
        self.positions = positions
        self.normals = normals
        self.uvs = uvs
        self.indices = indices    # flat triangle list into positions


class Model:
    def __init__(self):
        self.name = ""
        self.textures = []   # material names (without extension), resolved by search path later
        self.cdpaths = []
        self.meshes = []
        self.hulls = []      # list of convex pieces, each an (N,3) array of points (model space, inches)
        self.mins = np.zeros(3)
        self.maxs = np.zeros(3)
        self.mass = 0.0      # kg, from the .phy text section (sum over solids)
        self.eyeballs = []   # (bone, origin, up, forward, z offset, iris scale), bind-pose model space


def _pose_mats(fs, path):
    """Skinning matrices [n,3,4] of the model's first sequence, first frame, with its bone names; None if
    that's the bind pose (or there's no sequence)."""
    try:
        from anim import AnimatedModel, pose_matrices
        am = AnimatedModel(fs, path)
        if not am.seqs:
            return None
        bk = am.bake(next(iter(am.seqs)), max_frames=1)
        M = pose_matrices(am.skel, bk["pos"][0], bk["quat"][0])
    except Exception:  # noqa: BLE001
        return None
    ident = np.hstack([np.eye(3), np.zeros((3, 1))])
    if np.allclose(M, ident[None], atol=1e-3):
        return None
    return [b.name.lower() for b in am.skel], M


def load_model(fs, path, lod=0, pose=False):
    """Returns Model or None. path like 'models/foo/bar.mdl'.
    pose: for models drawn rigid (props): put a many-boned model in its first sequence's pose, which is how
    Source shows it. The doors are authored with their idle a quarter turn from the bind pose: drawn in the
    bind pose, a shut door stuck out of its frame like an open one and an open one lay in it."""
    mdl = fs.read(path)
    if mdl is None or mdl[:4] != b"IDST":
        return None
    base = path[:-4]
    vvd = fs.read(base + ".vvd")
    vtx = fs.read(base + ".dx90.vtx") or fs.read(base + ".vtx") or fs.read(base + ".dx80.vtx")
    if vvd is None or vtx is None:
        return None
    m = Model()
    m.name = path
    i32 = lambda o: struct.unpack_from("<i", mdl, o)[0]

    numtex, texidx = i32(204), i32(208)
    for t in range(numtex):
        o = texidx + t * 64
        m.textures.append(_cstr(mdl, o + i32(o)).replace("\\", "/").lower())
    numcd, cdidx = i32(212), i32(216)
    for c in range(numcd):
        m.cdpaths.append(_cstr(mdl, i32(cdidx + c * 4)).replace("\\", "/").lower())
    numskinref, skinidx = i32(220), i32(228)
    skin0 = [struct.unpack_from("<h", mdl, skinidx + r * 2)[0] for r in range(numskinref)]

    # ---- VVD vertices for the requested LOD (fixups rebuild the per-LOD vertex order)
    nlods = struct.unpack_from("<i", vvd, 12)[0]
    lod = min(lod, max(0, nlods - 1))
    nfix, fixstart, vstart = struct.unpack_from("<iii", vvd, 48)
    raw = np.frombuffer(vvd, dtype=[("w", "<f4", 3), ("b", "u1", 3), ("nb", "u1"), ("pos", "<f4", 3),
                                    ("n", "<f4", 3), ("uv", "<f4", 2)],
                        count=struct.unpack_from("<i", vvd, 16)[0], offset=vstart)
    to_lod0 = None  # this LOD's vertex index -> LOD0 vertex index (what flex morphs refer to)
    if nfix:
        order, order0 = [], []
        for f in range(nfix):
            flod, src, cnt = struct.unpack_from("<iii", vvd, fixstart + f * 12)
            if flod >= lod:
                order.extend(range(src, src + cnt))
            order0.extend(range(src, src + cnt))
        verts = raw[np.array(order, dtype=np.int64)]
        inv0 = np.zeros(len(raw), dtype=np.int64)
        inv0[np.array(order0, dtype=np.int64)] = np.arange(len(order0))
        to_lod0 = inv0[np.array(order, dtype=np.int64)]
    else:
        verts = raw

    # ---- where each mesh's vertices start in this LOD's vertex list: meshes are stored in order,
    # and each records how many vertices it has per LOD
    bodypart_count, bodypart_idx = i32(232), i32(236)
    mesh_start = {}
    cursor = 0
    for bp in range(bodypart_count):
        mbp = bodypart_idx + bp * 16
        nmodels, modelidx = i32(mbp + 4), i32(mbp + 12)
        for mi in range(nmodels):
            mmod = mbp + modelidx + mi * 148
            nmesh, meshidx = i32(mmod + 72), i32(mmod + 76)
            for me in range(nmesh):
                mmesh = mmod + meshidx + me * 116
                mesh_start[(bp, mi, me)] = cursor
                cursor += i32(mmesh + 52 + lod * 4)

    # ---- VTX strips -> triangle indices
    (ver, _cache, _mbs, _mbt, _mbv, _chk, vnlods, _mrl, nbp, bpoff) = struct.unpack_from("<iiHHiiiiii", vtx, 0)
    for bp in range(min(nbp, bodypart_count)):
        vbp = bpoff + bp * 8
        vnm, vmoff = struct.unpack_from("<ii", vtx, vbp)
        mbp = bodypart_idx + bp * 16
        nmodels, modelidx = i32(mbp + 4), i32(mbp + 12)
        for mi in range(min(vnm, nmodels)):
            if mi > 0:
                break  # first sub-model of each body part (default body group)
            mmod = mbp + modelidx + mi * 148
            nmesh, meshidx = i32(mmod + 72), i32(mmod + 76)
            vmod = vbp + vmoff + mi * 8
            mlods, lodoff = struct.unpack_from("<ii", vtx, vmod)
            use_lod = min(lod, mlods - 1)
            vlod = vmod + lodoff + use_lod * 12
            vnmesh, vmeshoff = struct.unpack_from("<ii", vtx, vlod)
            for me in range(min(nmesh, vnmesh)):
                mmesh = mmod + meshidx + me * 116
                mat = i32(mmesh)
                vstart_mesh = mesh_start[(bp, mi, me)]
                vmesh = vlod + vmeshoff + me * 9
                nsg, sgoff = struct.unpack_from("<ii", vtx, vmesh)
                pos_l, nrm_l, uv_l, idx_l, bone_l, v0_l = [], [], [], [], [], []
                for g in range(nsg):
                    sg = vmesh + sgoff + g * 25
                    nv, voff, ni, ioff, ns, soff = struct.unpack_from("<iiiiii", vtx, sg)
                    sgv = np.frombuffer(vtx, dtype=[("bwi", "u1", 3), ("nb", "u1"), ("orig", "<u2"), ("bid", "i1", 3)],
                                        count=nv, offset=sg + voff)
                    sgi = np.frombuffer(vtx, dtype="<u2", count=ni, offset=sg + ioff)
                    gidx = vstart_mesh + sgv["orig"].astype(np.int64)
                    gidx = np.clip(gidx, 0, len(verts) - 1)
                    base_i = sum(len(p) for p in pos_l)
                    pos_l.append(verts["pos"][gidx])
                    nrm_l.append(verts["n"][gidx])
                    uv_l.append(verts["uv"][gidx])
                    vw = verts["w"][gidx]
                    vb = verts["b"][gidx]
                    bone_l.append(vb[np.arange(len(gidx)), np.argmax(vw, axis=1)])
                    v0_l.append(to_lod0[gidx] if to_lod0 is not None else gidx)
                    for s in range(ns):
                        so = sg + soff + s * 27
                        sni, sio, _snv, _svo, _nb, sflags = struct.unpack_from("<iiiihB", vtx, so)
                        ind = sgi[sio:sio + sni].astype(np.int64)
                        if sflags & 2:  # triangle strip
                            tris = []
                            for k in range(len(ind) - 2):
                                a, b, c = ind[k], ind[k + 1], ind[k + 2]
                                if a == b or b == c or a == c:
                                    continue
                                tris += [a, c, b] if k & 1 else [a, b, c]
                            ind = np.array(tris, dtype=np.int64)
                        idx_l.append(ind + base_i)
                if not idx_l:
                    continue
                tex = skin0[mat] if 0 <= mat < len(skin0) else mat
                mesh = Mesh(tex, np.concatenate(pos_l), np.concatenate(nrm_l), np.concatenate(uv_l),
                            np.concatenate(idx_l), np.concatenate(bone_l))
                mesh.vvd0 = np.concatenate(v0_l)
                mesh.flexes = _mesh_flexes(mdl, mmod, mmesh)
                mesh.eyeball = -1
                if i32(mmesh + 24) == 1:  # materialtype 1: an eyeball, materialparam = which one
                    eyes = _eyeballs(mdl, mmod)
                    k = i32(mmesh + 28)
                    if 0 <= k < len(eyes):
                        if eyes[k] not in m.eyeballs:
                            m.eyeballs.append(eyes[k])
                        mesh.eyeball = m.eyeballs.index(eyes[k])
                m.meshes.append(mesh)
    m.flexctl, m.ndesc, m.rules = _flex_rules(mdl)
    m.attachments = _attachments(mdl)
    posed = None
    if pose and m.meshes and struct.unpack_from("<i", mdl, 156)[0] > 1 and all(me.bones is not None for me in m.meshes):
        posed = _pose_mats(fs, path)
    if posed:
        for me in m.meshes:
            Mb = posed[1][np.clip(np.asarray(me.bones, dtype=np.int64), 0, len(posed[1]) - 1)]
            me.positions = (np.einsum("nij,nj->ni", Mb[:, :, :3], me.positions) + Mb[:, :, 3]).astype(me.positions.dtype)
            nn = np.einsum("nij,nj->ni", Mb[:, :, :3], me.normals)
            me.normals = (nn / np.maximum(np.linalg.norm(nn, axis=1, keepdims=True), 1e-9)).astype(me.normals.dtype)
    if m.meshes:
        allp = np.concatenate([me.positions for me in m.meshes])
        m.mins, m.maxs = allp.min(0), allp.max(0)
    phy = fs.read(base + ".phy")
    if phy:
        m.mass = phy_mass(phy)
        try:
            hulls, bone_names = parse_phy(phy)
            bones = _bones(mdl)
            out = []
            for h, bname in zip(hulls, bone_names):
                # collision pieces are stored in their bone's space: bring them into model space
                R, t = bones.get(bname, bones.get(0, (np.eye(3), np.zeros(3))))
                hp = (h - t) @ R  # inverse of the bone's poseToBone (R p + t)
                if posed:
                    bi = posed[0].index(bname.lower()) if isinstance(bname, str) and bname.lower() in posed[0] else 0
                    hp = hp @ posed[1][bi][:, :3].T + posed[1][bi][:, 3]
                out.append(hp)
            m.hulls = _align_hulls(out, hulls, m.mins, m.maxs) if m.meshes else out
        except Exception:  # noqa: BLE001
            m.hulls = []
    _default_pose(fs, path, mdl, m)
    return m


def _default_pose(fs, path, mdl, m):
    """A one-bone model is drawn in its first sequence's pose, not its bind pose: Source always animates
    it, and some (Barney's helmet and faceplate) are authored with the idle turning the bone 90 degrees."""
    if struct.unpack_from("<i", mdl, 156)[0] != 1 or not m.meshes:
        return
    try:
        from anim import AnimatedModel, pose_matrices
        am = AnimatedModel(fs, path)
        if not am.seqs or len(am.skel) != 1:
            return
        bk = am.bake(next(iter(am.seqs)), max_frames=1)
        M = pose_matrices(am.skel, bk["pos"][0], bk["quat"][0])[0]
    except Exception:  # noqa: BLE001
        return
    if np.allclose(M, np.hstack([np.eye(3), np.zeros((3, 1))]), atol=1e-4):
        return
    R, t = M[:, :3], M[:, 3]
    for me in m.meshes:
        me.positions = (me.positions @ R.T + t).astype(me.positions.dtype)
        me.normals = (me.normals @ R.T).astype(me.normals.dtype)
    m.hulls = [h @ R.T + t for h in m.hulls]
    allp = np.concatenate([me.positions for me in m.meshes])
    m.mins, m.maxs = allp.min(0), allp.max(0)


def _f16(a):
    return np.frombuffer(np.asarray(a, dtype="<u2").tobytes(), dtype="<f2").astype(np.float32)


def _mesh_flexes(mdl, mmod, mmesh):
    """Facial morph targets of one mesh: [(flexdesc, flexpair, (t0,t1,t2,t3), lod0_idx[], delta[n,3], side[])]."""
    i32 = lambda o: struct.unpack_from("<i", mdl, o)[0]
    nflex, flexidx = i32(mmesh + 16), i32(mmesh + 20)
    if nflex <= 0:
        return []
    vbase = i32(mmod + 84) // 48 + i32(mmesh + 12)
    out = []
    for f in range(nflex):
        o = mmesh + flexidx + f * 60
        desc = i32(o)
        tg = struct.unpack_from("<4f", mdl, o + 4)
        nv, vi = i32(o + 20), i32(o + 24)
        pair = i32(o + 28)
        vtype = mdl[o + 32]
        size = 18 if vtype == 1 else 16
        if nv <= 0:
            continue
        rec = np.frombuffer(mdl, dtype=[("i", "<u2"), ("speed", "u1"), ("side", "u1"), ("d", "<u2", 3),
                                        ("nd", "<u2", 3)] + ([("w", "<i2")] if size == 18 else []),
                            count=nv, offset=o + vi)
        out.append((desc, pair, tg, vbase + rec["i"].astype(np.int64), _f16(rec["d"].reshape(-1)).reshape(-1, 3),
                    rec["side"].copy()))
    return out


def _flex_rules(mdl):
    """(controllers [(name, min, max)], number of flex descs, rules [(flexdesc, [(op, value)])])."""
    i32 = lambda o: struct.unpack_from("<i", mdl, o)[0]
    ndesc = i32(260)
    nctl, ctlidx = i32(268), i32(272)
    ctls = []
    for c in range(nctl):
        o = ctlidx + c * 20
        name = _cstr(mdl, o + i32(o + 4)).lower()
        lo, hi = struct.unpack_from("<2f", mdl, o + 12)
        ctls.append((name, lo, hi))
    nrule, ruleidx = i32(276), i32(280)
    rules = []
    for r in range(nrule):
        o = ruleidx + r * 12
        flex, nops, opidx = i32(o), i32(o + 4), i32(o + 8)
        ops = []
        for k in range(nops):
            po = o + opidx + k * 8
            op = i32(po)
            val = struct.unpack_from("<f", mdl, po + 4)[0] if op == 1 else float(i32(po + 4))
            ops.append((op, val))
        rules.append((flex, ops))
    return ctls, ndesc, rules


def _eyeballs(mdl, mmod):
    """A body part model's eyeballs, in bind-pose model space (where the mesh vertices are):
    (bone, origin, up, forward (into the head), z offset, iris scale)."""
    i32 = lambda o: struct.unpack_from("<i", mdl, o)[0]
    n, idx = i32(mmod + 100), i32(mmod + 104)
    bones = _bones(mdl)
    out = []
    for e in range(n):
        o = mmod + idx + e * 172
        bone = i32(o + 4)
        org = np.array(struct.unpack_from("<3f", mdl, o + 8))
        zoff = struct.unpack_from("<f", mdl, o + 20)[0]
        up = np.array(struct.unpack_from("<3f", mdl, o + 28))
        fwd = np.array(struct.unpack_from("<3f", mdl, o + 40))
        iris = struct.unpack_from("<f", mdl, o + 60)[0]
        R, t = bones.get(bone, (np.eye(3), np.zeros(3)))
        out.append((bone, tuple(np.round(R.T @ (org - t), 4)), tuple(np.round(R.T @ up, 5)),
                    tuple(np.round(R.T @ fwd, 5)), float(zoff), float(iris)))
    return out


def _attachments(mdl):
    """[(name, bone, local 3x4)] - points like a character's eyes or mouth that cameras aim at."""
    i32 = lambda o: struct.unpack_from("<i", mdl, o)[0]
    n, idx = i32(240), i32(244)
    out = []
    for a in range(n):
        o = idx + a * 92
        out.append((_cstr(mdl, o + i32(o)).lower(), i32(o + 8),
                    np.array(struct.unpack_from("<12f", mdl, o + 12), dtype=np.float64).reshape(3, 4)))
    # "_rhand": the right hand bone itself. Source bone-merges a held weapon's bones onto the character's
    # same-named ones; the engine puts the weapon's hand bone on the character's with this
    try:
        from anim import read_bones
        # "_rattach": the hand's attachment bone (anim_attachment_rh), which held things like the
        # passengers' suitcases are skinned to; it bone-merges onto the character's too
        for bi, bn in enumerate(read_bones(mdl)):
            if bn.name == "valvebiped.bip01_r_hand":
                out.append(("_rhand", bi, np.hstack([np.eye(3), np.zeros((3, 1))])))
            elif bn.name == "valvebiped.anim_attachment_rh":
                out.append(("_rattach", bi, np.hstack([np.eye(3), np.zeros((3, 1))])))
    except Exception:  # noqa: BLE001
        pass
    return out


def _bbox_err(hulls, mins, maxs):
    p = np.concatenate(hulls)
    return float(np.abs(p.min(0) - mins).sum() + np.abs(p.max(0) - maxs).sum())


def _rotations():
    """The 24 proper axis-aligned rotations."""
    import itertools
    out = []
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product((1, -1), repeat=3):
            R = np.zeros((3, 3))
            for i in range(3):
                R[i, perm[i]] = signs[i]
            if np.linalg.det(R) > 0:
                out.append(R)
    return out


_ROTS = _rotations()


def _align_hulls(boned, raw, mins, maxs):
    """A few models (doors, fences, vehicles) store collision in a frame our bone handling doesn't
    reproduce. Keep the bone-space result unless an axis-aligned rotation of the raw pieces matches
    the visible mesh far better (the visual mesh is the reference the player sees)."""
    e0 = _bbox_err(boned, mins, maxs)
    if e0 <= 24:
        return boned
    best, best_e = None, e0
    for R in _ROTS:
        cand = [h @ R.T for h in raw]
        e = _bbox_err(cand, mins, maxs)
        if e < best_e:
            best, best_e = cand, e
    return best if best is not None and best_e < 0.5 * e0 else boned


def _bones(mdl):
    """bone name / index -> (R, t) of its poseToBone matrix (model space -> bone space)."""
    numbones, boneidx = struct.unpack_from("<ii", mdl, 156)
    out = {}
    for b in range(numbones):
        o = boneidx + b * 216
        name = _cstr(mdl, o + struct.unpack_from("<i", mdl, o)[0]).lower()
        m34 = np.array(struct.unpack_from("<12f", mdl, o + 96)).reshape(3, 4)
        out[name] = out[b] = (m34[:, :3], m34[:, 3])
    return out


METERS_TO_INCHES = 1.0 / 0.0254


def phy_mass(data):
    """Total mass from the keyvalue text at the end of a .phy ("mass" "35.0" per solid)."""
    try:
        text = data.decode("latin-1")
    except Exception:  # noqa: BLE001
        return 0.0
    vals = re.findall(r'"mass"\s+"([0-9.]+)"', text)
    return float(sum(float(v) for v in vals)) if vals else 0.0


def phy_breaks(data):
    """The pieces a model breaks into: ("model path", fade seconds) from the .phy's "break" blocks."""
    try:
        text = data.decode("latin-1")
    except Exception:  # noqa: BLE001
        return []
    out = []
    for blk in re.findall(r'break\s*\{([^}]*)\}', text):
        m = re.search(r'"model"\s+"([^"]+)"', blk)
        if not m:
            continue
        path = m.group(1).lower().replace("\\", "/")
        if not path.startswith("models/"):
            path = "models/" + path
        if not path.endswith(".mdl"):
            path += ".mdl"
        f = re.search(r'"fadetime"\s+"([0-9.]+)"', blk)
        out.append((path, float(f.group(1)) if f else 20.0))
    return out


def parse_phy(data):
    """Returns (convex pieces as point arrays in bone space (inches, Source axes), bone name per piece)."""
    hdr_size, _id, nsolids = struct.unpack_from("<iii", data, 0)
    pos = hdr_size
    hulls = []
    owners = []
    solid_start = len(hulls)
    for solid in range(nsolids):
        solid_start = len(hulls)
        size = struct.unpack_from("<i", data, pos)[0]
        start = pos + 4
        surf = start
        if data[start:start + 4] == b"VPHY":
            surf = start + 28  # skip the VPHY header
        # IVP compact surface (48 bytes), then convex pieces ("ledges": 16-byte header + 16 bytes per
        # triangle) back to back, then one point pool they all index into, then the ledge tree.
        ledgetree = struct.unpack_from("<i", data, surf + 32)[0]
        # Only the ledges at the leaves of the ledge tree are the solid's convex pieces: inner tree nodes
        # point at convex hulls of everything under them (IVP's culling hulls), which would fill a door
        # frame's or an arch's opening with an invisible wall.
        leaf_ledges = []
        stack = [surf + ledgetree]
        guard = 0
        while stack and guard < 100000:
            guard += 1
            node = stack.pop()
            right, ledge_off = struct.unpack_from("<ii", data, node)
            if right == 0:
                if ledge_off:
                    leaf_ledges.append(node + ledge_off)
            else:
                stack.append(node + 28)       # left child follows the node (28-byte IVP_Compact_Ledgetree_Node)
                stack.append(node + right)    # right child
        for p in sorted(set(leaf_ledges)):
            point_off, _client, _flags, ntris = struct.unpack_from("<iiIh", data, p)
            if ntris <= 0 or point_off == 0:
                continue
            idxs = set()
            for t in range(ntris):
                tri = p + 16 + t * 16
                for e in range(3):
                    edge = struct.unpack_from("<I", data, tri + 4 + e * 4)[0]
                    idxs.add(edge & 0xFFFF)
            if idxs:
                pts = np.array([struct.unpack_from("<3f", data, p + point_off + i * 16) for i in sorted(idxs)])
                # IVP axes -> Source axes: (x, y, z) -> (x, z, -y), metres -> inches
                hulls.append(np.stack([pts[:, 0], pts[:, 2], -pts[:, 1]], 1) * METERS_TO_INCHES)
        owners += [solid] * (len(hulls) - solid_start)
        pos = start + size
    # the text block after the solids names each solid's bone
    names = {}
    text = data[pos:].split(b"\0")[0].decode("latin-1", "replace")
    for block in text.split("solid {")[1:]:
        kv = dict(re.findall(r'"([^"]+)"\s+"([^"]*)"', block.split("}")[0]))
        if "index" in kv:
            names[int(kv["index"])] = kv.get("name", "").lower()
    return hulls, [names.get(o, 0) for o in owners]


def convex_planes(points, eps=0.05):
    """Planes (normal, dist) bounding a convex point set: a gift-wrap over all point triples is too slow,
    so use the hull's own facets from a simple incremental hull."""
    from scipy.spatial import ConvexHull  # noqa: WPS433
    hull = ConvexHull(points)
    planes = []
    for eq in hull.equations:  # n.x + d <= 0 inside
        n, d = eq[:3], -eq[3]
        if any(np.allclose(n, q[0], atol=1e-3) and abs(d - q[1]) < eps for q in planes):
            continue
        planes.append((n, d))
    return planes


def pack_normals(n):
    """unit normals -> s8 x 64 (GX_NRM_XYZ, GX_S8, 6 fraction bits)"""
    n = np.asarray(n, dtype=np.float32)
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    n = np.where(ln > 1e-6, n / np.maximum(ln, 1e-6), np.array([0, 0, 1], np.float32))
    return np.clip(np.round(n * 64), -64, 63).astype(np.int8)
