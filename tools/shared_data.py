"""Data shared by every map: sound scripts, captions, the compiled scene image, the voice bank and the
caption font. Built once per conversion run; the bank and font are written next to the maps folder."""
import os
import struct

import numpy as np

from scenes import SceneImage
from soundbank import SoundBank
from sounds import Captions, SoundScripts

FONT_PX = 18
FONT_TEX = 256
FIRST_GLYPH, LAST_GLYPH = 32, 255


def i4_tiles(img):
    """8-bit grey [H, W] -> GX I4 (8x8 tiles, 4 bits per pixel, high nibble first)."""
    h, w = img.shape
    v = (img.astype(np.uint16) >> 4).astype(np.uint8)
    out = bytearray()
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            t = v[ty:ty + 8, tx:tx + 8].reshape(-1)
            out += bytes(((t[0::2] << 4) | t[1::2]).astype(np.uint8))
    return bytes(out)


def build_font(game_dir, path):
    """Caption font atlas (from the install's resource/dejavusans.ttf when present)."""
    from PIL import Image, ImageDraw, ImageFont
    ttf = os.path.join(game_dir, "resource", "dejavusans.ttf")
    try:
        font = ImageFont.truetype(ttf, FONT_PX)
    except Exception:  # noqa: BLE001
        font = ImageFont.load_default()
    img = Image.new("L", (FONT_TEX, FONT_TEX), 0)
    d = ImageDraw.Draw(img)
    asc, desc = font.getmetrics() if hasattr(font, "getmetrics") else (FONT_PX, 4)
    line = asc + desc
    x = y = 1
    glyphs = []
    for c in range(FIRST_GLYPH, LAST_GLYPH + 1):
        ch = bytes([c]).decode("latin-1")
        try:
            l, t, r, b = font.getbbox(ch)
            adv = int(round(font.getlength(ch)))
        except Exception:  # noqa: BLE001
            l, t, r, b, adv = 0, 0, 0, 0, FONT_PX // 3
        w, h = max(0, r - l), max(0, b - t)
        if c < 0x7F or c >= 0xA0:
            if x + w + 1 >= FONT_TEX:
                x, y = 1, y + line + 1
            if w and h and y + h < FONT_TEX:
                d.text((x - l, y - t), ch, font=font, fill=255)
            glyphs.append((x, y, w, h, l, t, adv))
            x += w + 2
        else:
            glyphs.append((0, 0, 0, 0, 0, 0, 0))
    a = np.array(img)
    hdr = struct.pack(">4sHHHH", b"HFNT", FONT_TEX, FONT_TEX, line, len(glyphs))
    tab = b"".join(struct.pack(">BBBBbbBx", gx, gy, min(gw, 255), min(gh, 255), max(-128, min(127, gl)),
                               max(-128, min(127, gt)), min(adv, 255)) for gx, gy, gw, gh, gl, gt, adv in glyphs)
    body = hdr + tab
    body += b"\0" * (-len(body) % 32)
    with open(path, "wb") as f:
        f.write(body + i4_tiles(a))


class Shared:
    def __init__(self, game_dir, hl2gc_dir, fs, windows_fonts=None):
        os.makedirs(hl2gc_dir, exist_ok=True)
        self.fs = fs
        self.scripts = SoundScripts(fs)
        self.captions = Captions(os.path.join(game_dir, "resource", "closecaption_english.txt"))
        img = fs.read("scenes/scenes.image")
        self.scenes = SceneImage(img)
        self.bank = SoundBank(hl2gc_dir)
        import frontend
        self.loc = frontend.Localizer(game_dir)
        self.titles = frontend.Titles(os.path.join(game_dir, "scripts", "titles.txt"), fs)
        self.titles.credits = frontend.load_credits(game_dir, fs)
        if not os.path.exists(os.path.join(hl2gc_dir, "font_title.bin")) or windows_fonts:
            frontend.build_fonts(game_dir, hl2gc_dir, windows_fonts)
        if not os.path.exists(os.path.join(hl2gc_dir, "font_icons.bin")) or not os.path.exists(os.path.join(hl2gc_dir, "font_cross.bin")):
            frontend.build_icon_font(game_dir, hl2gc_dir)
        if not os.path.exists(os.path.join(hl2gc_dir, "menu.bin")):
            frontend.build_menu(fs, game_dir, hl2gc_dir, self.loc, titles=self.titles)
