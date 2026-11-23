"""Reader for scenes/scenes.image (VSIF) and the compiled 'bvcd' choreography scenes inside it.

The binary layout mirrors CChoreoScene::SaveToBinaryBuffer from the Source SDK: every string is a
16-bit index into the image's shared string pool.
"""
import lzma
import struct
import zlib

# CChoreoEvent::EVENTTYPE
EV_NAMES = ["unspecified", "section", "expression", "lookat", "moveto", "speak", "gesture", "sequence",
            "face", "firetrigger", "flexanimation", "subscene", "loop", "interrupt", "stoppoint",
            "permitresponses", "generic"]
EV = {n: i for i, n in enumerate(EV_NAMES)}


def _unlzma(d):
    if d[:4] != b"LZMA":
        return d
    actual, packed = struct.unpack_from("<II", d, 4)
    props = d[12:17]
    pb = props[0] // 45
    lp = (props[0] % 45) // 9
    lc = props[0] % 9
    dict_size = struct.unpack_from("<I", props, 1)[0]
    dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA1, "dict_size": dict_size,
                                                             "lc": lc, "lp": lp, "pb": pb}])
    return dec.decompress(d[17:17 + packed], max_length=actual)


def scene_crc(name):
    n = name.lower().replace("/", "\\")
    if not n.endswith(".vcd"):
        n = n.rsplit(".", 1)[0] + ".vcd" if "." in n.rsplit("\\", 1)[-1] else n + ".vcd"
    return zlib.crc32(n.encode("latin-1")) & 0xFFFFFFFF


class Reader:
    def __init__(self, data, pool):
        self.d = data
        self.p = 0
        self.pool = pool

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def s8(self):
        v = struct.unpack_from("<b", self.d, self.p)[0]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]
        self.p += 2
        return v

    def s16(self):
        v = struct.unpack_from("<h", self.d, self.p)[0]
        self.p += 2
        return v

    def f32(self):
        v = struct.unpack_from("<f", self.d, self.p)[0]
        self.p += 4
        return v

    def s32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def string(self):
        i = self.s16()
        return self.pool[i] if 0 <= i < len(self.pool) else ""


class Event:
    __slots__ = ("type", "name", "start", "end", "p1", "p2", "p3", "flags", "dist", "rel_tags", "timing_tags",
                 "abs_tags", "gesture_dur", "rel_tag", "loop_count", "cc_type", "cc_token", "cc_flags",
                 "ramp", "flex")

    @property
    def tname(self):
        return EV_NAMES[self.type] if 0 <= self.type < len(EV_NAMES) else str(self.type)


def _curve(r):
    n = r.u8()
    return [(r.f32(), r.u8() / 255.0) for _ in range(n)]


def _event(r):
    e = Event()
    e.type = r.u8()
    e.name = r.string()
    e.start = r.f32()
    e.end = r.f32()
    e.p1 = r.string()
    e.p2 = r.string()
    e.p3 = r.string()
    e.ramp = _curve(r)
    e.flags = r.u8()
    e.dist = r.f32()
    e.rel_tags = [(r.string(), r.u8() / 255.0) for _ in range(r.u8())]
    e.timing_tags = [(r.string(), r.u8() / 255.0) for _ in range(r.u8())]
    e.abs_tags = []
    for _ in range(2):
        e.abs_tags.append([(r.string(), r.u16() / 4096.0) for _ in range(r.u8())])
    e.gesture_dur = r.f32() if e.type == EV["gesture"] else 0.0
    e.rel_tag = (r.string(), r.string()) if r.u8() else None
    e.flex = []
    for _ in range(r.u8()):
        name = r.string()
        fl = r.u8()
        lo, hi = r.f32(), r.f32()
        tracks = [[(r.f32(), r.u8() / 255.0, r.u16()) for _ in range(r.u16())]]
        if fl & 2:
            tracks.append([(r.f32(), r.u8() / 255.0, r.u16()) for _ in range(r.u16())])
        e.flex.append((name, fl, lo, hi, tracks))
    e.loop_count = r.s8() if e.type == EV["loop"] else 0
    if e.type == EV["speak"]:
        e.cc_type = r.u8()
        e.cc_token = r.string()
        e.cc_flags = r.u8()
    else:
        e.cc_type, e.cc_token, e.cc_flags = 0, "", 0
    return e


class Scene:
    def __init__(self, data, pool):
        r = Reader(data, pool)
        if data[:4] != b"bvcd":
            raise ValueError("not bvcd")
        r.p = 4
        self.version = r.u8()
        self.crc = r.s32()
        self.events = [_event(r) for _ in range(r.u8())]  # scene-level (no actor)
        self.actors = []
        for _ in range(r.u8()):
            aname = r.string()
            chans = []
            for _ in range(r.u8()):
                cname = r.string()
                evs = [_event(r) for _ in range(r.u8())]
                chans.append((cname, evs, r.u8()))
            self.actors.append((aname, chans, r.u8()))
        self.ramp = _curve(r)
        self.ignore_phonemes = r.u8()
        self.consumed = r.p
        self.size = len(data)

    def all_events(self):
        for e in self.events:
            yield None, e
        for aname, chans, _ in self.actors:
            for _, evs, _ in chans:
                for e in evs:
                    yield aname, e

    @property
    def duration(self):
        return max([e.end if e.end > e.start else e.start for _, e in self.all_events()] + [0.0])


class SceneImage:
    def __init__(self, data):
        sig, ver, ns, nstr, eoff = struct.unpack_from("<4si3i", data, 0)
        if sig != b"VSIF":
            raise ValueError("not VSIF")
        self.version = ver
        offs = struct.unpack_from(f"<{nstr}i", data, 20)
        self.pool = []
        for o in offs:
            e = data.index(b"\0", o)
            self.pool.append(data[o:e].decode("latin-1"))
        self.entries = {}
        for i in range(ns):
            crc, doff, dlen, soff = struct.unpack_from("<4I", data, eoff + 16 * i)
            self.entries[crc] = (doff, dlen, soff)
        self.data = data

    def raw(self, name):
        e = self.entries.get(scene_crc(name))
        if not e:
            return None
        return _unlzma(self.data[e[0]:e[0] + e[1]])

    def summary(self, name):
        e = self.entries.get(scene_crc(name))
        if not e:
            return None
        msecs, nsnd = struct.unpack_from("<Ii", self.data, e[2])
        snd = struct.unpack_from(f"<{nsnd}i", self.data, e[2] + 8)
        return msecs, [self.pool[i] for i in snd]

    def scene(self, name):
        d = self.raw(name)
        return Scene(d, self.pool) if d else None

    def all_scenes(self):
        for crc, (doff, dlen, _) in self.entries.items():
            yield crc, Scene(_unlzma(self.data[doff:doff + dlen]), self.pool)
