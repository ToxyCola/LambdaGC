"""Patch converted .hgm packs in place without reconverting them.

  python hgmpatch.py ropes <game_dir> <maps_dir>
      adds the map's ropes (move_rope / keyframe_rope: they used to be dropped at conversion) to every
      .hgm in maps_dir, with their colour (_ropecolor: the average of the rope material's texture)

The pack layout is convert.py's: "HGM1", version, chunk count, then 12-byte (tag, offset, size)
entries, each chunk 32-byte aligned.
"""
import os
import struct
import sys

ROPE_CLASSES = ("move_rope", "keyframe_rope")


def align(n):
    return (n + 31) & ~31


def read_pack(path):
    d = open(path, "rb").read()
    magic, version, n = struct.unpack_from(">4sII", d, 0)
    assert magic == b"HGM1", path
    chunks = []
    for i in range(n):
        tag, off, size = struct.unpack_from(">4sII", d, 16 + i * 12)
        chunks.append([tag, d[off:off + size]])
    return version, chunks


def write_pack(path, version, chunks):
    out = bytearray(align(16 + 12 * len(chunks)))
    struct.pack_into(">4sII", out, 0, b"HGM1", version, len(chunks))
    for i, (tag, payload) in enumerate(chunks):
        struct.pack_into(">4sII", out, 16 + i * 12, tag, len(out), len(payload))
        out += payload
        out += b"\0" * (align(len(out)) - len(out))
    open(path, "wb").write(out)


def get(chunks, tag):
    for c in chunks:
        if c[0] == tag:
            return c
    raise KeyError(tag)


def entity_pairs(chunks):
    """the pack's entities as lists of (key, value)"""
    ents, kvs, strs = get(chunks, b"ENTS")[1], get(chunks, b"EKVS")[1], get(chunks, b"ESTR")[1]

    def s(o):
        return strs[o:strs.index(b"\0", o)].decode("latin-1")

    n = struct.unpack_from(">I", ents, 0)[0]
    out = []
    for i in range(n):
        first, cnt = struct.unpack_from(">II", ents, 4 + i * 8)
        out.append([(s(k), s(v)) for k, v in struct.iter_unpack(">II", kvs[first * 8:(first + cnt) * 8])])
    return out


def append_entities(chunks, new):
    """add entities (lists of (key, value)) at the end; existing ones keep their indices and strings"""
    ec, kc, sc = get(chunks, b"ENTS"), get(chunks, b"EKVS"), get(chunks, b"ESTR")
    n = struct.unpack_from(">I", ec[1], 0)[0]
    ents = bytearray(ec[1])
    kvs = bytearray(kc[1])
    blob = bytearray(sc[1])
    have = {}

    def s(x):
        if x not in have:
            have[x] = len(blob)
            blob.extend(x.encode("latin-1", "replace") + b"\0")
        return have[x]

    for pairs in new:
        first = len(kvs) // 8
        for k, v in pairs:
            kvs += struct.pack(">II", s(k.lower()), s(v))
        ents += struct.pack(">II", first, len(pairs))
    struct.pack_into(">I", ents, 0, n + len(new))
    ec[1], kc[1], sc[1] = bytes(ents), bytes(kvs), bytes(blob)


_colours = {}


def rope_colour(fs, material):
    """the rope's colour: the average of its material's base texture (Source draws it textured; the
    ropes are a few pixels wide, so the average is what shows)"""
    name = (material or "cable/cable.vmt").lower().replace("\\", "/")
    if name.startswith("materials/"):
        name = name[10:]
    if name.endswith(".vmt"):
        name = name[:-4]
    if name in _colours:
        return _colours[name]
    col = (40, 40, 40)
    try:
        import material
        from vtf import VTF
        m = material.load_material(fs, name)
        tex = getattr(m, "basetexture", None)
        raw = fs.read(f"materials/{tex}.vtf") if tex else None
        if raw:
            v = VTF(raw)
            lv = max(0, v.mips - 4)
            px = v.rgba(lv)
            import numpy as np
            a = np.frombuffer(bytes(px), np.uint8).reshape(-1, 4).astype(float)
            w = a[:, 3] / 255.0 if v.has_alpha_format() else np.ones(len(a))
            if w.sum() > 0:
                col = tuple(int(round((a[:, k] * w).sum() / w.sum())) for k in range(3))
    except Exception as ex:  # a missing or odd material: the default dark cable
        print("  rope material", name, ex)
    _colours[name] = col
    return col


def rope_pairs(fs, e):
    pairs = list(e["_pairs"])
    pairs.append(("_ropecolor", "%d %d %d" % rope_colour(fs, e.get("RopeMaterial") or e.get("ropematerial"))))
    return pairs


def patch_ropes(game_dir, maps_dir):
    from vpk import GameFS
    from bsp import BSP
    fs = GameFS(game_dir)
    for f in sorted(os.listdir(maps_dir)):
        if not f.endswith(".hgm"):
            continue
        path = os.path.join(maps_dir, f)
        raw = fs.read(f"maps/{f[:-4]}.bsp")
        if not raw:
            continue
        b = BSP(raw)
        ropes = [e for e in b.entities if e.get("classname", "").lower() in ROPE_CLASSES]
        version, chunks = read_pack(path)
        have = sum(1 for p in entity_pairs(chunks) if dict(p).get("classname", "").lower() in ROPE_CLASSES)
        if not ropes or have:
            print(f"{f}: {len(ropes)} ropes, {have} already in")
            continue
        append_entities(chunks, [rope_pairs(fs, e) for e in ropes])
        write_pack(path, version, chunks)
        print(f"{f}: added {len(ropes)} ropes")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "ropes":
        patch_ropes(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
