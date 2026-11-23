"""VTF reader (7.0-7.5) and DXT decoding with numpy."""
import struct

import numpy as np

FMT_RGBA8888 = 0
FMT_ABGR8888 = 1
FMT_RGB888 = 2
FMT_BGR888 = 3
FMT_RGB565 = 4
FMT_I8 = 5
FMT_IA88 = 6
FMT_A8 = 8
FMT_RGB888_BLUESCREEN = 9
FMT_BGR888_BLUESCREEN = 10
FMT_ARGB8888 = 11
FMT_BGRA8888 = 12
FMT_DXT1 = 13
FMT_DXT3 = 14
FMT_DXT5 = 15
FMT_BGRX8888 = 16
FMT_BGR565 = 17
FMT_BGRX5551 = 18
FMT_BGRA4444 = 19
FMT_DXT1_ONEBITALPHA = 20
FMT_BGRA5551 = 21
FMT_UV88 = 22
FMT_RGBA16161616F = 24
FMT_RGBA16161616 = 25

_BPP = {
    FMT_RGBA8888: 32, FMT_ABGR8888: 32, FMT_RGB888: 24, FMT_BGR888: 24, FMT_RGB565: 16,
    FMT_I8: 8, FMT_IA88: 16, FMT_A8: 8, FMT_RGB888_BLUESCREEN: 24, FMT_BGR888_BLUESCREEN: 24,
    FMT_ARGB8888: 32, FMT_BGRA8888: 32, FMT_BGRX8888: 32, FMT_BGR565: 16, FMT_BGRX5551: 16,
    FMT_BGRA4444: 16, FMT_BGRA5551: 16, FMT_UV88: 16, FMT_RGBA16161616F: 64, FMT_RGBA16161616: 64,
}
_DXT = {FMT_DXT1: 8, FMT_DXT1_ONEBITALPHA: 8, FMT_DXT3: 16, FMT_DXT5: 16}

TEXTUREFLAGS_ENVMAP = 0x4000


def level_size(fmt, w, h):
    if fmt in _DXT:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * _DXT[fmt]
    return w * h * _BPP[fmt] // 8


class VTF:
    def __init__(self, data: bytes):
        if data[:4] != b"VTF\0":
            raise ValueError("not a VTF")
        major, minor, hdr = struct.unpack_from("<III", data, 4)
        self.width, self.height, self.flags, self.frames, _first = struct.unpack_from("<HHIHH", data, 16)
        self.fmt = struct.unpack_from("<i", data, 52)[0]
        self.mips = data[56]
        lowfmt = struct.unpack_from("<i", data, 57)[0]
        loww, lowh = data[61], data[62]
        depth = 1
        if minor >= 2:
            depth = struct.unpack_from("<H", data, 63)[0] or 1
        faces = 6 if self.flags & TEXTUREFLAGS_ENVMAP else 1
        if self.flags & TEXTUREFLAGS_ENVMAP and minor < 5 and _first == 0xFFFF:
            faces = 7  # old spheremap face
        self.faces = faces
        self.depth = depth
        hi_off = None
        if minor >= 3:
            nres = struct.unpack_from("<I", data, 68)[0]
            for i in range(nres):
                tag, rflags, val = struct.unpack_from("<3sBI", data, 80 + i * 8)
                if tag == b"\x30\0\0":
                    hi_off = val
        if hi_off is None:
            low = level_size(lowfmt, loww, lowh) if lowfmt != -1 and loww else 0
            hi_off = hdr + low
        self.data = data
        self.hi_off = hi_off

    def mip_dims(self, level):
        return max(1, self.width >> level), max(1, self.height >> level)

    def raw_mip(self, level, frame=0, face=0):
        """Raw bytes of one mip level (level 0 = largest). VTF stores smallest mip first."""
        off = self.hi_off
        per_image = lambda m: level_size(self.fmt, *self.mip_dims(m)) * max(1, self.depth >> m)
        for m in range(self.mips - 1, level, -1):
            off += per_image(m) * self.frames * self.faces
        off += per_image(level) * (frame * self.faces + face)
        return self.data[off : off + level_size(self.fmt, *self.mip_dims(level))]

    def rgba(self, level, frame=0):
        w, h = self.mip_dims(level)
        return decode(self.fmt, self.raw_mip(level, frame), w, h)

    def has_alpha_format(self):
        return self.fmt in (FMT_DXT3, FMT_DXT5, FMT_DXT1_ONEBITALPHA, FMT_BGRA8888, FMT_RGBA8888,
                            FMT_ABGR8888, FMT_ARGB8888, FMT_IA88, FMT_A8, FMT_BGRA4444, FMT_BGRA5551)


def _565(c):
    r = ((c >> 11) & 31).astype(np.uint32)
    g = ((c >> 5) & 63).astype(np.uint32)
    b = (c & 31).astype(np.uint32)
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def _dxt_color(blocks, punch):
    """blocks: (N,8) uint8 -> (N,16,4) uint8"""
    c0 = blocks[:, 0].astype(np.uint16) | (blocks[:, 1].astype(np.uint16) << 8)
    c1 = blocks[:, 2].astype(np.uint16) | (blocks[:, 3].astype(np.uint16) << 8)
    p0, p1 = _565(c0), _565(c1)
    four = (c0 > c1) | (not punch)
    four = np.asarray(four)
    if four.ndim == 0:
        four = np.full(len(blocks), bool(four))
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # N,4,3
    alpha = np.full((len(blocks), 4), 255, np.int32)
    alpha[:, 3] = np.where(four, 255, 0)
    bits = blocks[:, 4:8].astype(np.uint32)
    idx = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    sel = (idx[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    rgb = np.take_along_axis(pal, sel[:, :, None].astype(np.int64), 1)
    a = np.take_along_axis(alpha, sel.astype(np.int64), 1)
    return np.concatenate([rgb, a[:, :, None]], -1).astype(np.uint8)


def _dxt5_alpha(blocks):
    a0 = blocks[:, 0].astype(np.int32)
    a1 = blocks[:, 1].astype(np.int32)
    pal = np.zeros((len(blocks), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    gt = a0 > a1
    for i in range(1, 7):
        pal[:, i + 1] = np.where(gt, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, i + 1] = np.where(gt, pal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(gt, pal[:, 6], 0)
    pal[:, 7] = np.where(gt, pal[:, 7], 255)
    bits = np.zeros(len(blocks), np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = (bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)
    return np.take_along_axis(pal, sel.astype(np.int64), 1).astype(np.uint8)


def _unblock(px, w, h):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w]


def decode(fmt, raw, w, h):
    """-> (h,w,4) uint8 RGBA"""
    if fmt in (FMT_DXT1, FMT_DXT1_ONEBITALPHA):
        b = np.frombuffer(raw, np.uint8).reshape(-1, 8)
        return _unblock(_dxt_color(b, punch=True), w, h)
    if fmt == FMT_DXT5:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 16)
        px = _dxt_color(b[:, 8:], punch=False)
        px[:, :, 3] = _dxt5_alpha(b[:, :8])
        return _unblock(px, w, h)
    if fmt == FMT_DXT3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 16)
        px = _dxt_color(b[:, 8:], punch=False)
        a = b[:, :8]
        nib = np.stack([a & 15, a >> 4], -1).reshape(-1, 16)
        px[:, :, 3] = nib * 17
        return _unblock(px, w, h)
    a = np.frombuffer(raw, np.uint8)
    n = w * h
    if fmt in (FMT_BGR888, FMT_BGR888_BLUESCREEN):
        p = a[: n * 3].reshape(h, w, 3)[..., ::-1]
        return np.dstack([p, np.full((h, w), 255, np.uint8)])
    if fmt in (FMT_RGB888, FMT_RGB888_BLUESCREEN):
        p = a[: n * 3].reshape(h, w, 3)
        return np.dstack([p, np.full((h, w), 255, np.uint8)])
    if fmt == FMT_RGBA8888:
        return a[: n * 4].reshape(h, w, 4).copy()
    if fmt == FMT_BGRA8888:
        return a[: n * 4].reshape(h, w, 4)[..., [2, 1, 0, 3]].copy()
    if fmt == FMT_BGRX8888:
        p = a[: n * 4].reshape(h, w, 4)[..., [2, 1, 0, 3]].copy()
        p[..., 3] = 255
        return p
    if fmt == FMT_ABGR8888:
        return a[: n * 4].reshape(h, w, 4)[..., [3, 2, 1, 0]].copy()
    if fmt == FMT_ARGB8888:
        return a[: n * 4].reshape(h, w, 4)[..., [1, 2, 3, 0]].copy()
    if fmt == FMT_I8:
        p = a[:n].reshape(h, w)
        return np.dstack([p, p, p, np.full((h, w), 255, np.uint8)])
    if fmt == FMT_IA88:
        p = a[: n * 2].reshape(h, w, 2)
        return np.dstack([p[..., 0], p[..., 0], p[..., 0], p[..., 1]])
    if fmt == FMT_A8:
        p = a[:n].reshape(h, w)
        z = np.full((h, w), 255, np.uint8)
        return np.dstack([z, z, z, p])
    if fmt in (FMT_BGR565, FMT_RGB565):
        c = np.frombuffer(raw[: n * 2], "<u2").reshape(h, w)
        p = _565(c).astype(np.uint8)
        if fmt == FMT_BGR565:
            p = p[..., ::-1]
        return np.dstack([p, np.full((h, w), 255, np.uint8)])
    if fmt in (FMT_BGRA5551, FMT_BGRX5551):
        c = np.frombuffer(raw[: n * 2], "<u2").reshape(h, w).astype(np.uint32)
        b = (c & 31) * 255 // 31
        g = ((c >> 5) & 31) * 255 // 31
        r = ((c >> 10) & 31) * 255 // 31
        al = np.where((c >> 15) & 1, 255, 0) if fmt == FMT_BGRA5551 else np.full_like(c, 255)
        return np.dstack([r, g, b, al]).astype(np.uint8)
    if fmt == FMT_BGRA4444:
        c = np.frombuffer(raw[: n * 2], "<u2").reshape(h, w).astype(np.uint32)
        return (np.dstack([(c >> 8) & 15, (c >> 4) & 15, c & 15, (c >> 12) & 15]) * 17).astype(np.uint8)
    if fmt == FMT_RGBA16161616F:
        f = np.frombuffer(raw[: n * 8], "<f2").reshape(h, w, 4).astype(np.float32)
        return (np.clip(f, 0, 1) * 255).astype(np.uint8)
    raise NotImplementedError(f"VTF format {fmt}")
