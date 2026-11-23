"""Props (static + entity models) and entities with their I/O for hl2gc map packs."""
import math
import struct
from strips import stripify

import numpy as np

import bsp
from material import load_material
from model import phy_breaks, convex_planes, load_model, pack_normals

MODEL_VERT_BUDGET = 90000  # all models in a map, after LOD selection (16 bytes each)
TINT_FLOOR = 0.15
CUBE_FLOOR = 0.1
FAR_LOD_MIN_VERTS = 250   # models this big get a far LOD...
FAR_LOD_BUDGET = 40000    # ...within this many extra vertices per map (memory: textures shrink to make room)
LIGHT_CUTOFF = 0.004   # linear light below which a worldlight is ignored (sets each light's reach)
LIGHT_MAX_REACH = 2048.0


def worldlight_chunk(b):
    """WLIT: u32 count, then per light >3f origin, 3f intensity (linear, lightmap scale), 3f normal,
    i type (1 point, 2 spot, 0 surface, 3 sun), i cluster, i style (lightstyles.py), f stopdot, stopdot2, exponent, reach, c, l, q.
    Sky ambient is already in the leaf cubes; quake lights don't occur in HL2."""
    out = []
    for w in b.worldlights():
        t = int(w["type"])
        if t not in (0, 1, 2, 3):
            continue
        imax = float(np.max(w["intensity"]))
        if not np.isfinite(imax) or imax <= 0:
            continue
        if t == 3:
            reach = 0.0  # (no falloff)
        elif t == 0:
            reach = math.sqrt(imax / LIGHT_CUTOFF)
        else:
            c, l, q = float(w["c"]), float(w["l"]), float(w["q"])
            # imax / (c + l d + q d^2) = cutoff
            k = imax / LIGHT_CUTOFF - c
            if k <= 0:
                continue
            if q > 0:
                reach = (-l + math.sqrt(l * l + 4 * q * k)) / (2 * q)
            elif l > 0:
                reach = k / l
            else:
                reach = LIGHT_MAX_REACH
            if float(w["radius"]) > 0:
                reach = min(reach, float(w["radius"]))
        if t != 3:
            reach = min(reach, LIGHT_MAX_REACH)
            if reach < 8:
                continue
        out.append(struct.pack(">3f3f3fiii7f", *map(float, w["origin"]), *map(float, w["intensity"]),
                               *map(float, w["normal"]), t, int(w["cluster"]), int(w["style"]), float(w["stopdot"]),
                               float(w["stopdot2"]), float(w["exponent"]), reach, float(w["c"]),
                               float(w["l"]), float(w["q"])))
    return struct.pack(">I", len(out)) + b"".join(out)
# model vertex: position, texture coordinate (1/1024), normal (s8 x 64)
# model vertex: position s16 in 1/2^frac units (frac per model, in each mesh record), uv s16 1/1024,
# normal s8 x64, pad: 14 bytes (was 16 with f32 positions and no normal)
MVTX_DT = np.dtype([("p", ">i2", 3), ("uv", ">i2", 2), ("n", "i1", 3), ("pad", "u1")])


def pos_frac(positions):
    """fraction bits for a model's s16 positions: as fine as its size allows (8 = 1/256 unit)"""
    m = float(np.abs(positions).max()) if len(positions) else 0.0
    f = 8
    while f > 0 and m * (1 << f) > 32000:
        f -= 1
    return f

PROP_CLASSES = ("prop_dynamic", "prop_dynamic_override", "prop_physics", "prop_physics_override",
                "prop_physics_multiplayer", "prop_door_rotating", "prop_detail", "prop_physics_respawnable",
                "item_item_crate", "prop_dynamic_ornament", "item_healthcharger", "item_suitcharger",
                "item_ammo_crate", "prop_vehicle_airboat", "prop_vehicle_jeep", "prop_vehicle_prisoner_pod",
                "prop_vehicle_apc", "prop_vehicle_crane")

CLASS_MODELS = {"item_item_crate": "models/items/item_item_crate.mdl",
                "item_healthcharger": "models/props_combine/health_charger001.mdl",
                "item_suitcharger": "models/props_combine/suit_charger001.mdl",
                "item_ammo_crate": "models/items/ammocrate_smg1.mdl"}
AMMO_CRATE_MODELS = ["models/items/ammocrate_smg1.mdl", "models/items/ammocrate_smg1.mdl",
                     "models/items/ammocrate_ar2.mdl", "models/items/ammocrate_rockets.mdl",
                     "models/items/ammocrate_smg1.mdl", "models/items/ammocrate_grenade.mdl",
                     "models/items/ammocrate_smg1.mdl", "models/items/ammocrate_smg1.mdl",
                     "models/items/ammocrate_ar2.mdl", "models/items/ammocrate_smg1.mdl"]

GENERIC_GIBS = {
    "WoodChunks": ["models/gibs/wood_gib01e.mdl", "models/gibs/wood_gib01d.mdl", "models/gibs/wood_gib01c.mdl",
                   "models/gibs/wood_gib01b.mdl", "models/gibs/wood_gib01a.mdl"],
    "GlassChunks": ["models/gibs/glass_shard01.mdl", "models/gibs/glass_shard02.mdl", "models/gibs/glass_shard03.mdl",
                    "models/gibs/glass_shard04.mdl", "models/gibs/glass_shard05.mdl", "models/gibs/glass_shard06.mdl"],
    "MetalChunks": ["models/gibs/metal_gib1.mdl", "models/gibs/metal_gib2.mdl", "models/gibs/metal_gib3.mdl",
                    "models/gibs/metal_gib4.mdl", "models/gibs/metal_gib5.mdl"],
    "ConcreteChunks": ["models/props_debris/concrete_chunk08a.mdl", "models/props_debris/concrete_chunk09a.mdl",
                       "models/props_debris/concrete_chunk03a.mdl", "models/props_debris/concrete_chunk07a.mdl",
                       "models/props_debris/concrete_chunk02a.mdl"],
}
GENERIC_IDS = {"WoodChunks": -1, "GlassChunks": -2, "MetalChunks": -3, "ConcreteChunks": -4}

# entities the engine never needs (render/AI hints, editor-only, lighting): dropped to save memory
SKIP_CLASSES = ("info_node", "info_node_air", "info_node_air_hint", "info_node_climb",
                "infodecal", "info_overlay", "info_overlay_transition", "light", "light_spot", "light_environment",
                "env_cubemap", "info_lighting", "func_detail",
                "info_no_dynamic_shadow", "shadow_control",
                "func_areaportal", "func_areaportalwindow", "func_occluder", "func_viscluster",
                "env_tonemap_controller", "water_lod_control", "env_fog_controller",
                "info_hint", "func_lod", "env_sun", "func_dustmotes", "func_dustcloud",
                "env_smokestack", "env_steam", "beam_spotlight", "info_target_helicoptercrash")


def shooter_model(e):
    for k, v in e.items():
        if isinstance(v, str) and k.lower() == "shootmodel":
            return v.lower().replace("\\", "/")
    return ""


def angle_matrix(pitch, yaw, roll):
    """Source AngleMatrix: columns are forward, left, up."""
    sp, cp = math.sin(math.radians(pitch)), math.cos(math.radians(pitch))
    sy, cy = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    sr, cr = math.sin(math.radians(roll)), math.cos(math.radians(roll))
    return np.array([
        [cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy],
        [cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy],
        [-sp, sr * cp, cr * cp],
    ])


def parse_angles(s):
    try:
        v = [float(x) for x in s.split()[:3]]
        return v + [0.0] * (3 - len(v))
    except ValueError:
        return [0.0, 0.0, 0.0]


def parse_vec(s):
    return parse_angles(s)


class StaticProp:
    __slots__ = ("model", "origin", "angles", "solid", "skin", "light_origin", "fade_max", "leaves")


def static_props(b):
    """sprp game lump -> (model names, [StaticProp])."""
    gl = b.game_lumps()
    if "sprp" not in gl:
        return [], []
    ver, data = gl["sprp"]
    n = struct.unpack_from("<i", data, 0)[0]
    names = [data[4 + i * 128:4 + (i + 1) * 128].split(b"\0")[0].decode("latin-1").lower().replace("\\", "/")
             for i in range(n)]
    p = 4 + n * 128
    nl = struct.unpack_from("<i", data, p)[0]
    leaflist = struct.unpack_from(f"<{nl}H", data, p + 4) if nl > 0 else ()
    p += 4 + nl * 2
    ni = struct.unpack_from("<i", data, p)[0]
    p += 4
    stride = (len(data) - p) // max(ni, 1)
    out = []
    for i in range(ni):
        o = p + i * stride
        sp = StaticProp()
        sp.origin = list(struct.unpack_from("<3f", data, o))
        sp.angles = list(struct.unpack_from("<3f", data, o + 12))
        sp.model, firstleaf, leafcount, sp.solid, flags = struct.unpack_from("<HHHBB", data, o + 24)
        sp.leaves = leaflist[firstleaf:firstleaf + leafcount]  # the leaves vbsp found it touching
        sp.skin = struct.unpack_from("<i", data, o + 32)[0]
        sp.fade_max = struct.unpack_from("<f", data, o + 40)[0] if stride >= 44 else 0.0
        lo = list(struct.unpack_from("<3f", data, o + 44)) if stride >= 56 else sp.origin
        sp.light_origin = lo if flags & 0x2 else None  # (else: the model's centre, like Source)
        out.append(sp)
    return names, out


class Props:
    """Collects models used by the map, their materials, and the entities to export."""

    def __init__(self, b, fs, mats, log=print, far_lods=True):
        self.b, self.fs, self.log = b, fs, log
        # entities whose class supplies the model (Hammer leaves the key out)
        for e in b.entities:
            cls = e.get("classname", "")
            if cls in CLASS_MODELS and not e.get("model", "").lower().endswith(".mdl"):
                m = CLASS_MODELS[cls]
                if cls == "item_ammo_crate":
                    m = AMMO_CRATE_MODELS[min(max(int(float(e.get("AmmoType", e.get("ammotype", "0")) or 0)), 0), 9)]
                e["model"] = m
        static_names, self.static = static_props(b)
        ent_models = [e.get("model", "").lower().replace("\\", "/") for e in b.entities
                      if e.get("classname") in PROP_CLASSES and e.get("model", "").lower().endswith(".mdl")]
        # what env_shooters throw (gibs: "shootmodel")
        ent_models += [shooter_model(e) for e in b.entities
                       if e.get("classname") == "env_shooter" and shooter_model(e).endswith(".mdl")]
        wanted = []
        for nm in static_names + ent_models:
            if nm not in wanted:
                wanted.append(nm)
        # what breakable things break into: each model's own pieces (.phy "break"), and the generic
        # wood/glass/metal/concrete chunks func_breakables use (propdata.txt BreakableModels)
        self.breaks = {}
        for nm in list(wanted):
            raw = fs.read(nm[:-4] + ".phy")
            pieces = [(p_, f_) for p_, f_ in (phy_breaks(raw) if raw else []) if fs.exists(p_)][:8]
            if pieces:
                self.breaks[nm] = pieces
                for p_, _ in pieces:
                    if p_ not in wanted:
                        wanted.append(p_)
        mats_used = set()
        for e in b.entities:
            c = e.get("classname", "")
            if c == "func_breakable_surf":
                mats_used.add("GlassChunks")
            elif c == "func_breakable":
                mats_used.add({"0": "GlassChunks", "7": "GlassChunks", "1": "WoodChunks", "2": "MetalChunks",
                               "6": "MetalChunks", "4": "ConcreteChunks", "8": "ConcreteChunks"}.get(
                    e.get("material", "1").strip(), "WoodChunks"))
        self.generic = {}
        for name in GENERIC_GIBS:
            if name not in mats_used:
                continue
            lst = [p_ for p_ in GENERIC_GIBS[name] if fs.exists(p_)]
            self.generic[name] = lst
            for p_ in lst:
                if p_ not in wanted:
                    wanted.append(p_)

        # pick a LOD per model so the map's total model vertices fit the budget
        self.models = {}
        lods = {}
        for nm in wanted:
            m = load_model(fs, nm, 0, pose=True)
            if m and m.meshes:
                self.models[nm] = m
                lods[nm] = 0

        def nverts(m):
            return sum(len(me.positions) for me in m.meshes)

        total = sum(nverts(m) for m in self.models.values())
        tries = 0
        while total > MODEL_VERT_BUDGET and tries < 400:
            tries += 1
            nm = max(self.models, key=lambda k: nverts(self.models[k]) if lods[k] < 3 else -1)
            if lods[nm] >= 3:
                break
            m2 = load_model(fs, nm, lods[nm] + 1, pose=True)
            lods[nm] += 1
            if m2 and m2.meshes and nverts(m2) < nverts(self.models[nm]):
                total += nverts(m2) - nverts(self.models[nm])
                m2.hulls = self.models[nm].hulls
                self.models[nm] = m2
        self.model_index = {nm: i for i, nm in enumerate(self.models)}
        # far LODs: the renderer switches big models to a lighter version of themselves in the distance
        # (Source does this by screen size); appended after the map's models, render-only (no hulls)
        self.far_models = {}
        far_total = 0
        for nm in sorted(self.models, key=lambda k: -nverts(self.models[k])):
            n = nverts(self.models[nm])
            if not far_lods or n < FAR_LOD_MIN_VERTS or far_total >= FAR_LOD_BUDGET:
                continue
            best = None
            # the lightest of the next few LODs, if it saves enough to be worth the memory
            for lod in range(lods[nm] + 1, lods[nm] + 5):
                fm = load_model(fs, nm, lod, pose=True)
                if not (fm and fm.meshes):
                    break
                if nverts(fm) < 0.6 * n and (best is None or nverts(fm) < nverts(best)):
                    best = fm
                if best is not None and nverts(best) < 0.35 * n:
                    break
            if best is not None and far_total + nverts(best) <= FAR_LOD_BUDGET:
                best.hulls = []
                self.far_models[nm] = best
                far_total += nverts(best)
        if self.far_models:
            log(f"  far LODs: {len(self.far_models)} models, {far_total} vertices")
        self.static_names = static_names
        log(f"  models: {len(self.models)} ({total} vertices after LOD), static props: {len(self.static)}")

        # materials: model texture names resolved through the model's search paths
        self.mat_base = len(mats)
        self.model_mats = {}  # (model, texture index) -> material index
        mat_by_name = {}
        for nm, m in self.models.items():
            for ti, tex in enumerate(m.textures):
                found = None
                for cd in m.cdpaths or [""]:
                    cand = (cd + tex).replace("//", "/").strip("/")
                    if fs.exists(f"materials/{cand}.vmt"):
                        found = cand
                        break
                key = found or tex
                if key not in mat_by_name:
                    mat_by_name[key] = len(mats)
                    mats.append(load_material(fs, key))
                self.model_mats[(nm, ti)] = mat_by_name[key]

    # ---------------------------------------------------------------- lighting
    def tint(self, pos):
        b = self.b
        c = b.ambient_cube(b.leaf_at(np.array(pos, dtype=np.float64)))
        if c is None or c.max() <= 0:
            return (128, 128, 128)
        # the lightmaps' scale (half intensity: the TEV doubles it); leaf ambient is 255x below lightmaps
        return tuple(int(x) for x in bsp.light_u8(np.asarray(c) * 255.0, TINT_FLOOR))

    def cube(self, pos):
        """Source's ambient cube at pos (the leaf's), each face in the tint scale: 6 x (r, g, b), faces
        +X -X +Y -Y +Z -Z; None where the map has none"""
        b = self.b
        return b.ambient_faces_u8(b.leaf_at(np.array(pos, dtype=np.float64)), CUBE_FLOOR)

    # ---------------------------------------------------------------- chunks
    def chunks(self, chunk):
        mrec, mesh_rec, verts, idx, hull_rec, planes = [], [], [], [], [], []
        vcount = 0
        far_names = list(self.far_models)
        all_models = list(self.models.items()) + [(nm, self.far_models[nm]) for nm in far_names]
        for nm, m in all_models:
            first_mesh, first_vert, first_hull = len(mesh_rec), vcount, len(hull_rec)
            base = 0
            frac = pos_frac(np.concatenate([me.positions for me in m.meshes]) if m.meshes else np.zeros((0, 3)))
            if max((float(np.abs(me.positions).max()) for me in m.meshes if len(me.positions)), default=0) >= 32767:
                raise SystemExit(f"{nm}: bigger than 32767 units")
            for me in m.meshes:
                n = len(me.positions)
                v = np.zeros(n, dtype=MVTX_DT)
                v["p"] = np.clip(np.round(np.asarray(me.positions, np.float64) * (1 << frac)), -32768, 32767)
                v["uv"] = np.clip(np.round(me.uvs * 1024), -32768, 32767)
                v["n"] = pack_normals(me.normals)
                verts.append(v)
                first_idx = sum(len(x) for x in idx)
                ind = me.indices + base
                strip = stripify(ind)  # (strips: a third to half the vertices sent; frac's top bit says so)
                if strip is not None:
                    ind = strip
                idx.append(np.asarray(ind).astype(">u2"))
                mesh_rec.append(struct.pack(">IIHH", first_idx, len(ind),
                                            self.model_mats.get((nm, me.material), 0),
                                            frac | (0x8000 if strip is not None else 0)))
                base += n
            if base > 0xFFFF:
                raise SystemExit(f"{nm}: more than 65535 vertices in one model")
            vcount += base
            for h in m.hulls:
                try:
                    pl = convex_planes(h)
                except Exception:  # noqa: BLE001 (degenerate piece)
                    continue
                lo, hi = h.min(0), h.max(0)
                # axial bevels keep box sweeps from snagging on edges
                for ax in range(3):
                    for sgn, val in ((1, hi[ax]), (-1, -lo[ax])):
                        nrm = np.zeros(3)
                        nrm[ax] = sgn
                        if not any(np.allclose(nrm, q[0], atol=1e-3) for q in pl):
                            pl.append((nrm, val))
                hull_rec.append(struct.pack(">II3f3f", len(planes), len(pl), *lo, *hi))
                planes += pl
            mrec.append(struct.pack(">IIIIII3f3f", first_mesh, len(m.meshes), first_vert, base, first_hull,
                                    len(hull_rec) - first_hull, *m.mins, *m.maxs))
        chunk(b"MDLS", struct.pack(">I", len(mrec)) + b"".join(mrec))
        # MFAR: per model, the model that is its far LOD (0xFFFF none)
        far = [0xFFFF] * len(mrec)
        for k, nm in enumerate(far_names):
            far[self.model_index[nm]] = len(self.models) + k
        chunk(b"MFAR", struct.pack(f">{len(far)}H", *far))
        # MSRF: per model, its surface ($surfaceprop: impact and scrape sounds in weapons.hgm's SURF table)
        from surfaces import Surfaces
        surf = Surfaces(self.fs)
        sids = []
        for nm in self.models:
            sp = "default"
            try:
                raw = self.fs.read(nm)
                if raw and len(raw) > 312:
                    off = struct.unpack_from("<i", raw, 308)[0]
                    if 0 < off < len(raw):
                        sp = raw[off:off + 64].split(b"\0")[0].decode("latin-1") or "default"
            except Exception:  # noqa: BLE001
                pass
            sids.append(min(65535, surf.id_of(sp)))
        chunk(b"MSRF", b"".join(struct.pack(">H", x) for x in sids))
        # break pieces: per breakable model (or generic set, negative id), the models it breaks into
        gib_heads, gib_list = [], []
        for nm, pieces in getattr(self, "breaks", {}).items():
            ids = [self.model_index[p_] for p_, _ in pieces if p_ in self.model_index]
            if nm in self.model_index and ids:
                gib_heads.append(struct.pack(">hHHxxf", self.model_index[nm], len(gib_list), len(ids), pieces[0][1]))
                gib_list += ids
        for name, lst in getattr(self, "generic", {}).items():
            ids = [self.model_index[p_] for p_ in lst if p_ in self.model_index]
            if ids:
                gib_heads.append(struct.pack(">hHHxxf", GENERIC_IDS[name], len(gib_list), len(ids), 20.0))
                gib_list += ids
        chunk(b"GIBS", struct.pack(">I", len(gib_heads)) + b"".join(gib_heads))
        chunk(b"GIBL", struct.pack(f">{len(gib_list)}H", *gib_list) if gib_list else b"")
        chunk(b"MMSH", b"".join(mesh_rec))
        # np.concatenate returns native byte order: convert back to big-endian explicitly
        chunk(b"MVTX", np.concatenate(verts).astype(MVTX_DT).tobytes() if verts else b"")
        chunk(b"MIDX", np.concatenate(idx).astype(">u2").tobytes() if idx else b"")
        chunk(b"MHUL", b"".join(hull_rec))
        chunk(b"MPLN", b"".join(struct.pack(">4f", *n, d) for n, d in planes))

        recs, cubes, los, cranges, clist = [], [], [], [], []
        for sp in self.static:
            nm = self.static_names[sp.model] if sp.model < len(self.static_names) else None
            if nm not in self.model_index:
                continue
            lo = sp.light_origin
            if lo is None:
                # Source lights a static prop at its bounding box centre (rotated with the prop)
                m = self.models[nm]
                c = (np.asarray(m.mins, np.float64) + np.asarray(m.maxs, np.float64)) * 0.5
                lo = list(np.asarray(sp.origin) + np.asarray(angle_matrix(*sp.angles)) @ c)
            r, g, bb = self.tint(lo)
            cb = self.cube(lo)
            cubes.append(bytes(18) if cb is None else cb.tobytes())
            los.append(struct.pack(">3f", *[float(x) for x in lo]))
            # Source draws a static prop when any leaf it touches is visible (vbsp's leaf list): all of
            # their clusters go in SPCL; the record keeps the single one, or -1 when there are several.
            # (Clusters used to come from the origin: one under the floor, like Barney's office chair,
            # gave -1 and the engine never drew it.)
            cl = sorted({int(self.b.leafs[lf]["cluster"]) for lf in getattr(sp, "leaves", ())
                         if lf < len(self.b.leafs) and int(self.b.leafs[lf]["cluster"]) >= 0})
            if len(cl) == 1:
                cluster = cl[0]
            elif cl:
                cluster = -1
            else:
                cluster = int(self.b.leafs[self.b.leaf_at(np.array(sp.origin))]["cluster"])
                cl = [cluster] if cluster >= 0 else []
            cranges.append(struct.pack(">HH", len(clist), len(cl)))
            clist.extend(cl)
            recs.append(struct.pack(">HBB3f3f4Bhh", self.model_index[nm], 1 if sp.solid else 0, 0,
                                    *sp.origin, *sp.angles, r, g, bb, 255, cluster,
                                    # fademaxdist / 4 (the engine stops drawing it beyond that), 0 = never
                                    max(0, min(32767, int(sp.fade_max / 4))) if sp.fade_max > 0 else 0))
        chunk(b"SPRP", struct.pack(">I", len(recs)) + b"".join(recs))
        # SPCL: u32 props, u32 clusters, then per static prop (first, count), then the u16 cluster list:
        # every cluster the prop's leaves touch (it's drawn when any of them is visible)
        chunk(b"SPCL", struct.pack(">II", len(recs), len(clist)) + b"".join(cranges) +
              b"".join(struct.pack(">H", c) for c in clist))
        # SPCB: per static prop, its ambient cube (6 x RGB, +X -X +Y -Y +Z -Z; all zero = use the tint)
        chunk(b"SPCB", b"".join(cubes))
        # SPLO: per static prop, the point it's lit at (the engine adds the worldlights it can see there)
        chunk(b"SPLO", b"".join(los))
        # WLIT: the map's lights, for lighting models the way Source does (ambient cube + direct light)
        chunk(b"WLIT", worldlight_chunk(self.b))
        # LCUB: per BSP leaf, its ambient cube, for things that move (NPCs, physics props, doors)
        lc = []
        for leaf in range(len(self.b.leafs)):
            cb = self.b.ambient_faces_u8(leaf, CUBE_FLOOR) if self.b.leafs[leaf]["cluster"] >= 0 else None
            lc.append(bytes(18) if cb is None else cb.tobytes())
        chunk(b"LCUB", struct.pack(">I", len(lc)) + b"".join(lc))
        self.log(f"  static props: {len(recs)}  model verts: {vcount}  hull planes: {len(planes)}")

    def prop_data(self, mdl):
        """Breakable props: the model's prop_data (health, explosion), with its propdata.txt base class."""
        cache = self.__dict__.setdefault("_pd_cache", {})
        if mdl in cache:
            return cache[mdl]
        out = {}
        try:
            from sounds import parse_kv
            if "_pd_base" not in self.__dict__:
                self._pd_base = {}
                raw = self.fs.read("scripts/propdata.txt")
                for _, body in parse_kv(raw.decode("latin-1")) if raw else []:
                    if isinstance(body, list):
                        for name, vals in body:
                            if isinstance(vals, list):
                                self._pd_base[name.lower()] = {k.lower(): v for k, v in vals if isinstance(v, str)}
            d = self.fs.read(mdl) or b""
            i = d.find(b"mdlkeyvalue")
            kv = {}
            if i >= 0:
                j = d.find(b"\0", i)
                txt = d[i + len("mdlkeyvalue"):j].decode("latin-1")
                for k, body in parse_kv(txt):
                    if k.lower() == "prop_data" and isinstance(body, list):
                        kv = {x.lower(): y for x, y in body if isinstance(y, str)}
            chain, base = [kv], kv.get("base", "").lower()
            b0 = base.split(".")[0]
            out["material"] = {"wooden": 1, "cardboard": 1, "glass": 2, "metal": 3, "plastic": 3,
                               "stone": 4, "pottery": 4, "concrete": 4}.get(b0, 0)
            for _ in range(4):
                if not base or base not in self._pd_base:
                    break
                chain.append(self._pd_base[base])
                base = self._pd_base[base].get("base", "").lower()
            for c in reversed(chain):
                for k in ("health", "explosive_damage", "explosive_radius"):
                    if k in c:
                        try:
                            out[k] = float(c[k])
                        except ValueError:
                            pass
        except Exception as ex:  # noqa: BLE001
            self.log(f"  prop_data {mdl}: {ex}")
            out = {}
        cache[mdl] = out
        return out

    def entity_chunks(self, chunk, npcs=None, shared=None):
        """Every gameplay-relevant entity as key/value strings (outputs keep their order and repeats).
        NPC/scene/sound chunks share this string pool, so they are written from here too."""
        strings = {}
        blob = bytearray()

        def s(x):
            if x not in strings:
                strings[x] = len(blob)
                blob.extend(x.encode("latin-1", "replace") + b"\0")
            return strings[x]

        if npcs is not None:
            npcs.chunks(chunk, s, self.tint)
        ents, kvs = [], []
        for ei, e in enumerate(self.b.entities):
            cls = e.get("classname", "")
            # info_node_hint stays (act-busy spots, queue positions the scripts send NPCs to)
            # named lights stay: they switch their light style on and off (lightstyles.py)
            named_light = cls in ("light", "light_spot") and e.get("targetname") and e.get("style")
            if (cls in SKIP_CLASSES and not named_light) or (cls.startswith("info_node") and cls != "info_node_hint"):
                continue
            pairs = list(e["_pairs"])
            mdl = e.get("model", "").lower().replace("\\", "/")
            if mdl in self.model_index:
                pairs.append(("_mdl", str(self.model_index[mdl])))
                org = parse_vec(e.get("origin", "0 0 0"))
                pairs.append(("_tint", "%d %d %d" % self.tint([org[0], org[1], org[2] + 16])))
            if cls == "env_shooter" and shooter_model(e) in self.model_index:
                pairs.append(("_mdl", str(self.model_index[shooter_model(e)])))
            if mdl in self.models and cls.startswith("prop_physics"):
                pairs.append(("_mass", "%.1f" % (self.models[mdl].mass or 30.0)))
            if mdl.endswith(".mdl") and (cls.startswith("prop_") or cls == "item_item_crate"):
                pd = self.prop_data(mdl)
                if pd.get("health", 0) > 0:
                    pairs.append(("_hp", "%g" % pd["health"]))
                    if pd.get("material"):
                        pairs.append(("_mat", str(pd["material"])))
                if pd.get("explosive_damage", 0) > 0:
                    pairs.append(("_boom", "%g %g" % (pd["explosive_damage"], pd.get("explosive_radius", 0) or
                                                      pd["explosive_damage"] * 2.5)))
            pairs += getattr(self, "sprite_keys", {}).get(ei, [])
            if cls in ("move_rope", "keyframe_rope"):  # the rope's colour (hgmpatch.rope_colour)
                import hgmpatch
                pairs = hgmpatch.rope_pairs(self.fs, e)
            if npcs is not None:
                pairs += npcs.extra_keys(ei, e, self.tint)
            if shared is not None and cls in ("env_message", "game_text", "env_hudhint"):
                import frontend
                pairs += frontend.text_keys(e, shared.loc, shared.titles)
            first = len(kvs)
            for k, v in pairs:
                kvs.append(struct.pack(">II", s(k.lower()), s(v)))
            ents.append(struct.pack(">II", first, len(pairs)))
        # worldspawn's "chaptertitle": Source's CWorld makes an env_message for it that shows itself once,
        # a second after the map starts (the chapter's name, written out in titles.txt's effect)
        world = self.b.entities[0] if self.b.entities else {}
        if world.get("classname") == "worldspawn" and world.get("chaptertitle"):
            ct = {"classname": "env_message", "targetname": "chapter_title", "message": world["chaptertitle"]}
            pairs = list(ct.items()) + [("spawnflags", "1"), ("_auto", "1")]
            if shared is not None:
                import frontend
                pairs += frontend.text_keys(ct, shared.loc, shared.titles)
            first = len(kvs)
            for k, v in pairs:
                kvs.append(struct.pack(">II", s(k.lower()), s(v)))
            ents.append(struct.pack(">II", first, len(pairs)))
        chunk(b"ENTS", struct.pack(">I", len(ents)) + b"".join(ents))
        chunk(b"EKVS", b"".join(kvs))
        chunk(b"ESTR", bytes(blob))
        self.log(f"  entities: {len(ents)} ({len(kvs)} keys, {len(blob) // 1024} KB strings)")
