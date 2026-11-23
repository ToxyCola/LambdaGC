"""Source BSP (v19/v20) parser covering what the GameCube converter needs."""
import io
import struct
import zipfile

import numpy as np

LUMP_ENTITIES = 0
LUMP_PLANES = 1
LUMP_TEXDATA = 2
LUMP_VERTEXES = 3
LUMP_VISIBILITY = 4
LUMP_NODES = 5
LUMP_TEXINFO = 6
LUMP_FACES = 7
LUMP_LIGHTING = 8
LUMP_LEAFS = 10
LUMP_EDGES = 12
LUMP_SURFEDGES = 13
LUMP_MODELS = 14
LUMP_LEAFFACES = 16
LUMP_LEAFBRUSHES = 17
LUMP_BRUSHES = 18
LUMP_BRUSHSIDES = 19
LUMP_DISPINFO = 26
LUMP_DISP_VERTS = 33
LUMP_GAME_LUMP = 35
LUMP_PAKFILE = 40
LUMP_TEXDATA_STRING_DATA = 43
LUMP_TEXDATA_STRING_TABLE = 44
LUMP_LEAF_AMBIENT_INDEX_HDR = 51
LUMP_LEAF_AMBIENT_INDEX = 52
LUMP_LIGHTING_HDR = 53
LUMP_LEAF_AMBIENT_LIGHTING_HDR = 55
LUMP_LEAF_AMBIENT_LIGHTING = 56
LUMP_FACES_HDR = 58

# texinfo flags
SURF_SKY2D = 0x0002
SURF_SKY = 0x0004
SURF_WARP = 0x0008
SURF_TRANS = 0x0010
SURF_TRIGGER = 0x0040
SURF_NODRAW = 0x0080
SURF_HINT = 0x0100
SURF_SKIP = 0x0200
SURF_NOLIGHT = 0x0400
SURF_BUMPLIGHT = 0x0800

# brush contents
CONTENTS_SOLID = 0x1
CONTENTS_WINDOW = 0x2
CONTENTS_GRATE = 0x8
CONTENTS_WATER = 0x20
CONTENTS_MOVEABLE = 0x4000
CONTENTS_PLAYERCLIP = 0x10000
CONTENTS_LADDER = 0x20000000
MASK_PLAYERSOLID = CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_GRATE


def light_u8(lin, floor=0.0):
    """linear light (lightmap scale, 1.0 = full) -> 8-bit gamma at half intensity, as the lightmaps"""
    g = np.clip(np.power(np.clip(np.asarray(lin, np.float64), 0, None), 1 / 2.2), floor, 2.0)
    return (g * 0.5 * 255 + 0.5).clip(0, 255).astype(np.uint8)


WORLDLIGHT_DT = np.dtype([("origin", "<3f4"), ("intensity", "<3f4"), ("normal", "<3f4"), ("cluster", "<i4"),
                          ("type", "<i4"), ("style", "<i4"), ("stopdot", "<f4"), ("stopdot2", "<f4"),
                          ("exponent", "<f4"), ("radius", "<f4"), ("c", "<f4"), ("l", "<f4"), ("q", "<f4"),
                          ("flags", "<i4"), ("texinfo", "<i4"), ("owner", "<i4")])
LUMP_WORLDLIGHTS = 15
LUMP_WORLDLIGHTS_HDR = 54


class BSP:
    def __init__(self, data: bytes):
        self.data = data
        ident, self.version = struct.unpack_from("<4si", data, 0)
        if ident != b"VBSP":
            raise ValueError("not a VBSP file")
        if self.version not in (19, 20):
            raise ValueError(f"unsupported BSP version {self.version}")
        self.lumps = [struct.unpack_from("<iiii", data, 8 + i * 16) for i in range(64)]
        for i, (_, length, _, fourcc) in enumerate(self.lumps):
            if length and fourcc:
                raise NotImplementedError(f"lump {i} is LZMA-compressed (console map?)")
        self._parse()

    def lump(self, i):
        off, length, _, _ = self.lumps[i]
        return self.data[off : off + length]

    def lump_version(self, i):
        return self.lumps[i][2]

    def _arr(self, i, dtype):
        raw = self.lump(i)
        dt = np.dtype(dtype)
        return np.frombuffer(raw[: len(raw) // dt.itemsize * dt.itemsize], dtype=dt)

    def _parse(self):
        self.planes = self._arr(LUMP_PLANES, [("normal", "<3f4"), ("dist", "<f4"), ("type", "<i4")])
        self.vertexes = self._arr(LUMP_VERTEXES, ("<f4", 3))
        self.edges = self._arr(LUMP_EDGES, ("<u2", 2))
        self.surfedges = self._arr(LUMP_SURFEDGES, "<i4")
        self.texinfo = self._arr(
            LUMP_TEXINFO,
            [("tex", "<f4", (2, 4)), ("lm", "<f4", (2, 4)), ("flags", "<i4"), ("texdata", "<i4")],
        )
        self.texdata = self._arr(
            LUMP_TEXDATA,
            [("reflectivity", "<3f4"), ("name", "<i4"), ("w", "<i4"), ("h", "<i4"), ("vw", "<i4"), ("vh", "<i4")],
        )
        face_dt = [
            ("planenum", "<u2"), ("side", "u1"), ("onNode", "u1"),
            ("firstedge", "<i4"), ("numedges", "<i2"), ("texinfo", "<i2"),
            ("dispinfo", "<i2"), ("fogvol", "<i2"), ("styles", "u1", 4),
            ("lightofs", "<i4"), ("area", "<f4"),
            ("lmMins", "<i4", 2), ("lmSize", "<i4", 2),
            ("origFace", "<i4"), ("numPrims", "<u2"), ("firstPrim", "<u2"), ("smoothing", "<u4"),
        ]
        self.faces = self._arr(LUMP_FACES, face_dt)
        self.lighting = self.lump(LUMP_LIGHTING)
        if not self.lighting and self.lumps[LUMP_LIGHTING_HDR][1]:
            # HDR-only map: fall back to HDR faces/lighting (same layout)
            self.lighting = self.lump(LUMP_LIGHTING_HDR)
            self.faces = self._arr(LUMP_FACES_HDR, face_dt)
        node_dt = [
            ("planenum", "<i4"), ("children", "<i4", 2), ("mins", "<i2", 3), ("maxs", "<i2", 3),
            ("firstface", "<u2"), ("numfaces", "<u2"), ("area", "<i2"), ("pad", "<i2"),
        ]
        self.nodes = self._arr(LUMP_NODES, node_dt)
        leaf_base = [
            ("contents", "<i4"), ("cluster", "<i2"), ("areaflags", "<i2"),
            ("mins", "<i2", 3), ("maxs", "<i2", 3),
            ("firstleafface", "<u2"), ("numleaffaces", "<u2"),
            ("firstleafbrush", "<u2"), ("numleafbrushes", "<u2"), ("water", "<i2"),
        ]
        if self.version == 19 and self.lump_version(LUMP_LEAFS) == 0:
            leaf_dt = leaf_base + [("ambient", "u1", 24), ("pad", "<i2")]
        else:
            leaf_dt = leaf_base + [("pad", "<i2")]
        self.leafs = self._arr(LUMP_LEAFS, leaf_dt)
        self.leaffaces = self._arr(LUMP_LEAFFACES, "<u2")
        self.leafbrushes = self._arr(LUMP_LEAFBRUSHES, "<u2")
        self.brushes = self._arr(LUMP_BRUSHES, [("firstside", "<i4"), ("numsides", "<i4"), ("contents", "<i4")])
        self.brushsides = self._arr(LUMP_BRUSHSIDES, [("planenum", "<u2"), ("texinfo", "<i2"), ("dispinfo", "<i2"),
                                                      ("bevel", "<i2")])
        self.models = self._arr(
            LUMP_MODELS,
            [("mins", "<3f4"), ("maxs", "<3f4"), ("origin", "<3f4"), ("headnode", "<i4"),
             ("firstface", "<i4"), ("numfaces", "<i4")],
        )
        self.dispinfo = self._arr(
            LUMP_DISPINFO,
            [("startPos", "<3f4"), ("dispVertStart", "<i4"), ("dispTriStart", "<i4"),
             ("power", "<i4"), ("minTess", "<i4"), ("smoothAngle", "<f4"), ("contents", "<i4"),
             ("mapFace", "<u2"), ("lmAlphaStart", "<i4"), ("lmSamplePosStart", "<i4"),
             ("rest", "u1", 130)],
        )
        assert self.dispinfo.dtype.itemsize == 176, self.dispinfo.dtype.itemsize
        self.dispverts = self._arr(LUMP_DISP_VERTS, [("vec", "<3f4"), ("dist", "<f4"), ("alpha", "<f4")])

        strdata = self.lump(LUMP_TEXDATA_STRING_DATA)
        table = self._arr(LUMP_TEXDATA_STRING_TABLE, "<i4")
        self.texnames = []
        for td in self.texdata:
            off = table[td["name"]]
            end = strdata.index(b"\0", off)
            self.texnames.append(strdata[off:end].decode("latin-1"))

        self.entities = parse_entities(self.lump(LUMP_ENTITIES).rstrip(b"\0").decode("latin-1"))

        self.vis = None
        vis = self.lump(LUMP_VISIBILITY)
        if vis:
            n = struct.unpack_from("<i", vis, 0)[0]
            offs = np.frombuffer(vis[4 : 4 + 8 * n], dtype="<i4").reshape(n, 2)
            self.vis = (n, vis, offs)

    def leaf_at(self, pos):
        node = 0
        while node >= 0:
            n = self.nodes[node]
            pl = self.planes[n["planenum"]]
            node = int(n["children"][0 if float(np.dot(pl["normal"], pos)) - pl["dist"] >= 0 else 1])
        return -1 - node

    def ambient_cube(self, leaf):
        """Average ambient light (linear RGB, 0..~1) for a leaf, from its ambient cube samples."""
        cubes = []
        if "ambient" in self.leafs.dtype.names:
            cubes.append(self.leafs[leaf]["ambient"])
        else:
            for idx_l, smp_l in ((LUMP_LEAF_AMBIENT_INDEX, LUMP_LEAF_AMBIENT_LIGHTING),
                                 (LUMP_LEAF_AMBIENT_INDEX_HDR, LUMP_LEAF_AMBIENT_LIGHTING_HDR)):
                idx = self._arr(idx_l, [("count", "<u2"), ("first", "<u2")])
                smp = self._arr(smp_l, [("cube", "u1", 24), ("pos", "u1", 4)])
                if len(idx) > leaf and len(smp) and smp["cube"][:, :3].any():
                    c, f = int(idx[leaf]["count"]), int(idx[leaf]["first"])
                    cubes += [smp[k]["cube"] for k in range(f, min(f + c, len(smp)))]
                    break  # use the first lump set that actually has data (LDR is all zero in some maps)
        if not cubes:
            return None
        a = np.array(cubes, dtype=np.uint8).reshape(-1, 6, 4)
        exp = a[..., 3].astype(np.int8).astype(np.float32)
        lin = a[..., :3].astype(np.float32) * np.exp2(exp)[..., None] / 255.0
        return lin.reshape(-1, 3).mean(0)

    def ambient_faces(self, leaf):
        """The leaf's ambient cube, 6 faces (+X -X +Y -Y +Z -Z) of linear RGB, averaged over its samples"""
        cubes = []
        if "ambient" in self.leafs.dtype.names:
            cubes.append(self.leafs[leaf]["ambient"])
        else:
            for idx_l, smp_l in ((LUMP_LEAF_AMBIENT_INDEX, LUMP_LEAF_AMBIENT_LIGHTING),
                                 (LUMP_LEAF_AMBIENT_INDEX_HDR, LUMP_LEAF_AMBIENT_LIGHTING_HDR)):
                idx = self._arr(idx_l, [("count", "<u2"), ("first", "<u2")])
                smp = self._arr(smp_l, [("cube", "u1", 24), ("pos", "u1", 4)])
                if len(idx) > leaf and len(smp) and smp["cube"][:, :3].any():
                    c, f = int(idx[leaf]["count"]), int(idx[leaf]["first"])
                    cubes += [smp[k]["cube"] for k in range(f, min(f + c, len(smp)))]
                    break
        if not cubes:
            return None
        a = np.array(cubes, dtype=np.uint8).reshape(-1, 6, 4)
        exp = a[..., 3].astype(np.int8).astype(np.float32)
        lin = a[..., :3].astype(np.float32) * np.exp2(exp)[..., None] / 255.0
        return lin.mean(0)

    def ambient_faces_u8(self, leaf, floor):
        """ambient_faces in the lightmaps' scale (gamma, halved: the TEV doubles it), or None. Leaf ambient
        samples are stored 255x smaller than lightmaps (c * 2^exp is already 0..1 linear for them);
        the engine adds the map's worldlights on top (Source lights models with both)."""
        if leaf < 0 or leaf >= len(self.leafs):
            return None
        c = self.ambient_faces(leaf)
        if c is None or c.max() <= 0:
            return None
        return light_u8(c * 255.0, floor)

    def worldlights(self):
        """dworldlight_t records (LDR lump; 88 bytes in v19, 100 with shadow_cast_offset in v20)"""
        lump = LUMP_WORLDLIGHTS if self.lumps[LUMP_WORLDLIGHTS][1] else LUMP_WORLDLIGHTS_HDR
        raw = self.lump(lump)
        if not raw:
            return np.zeros(0, WORLDLIGHT_DT)
        size = 100 if self.lump_version(lump) >= 1 else 88
        if size == 88:
            return np.frombuffer(raw[: len(raw) // 88 * 88], WORLDLIGHT_DT).copy()
        a = np.frombuffer(raw[: len(raw) // 100 * 100], np.uint8).reshape(-1, 100)
        return np.frombuffer(np.concatenate([a[:, :36], a[:, 48:]], 1).tobytes(), WORLDLIGHT_DT).copy()

    def ambient_reference(self):
        """Brightness the map's well-lit areas reach (90th percentile over in-world leaves); used to
        normalise prop lighting, since ambient samples use a different scale than lightmaps."""
        if getattr(self, "_amb_ref", None) is None:
            vals = []
            for leaf in range(0, len(self.leafs), max(1, len(self.leafs) // 800)):
                if self.leafs[leaf]["cluster"] < 0:
                    continue
                c = self.ambient_cube(leaf)
                if c is not None and c.max() > 0:
                    vals.append(float(c.mean()))
            self._amb_ref = float(np.percentile(vals, 90)) if vals else 1.0
        return self._amb_ref

    def pakfile(self):
        raw = self.lump(LUMP_PAKFILE)
        out = {}
        if raw:
            with zipfile.ZipFile(io.BytesIO(raw)) as z:
                for info in z.infolist():
                    out[info.filename.lower().replace("\\", "/")] = z.read(info)
        return out

    def game_lumps(self):
        raw = self.lump(LUMP_GAME_LUMP)
        base = self.lumps[LUMP_GAME_LUMP][0]
        n = struct.unpack_from("<i", raw, 0)[0]
        out = {}
        for i in range(n):
            gid, flags, ver, off, length = struct.unpack_from("<iHHii", raw, 4 + i * 16)
            out[struct.pack(">i", gid).decode("latin-1")] = (ver, self.data[off : off + length])
        return out

    def decompress_pvs(self, cluster):
        """Return a bool array of clusters visible from `cluster`."""
        n, vis, offs = self.vis
        out = np.zeros(n, dtype=bool)
        p = offs[cluster][0]
        c = 0
        while c < n:
            b = vis[p]
            if b == 0:
                c += 8 * vis[p + 1]
                p += 2
                continue
            for bit in range(8):
                if c + bit < n and (b >> bit) & 1:
                    out[c + bit] = True
            c += 8
            p += 1
        return out


def parse_entities(text):
    ents = []
    cur = None
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "{":
            cur = {"_pairs": []}  # _pairs keeps every key/value in order (outputs repeat keys)
            i += 1
        elif ch == "}":
            if cur is not None:
                ents.append(cur)
            cur = None
            i += 1
        elif ch == '"':
            e = text.index('"', i + 1)
            key = text[i + 1 : e]
            s = text.index('"', e + 1)
            e2 = text.index('"', s + 1)
            val = text[s + 1 : e2]
            if cur is not None:
                cur["_pairs"].append((key, val))
                if key not in cur:
                    cur[key] = val
            i = e2 + 1
        else:
            i += 1
    return ents
