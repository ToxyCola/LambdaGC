"""
The disc's banner (opening.bnr, BNR1): the 96x32 picture and the title text the GameCube menu, Dolphin's
game list and Swiss show for the disc. Made from any image (cropped to the banner's 3:1 shape around its
centre, or around --focus-y), written to <disc>/files/opening.bnr where make_iso.sh puts it in the disc root.

usage: python tools/banner.py header.jpg disc/files/opening.bnr [--focus-y 0.2]
"""
import argparse
import struct

import numpy as np
from PIL import Image

from gxtex import encode_rgb5a3

W, H = 96, 32
SHORT_NAME = "Half-Life 2"
SHORT_MAKER = "hl2gc"
LONG_NAME = "Half-Life 2"
LONG_MAKER = "hl2gc (Source engine port)"
DESCRIPTION = "Half-Life 2 on GameCube, converted from your own copy of the game."


def make_banner(src, out, focus_y=0.2):
    im = Image.open(src).convert("RGB")
    w, h = im.size
    # crop to 3:1 (the banner's shape), keeping the whole width if the picture is taller than that
    if w / h < W / H:
        ch = int(round(w * H / W))
        top = int(round((h - ch) * min(1.0, max(0.0, focus_y))))
        im = im.crop((0, top, w, top + ch))
    else:
        cw = int(round(h * W / H))
        left = (w - cw) // 2
        im = im.crop((left, 0, left + cw, h))
    im = im.resize((W, H), Image.LANCZOS)
    rgba = np.dstack([np.array(im), np.full((H, W), 255, np.uint8)])
    pixels = encode_rgb5a3(rgba)
    assert len(pixels) == W * H * 2

    def field(s, n):
        b = s.encode("latin-1", "replace")[:n - 1]
        return b + b"\0" * (n - len(b))

    data = b"BNR1" + b"\0" * 0x1C + pixels
    data += field(SHORT_NAME, 0x20) + field(SHORT_MAKER, 0x20) + field(LONG_NAME, 0x40)
    data += field(LONG_MAKER, 0x40) + field(DESCRIPTION, 0x80)
    assert len(data) == 0x1960
    with open(out, "wb") as f:
        f.write(data)
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--focus-y", type=float, default=0.2, help="where to crop vertically, 0 top .. 1 bottom")
    a = ap.parse_args()
    make_banner(a.image, a.out, a.focus_y)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
