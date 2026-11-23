"""The OG Xbox Half-Life 2 menu recreation (hl2OGXboxMenu.swf, the user's own file): its sounds and button
icon for the engine's Xbox-style main menu (menu.c). The layout, timings and colours are taken from the
SWF's timeline and are hard-coded in menu.c; this extracts the assets.

xmenu.bin: "HXMN", u16 numSounds, u16 iconW, u16 iconH, u16 pad, then numSounds PackSound records
(offset/size in vo.bin, like the maps' SNDS), then the icon as RGBA8 (GX_TF_RGBA8 tiles)."""
import lzma
import os
import struct
import subprocess
import tempfile
import zlib

import numpy as np

from soundbank import RATE, ima_encode

# DefineSound ids in the SWF -> menu.c's XS_* order
SOUNDS = [(10, "music", True), (7, "open", False), (8, "settle", False), (18, "rollover", False),
          (19, "press", False), (32, "submenu", False), (67, "back", False)]
ICON_SHAPE = None  # the SWF's prompt icon is a PC mouse; the GameCube menu draws its A button instead


class _Bits:
    def __init__(self, b, o):
        self.b, self.p = b, o * 8

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def s(self, n):
        v = self.u(n)
        return v - (1 << n) if n and v & (1 << (n - 1)) else v

    @property
    def o(self):
        self.p = (self.p + 7) & ~7
        return self.p >> 3


def _load(path):
    d = open(path, "rb").read()
    if d[:3] == b"CWS":
        d = d[:8] + zlib.decompress(d[8:])
    elif d[:3] == b"ZWS":
        d = d[:8] + lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(
            d[12:17] + struct.pack("<Q", 0xFFFFFFFFFFFFFFFF) + d[17:])
    return d


def _tags(d, o):
    while o < len(d):
        h = struct.unpack_from("<H", d, o)[0]
        o += 2
        code, ln = h >> 6, h & 63
        if ln == 63:
            ln = struct.unpack_from("<I", d, o)[0]
            o += 4
        yield code, d[o:o + ln]
        o += ln
        if code == 0:
            break


def _start(d):
    nb = d[8] >> 3
    return 8 + (5 + 4 * nb + 7) // 8 + 4


def _mp3_to_pcm(mp3, rate):
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "in.mp3")
        open(src, "wb").write(mp3)
        out = subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
                             capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype="<i2").astype(np.int16)


def _shape(b, code):
    """DefineShape2/3/4 -> (bounds, fills[(r,g,b,a)], segments[(x0,y0,x1,y1,fill0,fill1)]) in pixels."""
    r = _Bits(b, 2)
    n = r.u(5)
    bounds = [r.s(n) / 20 for _ in range(4)]
    o = r.o
    if code == 83:
        r = _Bits(b, o)
        n = r.u(5)
        [r.s(n) for _ in range(4)]
        o = r.o + 1
    rgba = code >= 32

    def styles(o):
        nf = b[o]
        o += 1
        if nf == 0xFF:
            nf = struct.unpack_from("<H", b, o)[0]
            o += 2
        fills = []
        for _ in range(nf):
            t = b[o]
            o += 1
            if t == 0:
                c = tuple(b[o:o + (4 if rgba else 3)]) + (() if rgba else (255,))
                o += 4 if rgba else 3
                fills.append(c)
            else:
                raise ValueError("only solid fills are supported")
        nl = b[o]
        o += 1
        if nl == 0xFF:
            nl = struct.unpack_from("<H", b, o)[0]
            o += 2
        for _ in range(nl):
            if code == 83:
                o += 2
                fl = struct.unpack_from("<H", b, o)[0]
                o += 2
                if fl & 0x30 == 0x20:
                    o += 2
                o += 4 if not (fl & 0x08) else 0
                if fl & 0x08:
                    raise ValueError("line fill")
            else:
                o += 2 + (4 if rgba else 3)
        return fills, o

    fills, o = styles(o)
    r = _Bits(b, o)
    nfb, nlb = r.u(4), r.u(4)
    x = y = 0
    f0 = f1 = 0
    base = 0
    allfills = list(fills)
    segs = []
    while True:
        if r.u(1) == 0:
            flags = r.u(5)
            if flags == 0:
                break
            if flags & 1:
                n = r.u(5)
                x, y = r.s(n), r.s(n)
            if flags & 2:
                f0 = r.u(nfb)
                f0 = f0 + base if f0 else 0
            if flags & 4:
                f1 = r.u(nfb)
                f1 = f1 + base if f1 else 0
            if flags & 8:
                r.u(nlb)
            if flags & 16:
                o = r.o
                nf, o = styles(o)
                base = len(allfills)
                allfills += nf
                r = _Bits(b, o)
                nfb, nlb = r.u(4), r.u(4)
        else:
            if r.u(1):
                n = r.u(4) + 2
                if r.u(1):
                    dx, dy = r.s(n), r.s(n)
                elif r.u(1):
                    dx, dy = 0, r.s(n)
                else:
                    dx, dy = r.s(n), 0
                segs.append((x, y, x + dx, y + dy, f0, f1))
                x, y = x + dx, y + dy
            else:
                n = r.u(4) + 2
                cx, cy = x + r.s(n), y + r.s(n)
                ax, ay = cx + r.s(n), cy + r.s(n)
                px, py = x, y
                for k in range(1, 9):
                    t = k / 8
                    qx = (1 - t) ** 2 * x + 2 * (1 - t) * t * cx + t * t * ax
                    qy = (1 - t) ** 2 * y + 2 * (1 - t) * t * cy + t * t * ay
                    segs.append((px, py, qx, qy, f0, f1))
                    px, py = qx, qy
                x, y = ax, ay
    return bounds, allfills, [(a / 20, b_ / 20, c / 20, d / 20, e, f) for a, b_, c, d, e, f in segs]


def _raster(bounds, fills, segs, scale, ss=4):
    x0, x1, y0, y1 = bounds
    W, H = int(np.ceil((x1 - x0) * scale)), int(np.ceil((y1 - y0) * scale))
    img = np.zeros((H, W, 4), np.float32)
    for fi in range(1, len(fills) + 1):
        es = [s for s in segs if s[4] == fi or s[5] == fi]
        if not es:
            continue
        cov = np.zeros((H * ss, W * ss), np.float32)
        ys = (np.arange(H * ss) + 0.5) / (scale * ss) + y0
        xs = (np.arange(W * ss) + 0.5) / (scale * ss) + x0
        for row, yy in enumerate(ys):
            cross = []
            for ax, ay, bx, by, _, _ in es:
                if (ay <= yy) != (by <= yy):
                    cross.append(ax + (yy - ay) * (bx - ax) / (by - ay))
            cross.sort()
            for k in range(0, len(cross) - 1, 2):
                cov[row, (xs >= cross[k]) & (xs < cross[k + 1])] = 1
        c = cov.reshape(H, ss, W, ss).mean((1, 3))[..., None]
        col = np.array(fills[fi - 1], np.float32) / 255
        a = c * col[3]
        img[..., :3] = img[..., :3] * (1 - a) + col[:3] * a
        img[..., 3:] = img[..., 3:] * (1 - a) + a
    return (np.clip(img, 0, 1) * 255).astype(np.uint8)


def _rgba8_tiles(img):
    """RGBA image (H, W multiples of 4) -> GX_TF_RGBA8 tiles (AR block then GB block per 4x4)."""
    H, W = img.shape[:2]
    out = bytearray()
    for ty in range(0, H, 4):
        for tx in range(0, W, 4):
            t = img[ty:ty + 4, tx:tx + 4]
            out += bytes(np.stack([t[..., 3], t[..., 0]], -1).reshape(-1))
            out += bytes(np.stack([t[..., 1], t[..., 2]], -1).reshape(-1))
    return bytes(out)


def build_xbox_menu(swf_path, bank, out_dir, log=print):
    d = _load(swf_path)
    sounds, shapes = {}, {}
    for code, b in _tags(d, _start(d)):
        if code == 14:
            sid = struct.unpack_from("<H", b, 0)[0]
            fl = b[2]
            if fl >> 4 == 2:
                sounds[sid] = b[7:]
        elif code in (22, 32, 83):
            shapes[struct.unpack_from("<H", b, 0)[0]] = (code, b)
    recs = []
    for sid, name, loop in SOUNDS:
        if sid not in sounds:
            recs.append(struct.pack(">III", 0, 0, 0))
            continue
        mp3 = sounds[sid][2:]  # MP3SoundData: SeekSamples (s16), then frames
        pcm = _mp3_to_pcm(mp3, RATE)
        off, size, n = bank.add_pcm(f"xboxmenu/{name}", pcm, 0 if loop else None)
        # volume 255, soundlevel 0 (heard everywhere: it's the menu)
        recs.append(struct.pack(">III", off, (255 << 24) | (size & 0xFFFFFF), n & 0xFFFFFF))
    iw = ih = 0
    icon = b""
    if ICON_SHAPE in shapes:
        bounds, fills, segs = _shape(shapes[ICON_SHAPE][1], shapes[ICON_SHAPE][0])
        scale = 32.0 / (bounds[3] - bounds[2])  # 32 pixels tall
        img = _raster(bounds, fills, segs, scale)
        h, w = img.shape[:2]
        pad = np.zeros(((h + 3) // 4 * 4, (w + 3) // 4 * 4, 4), np.uint8)
        pad[:h, :w] = img
        ih, iw = pad.shape[:2]
        icon = _rgba8_tiles(pad)
    if not icon:
        # the GameCube's A and B buttons for the legend bar (tools/assets, the user's SVGs rasterized at
        # 64x64 with rsvg-convert): A on top of B, one 64x128 RGBA8 texture
        here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
        try:
            from PIL import Image
            a = np.asarray(Image.open(os.path.join(here, "gcn_a.png")).convert("RGBA").resize((64, 64)))
            bb = np.asarray(Image.open(os.path.join(here, "gcn_b.png")).convert("RGBA").resize((64, 64)))
            img = np.concatenate([a, bb], 0).astype(np.uint8)
            ih, iw = img.shape[:2]
            icon = _rgba8_tiles(img)
        except Exception as ex:  # noqa: BLE001
            log(f"  xbox menu: button icons: {ex}")
    hdr = struct.pack(">4sHHHH", b"HXMN", len(recs), iw, ih, 0) + b"".join(recs)
    hdr += b"\0" * (-len(hdr) % 32)
    open(os.path.join(out_dir, "xmenu.bin"), "wb").write(hdr + icon)
    log(f"  xbox menu: {len(sounds)} sounds, icon {iw}x{ih}")
