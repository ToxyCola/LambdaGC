"""Switchable and animated lights (Source light styles).

vrad bakes a named light (one that can be switched) or a light with a "style" pattern into separate
lightmaps, one per style, stored after the face's normal one. The engine draws the base lightmap and
adds each style's layer on top, scaled by that style's current brightness (a flicker pattern, or on/off
from the light entity's TurnOn/TurnOff inputs).

Layers are stored as the difference they make in the lightmaps' gamma scale, g(base + style) - g(base),
so a light that is fully on gives exactly what one combined lightmap would. Each layer goes in a
"parallel" lightmap page: same layout as the base page (the face's lightmap coordinates work as they
are), but only as tall as the styled faces packed at its top need (the engine scales v to match).

Chunks:
  LSTS  64 x (u8 initial brightness x 64 [0..255 = 0..~4], u8 pattern length, 30 bytes pattern)
  LSTP  u32 count, then per style page: u16 texture, u8 layer, u8 pad, f32 v scale
  LSTF  u32 count, then per (face, layer): u16 face, u8 style, u8 style page (sorted by page, style)
"""
import struct

import numpy as np

# Source's default patterns for styles 0-12 (CWorld::Precache); 'a' = dark, 'm' = normal, 'z' = double
DEFAULT_PATTERNS = {
    0: "m",
    1: "mmnmmommommnonmmonqnmmo",
    2: "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
    3: "mmmmmaaaaammmmmaaaaaabcdefgabcdefg",
    4: "mamamamamama",
    5: "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
    6: "nmonqnmomnmomomno",
    7: "mmmaaaabcdefgmmmmaaaammmaamm",
    8: "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
    9: "aaaaaaaazzzzzzzz",
    10: "mmamammmmammamamaaamammma",
    11: "abcdefghijklmnopqrrqponmlkjihgfedcba",
    12: "mmnnmmnnnmmnn",
}
NUM_STYLES = 64
PATTERN_MAX = 30
LIGHT_CLASSES = ("light", "light_spot", "light_environment")


def pattern_value(ch):
    """one pattern letter -> brightness (m = 1.0)"""
    return max(0, ord(ch.lower()) - ord("a")) / 12.0


def style_table(entities):
    """per style: (initial brightness, pattern). Named lights get 32+ (vrad writes "style" into them);
    one flagged 'initially dark' (spawnflags 1) starts off."""
    on = {s: 1.0 for s in DEFAULT_PATTERNS}
    pats = dict(DEFAULT_PATTERNS)
    for e in entities:
        if e.get("classname", "") not in LIGHT_CLASSES:
            continue
        try:
            s = int(float(e.get("style", "0") or 0))
        except ValueError:
            continue
        if s <= 0 or s >= NUM_STYLES:
            continue
        pat = (e.get("pattern") or "").strip()
        if s >= 32:
            flags = int(float(e.get("spawnflags", "0") or 0))
            on[s] = 0.0 if flags & 1 else 1.0
            pats[s] = pat or "m"
        elif pat:
            pats[s] = pat
    out = []
    for s in range(NUM_STYLES):
        p = (pats.get(s) or "m")[:PATTERN_MAX]
        out.append(struct.pack(">BB", int(round(on.get(s, 1.0) * 64)), len(p)) + p.encode("ascii", "replace").ljust(PATTERN_MAX, b"\0"))
    return b"".join(out)


def rgbexp_lin(raw):
    a = np.frombuffer(raw, np.uint8).reshape(-1, 4)
    exp = a[:, 3].astype(np.int8).astype(np.float32)
    return a[:, :3].astype(np.float32) * np.exp2(exp)[:, None] / 255.0


def gamma_half(lin):
    return np.power(np.clip(lin, 0, None), 1 / 2.2) * 0.5 * 255


def face_layers(lighting, f, bump):
    """[(style, (lh, lw, 3) u8 delta)] for the face's extra styles (slots 1-3)"""
    lw, lh = int(f["lmSize"][0]) + 1, int(f["lmSize"][1]) + 1
    n = lw * lh * 4
    stride = n * (4 if bump else 1)
    o = int(f["lightofs"])
    base = rgbexp_lin(lighting[o : o + n])
    g0 = gamma_half(base)
    out = []
    for k in range(1, 4):
        s = int(f["styles"][k])
        if s == 255:
            break
        raw = lighting[o + k * stride : o + k * stride + n]
        if len(raw) < n or s >= NUM_STYLES:
            break
        d = gamma_half(base + rgbexp_lin(raw)) - g0
        d = np.clip(d + 0.5, 0, 255).astype(np.uint8)
        if d.max() < 2:  # (lights nothing visibly)
            continue
        out.append((s, d.reshape(lh, lw, 3)))
    return out


def style_pages(layers, placement, page_h, page_w, make_tex):
    """layers: {block index: [(style, delta)]}. Returns ([(tex, layer, vscale)], {(block, slot): page index})
    make_tex(rgba, w, h) -> texture index."""
    pages, where = [], {}
    nlayers = max((len(v) for v in layers.values()), default=0)
    for slot in range(nlayers):
        by_page = {}
        for bi, ls in layers.items():
            if slot < len(ls):
                by_page.setdefault(placement[bi][0], []).append(bi)
        for pg, blocks in sorted(by_page.items()):
            # the cells as pack_lightmaps placed them: 1-luxel border, padded to 4x4
            cells = []
            for bi in blocks:
                _, x, y = placement[bi]
                d = layers[bi][slot][1]
                h, w, _ = d.shape
                bw, bh = (w + 2 + 3) & ~3, (h + 2 + 3) & ~3
                cells.append((x - 1, y - 1, bw, bh, np.pad(d, ((1, bh - h - 1), (1, bw - w - 1), (0, 0)), mode="edge")))
            hgt = (max(cy + bh for _, cy, _, bh, _ in cells) + 7) & ~7
            img = np.zeros((hgt, page_w, 3), np.uint8)
            for cx, cy, bw, bh, cell in cells:
                img[cy : cy + bh, cx : cx + bw] = cell
            rgba = np.dstack([img, np.full((hgt, page_w), 255, np.uint8)])
            tex = make_tex(rgba, page_w, hgt)
            idx = len(pages)
            pages.append((tex, slot + 1, page_h[pg] / hgt))
            for bi in blocks:
                where[(bi, slot)] = idx
    return pages, where


def chunks_for(chunk, entities, pages, face_recs):
    chunk(b"LSTS", style_table(entities))
    chunk(b"LSTP", struct.pack(">I", len(pages)) + b"".join(struct.pack(">HBBf", t, l, 0, v) for t, l, v in pages))
    recs = sorted(face_recs, key=lambda r: (r[2], r[1], r[0]))
    chunk(b"LSTF", struct.pack(">I", len(recs)) + b"".join(struct.pack(">HBB", *r) for r in recs))
