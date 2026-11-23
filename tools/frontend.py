"""Front end data from the user's install: fonts, localized strings, titles.txt messages, and the
main menu (labels, chapters with their thumbnails and start maps, background maps).

Fonts: Half-Life 2's scheme asks for Windows fonts (Tahoma, Verdana, Trebuchet MS) plus the game's own
halflife2.ttf for the title. Pass the Windows fonts folder to use them; otherwise the DejaVu Sans that
ships with the game stands in."""
import os
import re
import struct

import numpy as np

from gxtex import encode_rgb565
from sounds import parse_kv

# Chapter start maps (Half-Life 2's chapter configs), in menu order
# (the maps whose worldspawn carries the chapter's "chaptertitle", as the PC's chapter list starts them)
CHAPTERS = [("1", "d1_trainstation_01"), ("2", "d1_trainstation_05"), ("3", "d1_canals_01"),
            ("4", "d1_canals_06"), ("5", "d1_eli_01"), ("6", "d1_town_01"), ("7", "d2_coast_01"),
            ("8", "d2_coast_09"), ("9", "d2_prison_02"), ("9a", "d2_prison_06"), ("10", "d3_c17_02"),
            ("11", "d3_c17_09"), ("12", "d3_citadel_01"), ("13", "d3_breen_01")]

THUMB_W, THUMB_H = 256, 144  # chapter previews (16:9, like the big-picture art)

# string ids the engine uses (menu.bin string table order)
MENU_STRINGS = [
    ("gameui_gamemenu_resumegame", "RESUME GAME"), ("gameui_gamemenu_newgame", "NEW GAME"),
    ("gameui_gamemenu_loadgame", "LOAD GAME"), ("gameui_gamemenu_savegame", "SAVE GAME"),
    ("gameui_gamemenu_options", "OPTIONS"), ("gameui_gamemenu_quit", "QUIT"),
    ("gameui_newgame", "NEW GAME"), ("gameui_startnewgame", "Start New Game"), ("gameui_back", "Back"),
    ("gameui_options", "Options"), ("gameui_closecaptions_checkbox", "Closed captions"),
    ("gameui_reversemouse", "Reverse look"), ("gameui_mousesensitivity", "Look sensitivity"),
    ("gameui_soundeffectvolume", "Volume"), ("gameui_quitconfirmationtitle", "Quit"),
    ("gameui_ok", "OK"), ("gameui_cancel", "Cancel"), ("gameui_gametitle", "HALF-LIFE 2"),
]

FONT_SPECS = [
    # file, size, texture, windows font, windows size
    ("font.bin", 14, (256, 256), "tahoma.ttf", 13),          # closed captions (small, like PC)
    ("font_hud.bin", 24, (512, 256), "trebucbd.ttf", 26),     # HUD messages, titles, hints
    ("font_menu.bin", 22, (512, 256), "verdana.ttf", 21),     # menu items
]
TITLE_FONT = ("font_title.bin", 64, (512, 512))              # halflife2.ttf: the logo and the HUD numbers


def load_tokens(path):
    if not os.path.exists(path):
        return {}
    raw = open(path, "rb").read()
    text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
    out = {}
    for _, body in parse_kv(text):
        if isinstance(body, list):
            for k, v in body:
                if k.lower() == "tokens" and isinstance(v, list):
                    out.update({a.lower(): c for a, c in v if isinstance(c, str)})
    # every "key" "value" line read directly too: the KeyValues parser stops at an escaped quote
    # ("\"A RED LETTER DAY\""), which threw the pairs after it out of step (keys became values, so
    # chapters 3-5 and 7-10 had no titles)
    # Some strings have per-platform versions ("..." [$X360] / [$WIN32]): this is a console, so the
    # Xbox 360 wording wins (controller names instead of keyboard ones), then plain, then [$WIN32]
    rank = {}
    for m in re.finditer(r'^[ \t]*"([^"\r\n]+)"[ \t]+"((?:[^"\\\r\n]|\\.)*)"[ \t]*(\[[^\]\r\n]*\])?', text, re.M):
        cond = (m.group(3) or "").upper()
        r = 2 if "X360" in cond and "!" not in cond else 0 if ("WIN32" in cond or "!$X360" in cond) else 1
        k = m.group(1).lower()
        if r >= rank.get(k, -1):
            rank[k] = r
            out[k] = m.group(2).replace('\\"', '"')
    return out


class Localizer:
    def __init__(self, game_dir):
        self.t = {}
        for f in ("gameui_english.txt", "valve_english.txt", "hl2_english.txt"):
            self.t.update(load_tokens(os.path.join(game_dir, "resource", f)))

    def get(self, token, default=None):
        tok = token.lstrip("#").lower()
        v = self.t.get(tok)
        return clean(v) if v is not None else default

    def resolve(self, s):
        """'#Token' -> localized text; anything else unchanged."""
        if s and s.startswith("#"):
            return self.get(s, s)
        return s


def clean(s):
    s = re.sub(r"<[^>]*>", "", s)
    s = s.replace("\\n", "\n").replace("%s1", "").replace("&", "")
    return s


def latin1(s):
    return s.encode("latin-1", "replace").decode("latin-1")


class Titles:
    """scripts/titles.txt: messages for env_message, each with the $parameters set above it."""

    def __init__(self, path, fs=None):
        self.msgs = {}
        if os.path.exists(path):
            raw = open(path, "rb").read()
        else:  # (the Steam release keeps it in hl2_misc.vpk, not loose)
            raw = fs.read("scripts/titles.txt") if fs is not None else None
        if not raw:
            return
        params = {"position": (-1.0, 0.6), "color": (255, 255, 255), "color2": (255, 255, 255), "fadein": 0.5,
                  "fadeout": 1.0, "holdtime": 3.0, "fxtime": 0.25, "effect": 0}
        lines = raw.decode("latin-1").replace("\r", "").split("\n")
        i = 0
        while i < len(lines):
            ln = lines[i].strip()
            i += 1
            if not ln or ln.startswith("//"):
                continue
            if ln.startswith("$"):
                parts = ln[1:].split()
                key, vals = parts[0].lower(), parts[1:]
                try:
                    if key == "position":
                        params["position"] = (float(vals[0]), float(vals[1]))
                    elif key in ("color", "color2"):
                        params[key] = tuple(int(float(v)) for v in vals[:3])
                    elif key in ("fadein", "fadeout", "holdtime", "fxtime"):
                        params[key] = float(vals[0])
                    elif key == "effect":
                        params["effect"] = int(vals[0])
                except (ValueError, IndexError):
                    pass
                continue
            name = ln
            if i < len(lines) and lines[i].strip() == "{":
                i += 1
                body = []
                while i < len(lines) and lines[i].strip() != "}":
                    body.append(lines[i].strip())
                    i += 1
                i += 1
                self.msgs[name.lower()] = (" ".join(b for b in body if b), dict(params))


def build_font(ttf, px, path, tex=(256, 256), chars=range(32, 256)):
    from PIL import Image, ImageDraw, ImageFont
    from shared_data import i4_tiles
    try:
        font = ImageFont.truetype(ttf, px)
    except Exception:  # noqa: BLE001
        font = ImageFont.load_default()
    W, H = tex
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    asc, desc = font.getmetrics() if hasattr(font, "getmetrics") else (px, 4)
    line = asc + desc
    x = y = 1
    glyphs = []
    for c in range(32, 256):
        ch = bytes([c]).decode("latin-1")
        if c not in chars or 0x7F <= c < 0xA0:
            glyphs.append((0, 0, 0, 0, 0, 0, 0))
            continue
        try:
            l, t, r, b = font.getbbox(ch)
            adv = int(round(font.getlength(ch)))
        except Exception:  # noqa: BLE001
            l, t, r, b, adv = 0, 0, 0, 0, px // 3
        w, h = max(0, r - l), max(0, b - t)
        if x + w + 1 >= W:
            x, y = 1, y + line + 1
        if w and h and y + h < H:
            d.text((x - l, y - t), ch, font=font, fill=255)
            glyphs.append((x, y, w, h, l, t, adv))
        else:
            glyphs.append((0, 0, 0, 0, 0, 0, adv))
        x += w + 2
    a = np.array(img)
    # glyph table uses 16-bit positions so atlases can be wider than 256
    hdr = struct.pack(">4sHHHH", b"HFN2", W, H, line, len(glyphs))
    tab = b"".join(struct.pack(">HHBBbbBx", gx, gy, min(gw, 255), min(gh, 255), max(-128, min(127, gl)),
                               max(-128, min(127, gt)), min(adv, 255)) for gx, gy, gw, gh, gl, gt, adv in glyphs)
    body = hdr + tab
    body += b"\0" * (-len(body) % 32)
    with open(path, "wb") as f:
        f.write(body + i4_tiles(a))


def build_fonts(game_dir, out_dir, windows_fonts=None):
    dejavu = os.path.join(game_dir, "resource", "dejavusans.ttf")
    for fname, px, tex, winfont, winpx in FONT_SPECS:
        ttf, size = dejavu, px
        if windows_fonts and os.path.exists(os.path.join(windows_fonts, winfont)):
            ttf, size = os.path.join(windows_fonts, winfont), winpx
        build_font(ttf, size, os.path.join(out_dir, fname), tex)
    title = os.path.join(game_dir, "resource", "halflife2.ttf")
    # only what the logo and HUD need, so the big glyphs fit the atlas
    need = set(range(ord("A"), ord("Z") + 1)) | set(range(ord("0"), ord("9") + 1)) | set(map(ord, " '-%/:"))
    build_font(title if os.path.exists(title) else dejavu, TITLE_FONT[1], os.path.join(out_dir, TITLE_FONT[0]),
               TITLE_FONT[2], chars=need)
    build_icon_font(game_dir, out_dir)


ICON_FONT = ("font_icons.bin", 64, (512, 512))  # halflife2.ttf's icon glyphs (WeaponIcons, 64 tall on 480 lines)


def build_icon_font(game_dir, out_dir):
    """The HUD's icons are glyphs of halflife2.ttf: weapons, their ammo, items (the weapon scripts' and
    hud_textures.txt's "character"s), and the squad members (HudNumbers 'C', medics 'M')."""
    title = os.path.join(game_dir, "resource", "halflife2.ttf")
    if not os.path.exists(title):
        return
    chars = set(map(ord, "CM"))
    sdir = os.path.join(game_dir, "scripts")
    for f in os.listdir(sdir) if os.path.isdir(sdir) else []:
        if f.startswith("weapon_") or f == "hud_textures.txt":
            t = open(os.path.join(sdir, f), "rb").read().decode("latin-1")
            for m in re.finditer(r'"font"\s*"(?:WeaponIcons\w*|HudNumbers\w*|Crosshairs)"\s*"character"\s*"(.)"', t):
                chars.add(ord(m.group(1)))
    build_font(title, ICON_FONT[1], os.path.join(out_dir, ICON_FONT[0]), ICON_FONT[2], chars=chars)
    # the quick info brackets around the crosshair: hl2crosshairs.ttf ("HL2cross", clientscheme QuickInfo)
    cross = os.path.join(game_dir, "resource", "hl2crosshairs.ttf")
    if os.path.exists(cross):
        build_font(cross, 64, os.path.join(out_dir, "font_cross.bin"), (256, 128), chars=set(map(ord, "()[]{}")))


def build_menu(fs, game_dir, out_dir, loc, log=print, titles=None):
    """menu.bin: strings, chapters (title, start map, background map, 128x64 RGB565 thumbnail)."""
    from vtf import VTF
    bgs = {}
    p = os.path.join(game_dir, "scripts", "chapterbackgrounds.txt")
    if os.path.exists(p):
        for _, body in parse_kv(open(p, "rb").read().decode("latin-1")):
            if isinstance(body, list):
                bgs = {k: v for k, v in body if isinstance(v, str)}
    strings = [latin1(loc.get(tok, default)) for tok, default in MENU_STRINGS]
    # (a controller: the look is on the C-stick, not a mouse)
    strings = [re.sub(r"\bmouse\b", "stick", re.sub(r"\bMouse\b", "Stick", re.sub(r"\bMOUSE\b", "STICK", x)))
               for x in strings]
    gi = os.path.join(game_dir, "gameinfo.txt")
    if os.path.exists(gi):
        m = re.search(r'\btitle\s+"([^"]+)"', open(gi, "rb").read().decode("latin-1"))
        if m:
            strings[-1] = latin1(m.group(1))  # drawn in the logo font: ' is the logo's 2
    chapters = []
    for num, start in CHAPTERS:
        title = loc.get(f"hl2_chapter{num}_title")
        if not title and titles is not None:
            t = titles.msgs.get(f"chapter{num}_title")
            if t:
                title = loc.resolve(t[0])
        title = title or f"Chapter {num}"
        thumb = np.zeros((THUMB_H, THUMB_W, 4), np.uint8)
        for path in (f"materials/gamepadui/chapter{num}.vtf", f"materials/vgui/chapters/chapter{num}.vtf"):
            try:
                data = fs.read(path)
            except Exception:  # noqa: BLE001
                data = None
            if data:
                try:
                    from PIL import Image
                    rgba = VTF(data).rgba(0)
                    if rgba.shape[1] == 256 and rgba.shape[0] == 128:
                        rgba = rgba[:86, :152]  # the PC thumbnails only fill 152x86 of their texture
                    im = Image.fromarray(rgba).convert("RGBA").resize((THUMB_W, THUMB_H), Image.LANCZOS)
                    thumb = np.array(im)
                    break
                except Exception as ex:  # noqa: BLE001
                    log(f"  chapter {num} thumbnail: {ex}")
        chapters.append((latin1(title.replace("\n", " ")), start, bgs.get(num, "background01"),
                         encode_rgb565(thumb)))
    blob = bytearray()
    offs = []

    def s(x):
        offs.append(len(blob))
        blob.extend(x.encode("latin-1", "replace") + b"\0")
        return offs[-1]

    str_offs = [s(x) for x in strings]
    ch_recs = []
    for title, start, bg, _ in chapters:
        ch_recs.append((s(title), s(start), s(bg)))
    hdr = struct.pack(">4sHH", b"HMNU", len(str_offs), len(chapters))
    table = b"".join(struct.pack(">I", o) for o in str_offs) + b"".join(struct.pack(">III", *r) for r in ch_recs)
    head = hdr + table
    strings_off = len(head)
    head += bytes(blob)
    head += b"\0" * (-len(head) % 32)
    thumbs_off = len(head)
    data = head + b"".join(c[3] for c in chapters)
    # 32-byte prefix (keeps everything after it 32-byte aligned): where the strings and thumbnails start,
    # both relative to the end of the prefix
    with open(os.path.join(out_dir, "menu.bin"), "wb") as f:
        f.write(struct.pack(">II24x", strings_off, thumbs_off) + data)
    log(f"  menu: {len(strings)} strings, {len(chapters)} chapters")


def text_keys(e, loc, titles):
    """Extra keys for text entities: resolved message, position, colour and timing."""
    cls = e.get("classname", "")
    k = {a.lower(): v for a, v in e.items() if isinstance(v, str)}
    out = []
    if cls == "env_message":
        msg = k.get("message", "")
        t = titles.msgs.get(msg.lower())
        if t:
            text, pr = t
            text = loc.resolve(text) if text.startswith("#") else text
            out += [("_text", latin1(clean(text))), ("_pos", "%g %g" % pr["position"]),
                    ("_color", "%d %d %d" % pr["color"]),
                    ("_time", "%g %g %g" % (pr["fadein"], pr["holdtime"], pr["fadeout"])),
                    ("_fx", "%d %g %d %d %d" % (pr["effect"], pr["fxtime"], *pr["color2"]))]
        elif msg:
            out.append(("_text", latin1(loc.resolve(msg))))
    elif cls == "env_credits":
        cr = getattr(titles, "credits", None)
        if cr:
            out += [("_logo", latin1(cr["logo"])), ("_cparams", cr["params"]), ("_names", latin1(cr["names"]))]
    elif cls == "game_text":
        msg = k.get("message", "")
        if msg:
            out.append(("_text", latin1(clean(loc.resolve(msg)))))
    elif cls == "env_hudhint":
        msg = k.get("message", "")
        if msg:
            # CHudHintDisplay looks the message up whether or not it has the '#' (HL2's maps leave it off)
            text = loc.get(msg) or loc.resolve(msg)
            out.append(("_text", latin1(clean(controller_bindings(text)))))
    return out


# Source's hint strings name keys as %+use%, %impulse 100%...: here, the GameCube controller's buttons as
# main.c maps them (README / main.c's controls comment)
GC_BINDINGS = {
    "+use": "X", "+attack": "R", "+attack2": "L", "+jump": "A", "+speed": "B", "+duck": "Z",
    "+reload": "Y", "impulse 100": "D-PAD UP", "+forward": "CONTROL STICK", "+back": "CONTROL STICK",
    "+moveleft": "CONTROL STICK", "+moveright": "CONTROL STICK", "xmove": "CONTROL STICK",
    "xlook": "C-STICK", "+zoom": "L", "phys_swap": "D-PAD DOWN", "lastinv": "D-PAD DOWN",
    "invnext": "D-PAD RIGHT", "invprev": "D-PAD LEFT", "impulse 50": "D-PAD DOWN",
    "slot1": "D-PAD", "slot2": "D-PAD", "slot3": "D-PAD", "slot4": "D-PAD", "slot5": "D-PAD", "slot6": "D-PAD",
}


def controller_bindings(text):
    def sub(m):
        key = m.group(1).strip().lower()
        return "[" + GC_BINDINGS.get(key, key.lstrip("+").upper()) + "]"
    return re.sub(r"%([^%\n]+)%", sub, text or "")


def load_credits(game_dir, fs=None):
    """scripts/credits.txt: the logo, the intro's names and their timing, for env_credits (CHudCredits)."""
    path = os.path.join(game_dir, "scripts", "credits.txt")
    raw = open(path, "rb").read() if os.path.exists(path) else (fs.read("scripts/credits.txt") if fs else None)
    if not raw:
        return None
    text = raw.decode("latin-1")
    def block(name):
        i = text.find('"%s"' % name)
        if i < 0:
            return ""
        a = text.find("{", i)
        b = text.find("}", a)
        return text[a + 1:b]
    pr = dict((m.group(1).lower(), m.group(2)) for m in
              re.finditer(r'^[ \t]*"([^"]+)"[ \t]+"([^"]*)"', block("CreditsParams"), re.M))
    names = []
    for m in re.finditer(r'^[ \t]*"([^"]*)"[ \t]+"([^"]*)"', block("IntroCreditsNames"), re.M):
        if m.group(2).lower() == "creditstext" and m.group(1).strip():
            names.append(m.group(1).strip())
    f = lambda k, d: float(pr.get(k, d))
    params = "%g %g %g %g %g %g %g %g" % (f("fadeintime", 1), f("fadeouttime", 1), f("fadeholdtime", 1),
                                         f("nextfadetime", 1), f("pausebetweenwaves", 1), f("logotime", 1),
                                         f("posx", 96), f("posy", 360))
    return {"logo": pr.get("logo", "HALF-LIFE'"), "params": params, "names": "\n".join(names)}
