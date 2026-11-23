"""Soundscript (game_sounds_manifest) resolution and close-caption lookup."""
import os
import re
import zlib

_TOK = re.compile(r'"([^"]*)"|(\{)|(\})|([^\s{}"]+)')


def parse_kv(text):
    """Minimal KeyValues parser. Returns a list of (key, value) where value is a str or a nested list."""
    text = re.sub(r"//[^\n]*", "", text)
    toks = []
    for m in _TOK.finditer(text):
        if m.group(1) is not None:
            toks.append(("s", m.group(1)))
        elif m.group(2):
            toks.append(("{", None))
        elif m.group(3):
            toks.append(("}", None))
        else:
            w = m.group(4)
            if w.startswith("[") and w.endswith("]"):
                continue  # conditionals like [$X360]
            toks.append(("s", w))
    pos = 0

    def block():
        nonlocal pos
        out = []
        while pos < len(toks):
            t, v = toks[pos]
            if t == "}":
                pos += 1
                return out
            if t != "s":
                pos += 1
                continue
            key = v
            pos += 1
            if pos >= len(toks):
                break
            t2, v2 = toks[pos]
            if t2 == "{":
                pos += 1
                out.append((key, block()))
            elif t2 == "s":
                pos += 1
                out.append((key, v2))
        return out

    return block()


_SNDLVL = {"SNDLVL_NONE": 0, "SNDLVL_IDLE": 60, "SNDLVL_STATIC": 66, "SNDLVL_NORM": 75, "SNDLVL_TALKING": 80,
           "SNDLVL_GUNFIRE": 140}


def _soundlevel(v):
    v = v.strip().upper()
    if v in _SNDLVL:
        return _SNDLVL[v]
    m = re.match(r"SNDLVL_(\d+)DB", v)
    if m:
        return int(m.group(1))
    try:
        return int(float(v.split(",")[0]))
    except ValueError:
        return 75


def _volume(v):
    v = v.strip().upper()
    if v == "VOL_NORM":
        return 1.0
    try:
        parts = [float(x) for x in v.split(",")]
        return sum(parts) / len(parts)
    except ValueError:
        return 1.0


class SoundScripts:
    def __init__(self, fs):
        self.fs = fs
        self.entries = {}
        man = self._read("scripts/game_sounds_manifest.txt")
        files = []
        if man:
            for _, body in parse_kv(man):
                if isinstance(body, list):
                    for k, v in body:
                        if k.lower() in ("precache_file", "preload_file") and isinstance(v, str):
                            files.append(v)
        for f in files:
            t = self._read(f)
            if not t:
                continue
            for name, body in parse_kv(t):
                if not isinstance(body, list):
                    continue
                waves = []
                vol = 1.0
                lvl = 75
                for k, v in body:
                    kl = k.lower()
                    if kl == "wave" and isinstance(v, str):
                        waves.append(v)
                    elif kl == "rndwave" and isinstance(v, list):
                        waves += [w for kk, w in v if kk.lower() == "wave" and isinstance(w, str)]
                    elif kl == "volume" and isinstance(v, str):
                        vol = _volume(v)
                    elif kl == "soundlevel" and isinstance(v, str):
                        lvl = _soundlevel(v)
                    elif kl == "attenuation" and isinstance(v, str):
                        try:  # old-style attenuation -> soundlevel (ATTN_TO_SNDLVL)
                            a = float(v.split(",")[0])
                            lvl = 0 if a <= 0 else int(50 + 20 / a)
                        except ValueError:
                            pass
                self.entries[name.lower()] = (waves, vol, lvl)

    def _read(self, path):
        try:
            d = self.fs.read(path)
        except Exception:
            return None
        if d is None:
            return None
        return d.decode("latin-1")

    @staticmethod
    def clean_wave(w):
        w = w.lstrip("*#@<>^)}$!?&~`+%").replace("\\", "/")
        return "sound/" + w.lower()

    def meta(self, name):
        """(volume 0..1, soundlevel dB, 0 = heard everywhere) of a soundscript; raw wav paths get the defaults."""
        e = self.entries.get(name.lower())
        return (e[1], e[2]) if e else (1.0, 75)

    def resolve(self, name):
        """Soundscript name or raw wav path -> list of 'sound/...' paths."""
        n = name.lower()
        if n in self.entries:
            return [self.clean_wave(w) for w in self.entries[n][0]]
        if n.endswith((".wav", ".mp3")):
            return [self.clean_wave(n)]
        return []


class Captions:
    """closecaption_<lang>.txt (UTF-16). Tokens map to text; formatting tags like <clr:...> are stripped."""

    def __init__(self, path):
        self.tokens = {}
        if not os.path.exists(path):
            return
        raw = open(path, "rb").read()
        text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
        tree = parse_kv(text)
        for _, body in tree:
            if not isinstance(body, list):
                continue
            for k, v in body:
                if k.lower() == "tokens" and isinstance(v, list):
                    for tk, tv in v:
                        if isinstance(tv, str):
                            self.tokens[tk.lower()] = tv

    @staticmethod
    def strip(text):
        speaker = None
        m = re.match(r"\s*((?:<[^>]*>\s*)*)([^:<]{1,24}):\s*", text)
        out = re.sub(r"<[^>]*>", "", text)
        out = re.sub(r"\s+", " ", out).strip()
        return out

    def get(self, token):
        t = self.tokens.get(token.lower())
        return self.strip(t) if t else None


class Soundscapes:
    """scripts/soundscapes*.txt (C_SoundscapeSystem): each soundscape flattened to its dsp room and a list of
    elements: (loop 0 / random 1, position (0-7, 254 random, 255 none), soundlevel dB (0 = everywhere),
    volume min, max, time min, max, [wave paths])."""

    def __init__(self, fs):
        self.raw = {}
        man = fs.read("scripts/soundscapes_manifest.txt")
        files = []
        if man:
            for _, body in parse_kv(man.decode("latin-1")):
                if isinstance(body, list):
                    files += [v for k, v in body if k.lower() == "file" and isinstance(v, str)]
        for f in files:
            d = fs.read(f)
            if not d:
                continue
            for name, body in parse_kv(d.decode("latin-1")):
                if isinstance(body, list):
                    self.raw[name.lower()] = body

    @staticmethod
    def _range(v, default):
        try:
            p = [float(x) for x in str(v).split(",")]
            return (p[0], p[-1])
        except ValueError:
            return (default, default)

    def flatten(self, name, vol=1.0, pos_override=None, amb_override=None, depth=0):
        """-> (dsp or None, [elements])"""
        body = self.raw.get(name.lower())
        if body is None or depth > 4:
            return None, []
        dsp, out = None, []
        for k, v in body:
            kl = k.lower()
            if kl == "dsp" and isinstance(v, str):
                try:
                    dsp = int(float(v))
                except ValueError:
                    pass
            elif kl in ("playlooping", "playrandom") and isinstance(v, list):
                kv = {a.lower(): b for a, b in v if isinstance(b, str)}
                waves = [b for a, b in v if a.lower() == "wave" and isinstance(b, str)]
                for a, b in v:
                    if a.lower() == "rndwave" and isinstance(b, list):
                        waves += [w for x, w in b if x.lower() == "wave" and isinstance(w, str)]
                if not waves:
                    continue
                p = kv.get("position")
                pos = 255 if p is None else (254 if p.lower() == "random" else max(0, min(7, int(float(p)))))
                if pos_override is not None:
                    pos = pos_override
                elif pos == 255 and amb_override is not None:
                    pos = amb_override
                lvl = _soundlevel(kv["soundlevel"]) if "soundlevel" in kv else (0 if pos == 255 else 75)
                vmin, vmax = self._range(kv.get("volume", "1"), 1.0)
                tmin, tmax = self._range(kv.get("time", "0"), 0.0)
                out.append((0 if kl == "playlooping" else 1, pos, max(0, min(255, lvl)), vmin * vol, vmax * vol,
                            tmin, tmax, waves))
            elif kl == "playsoundscape" and isinstance(v, list):
                kv = {a.lower(): b for a, b in v if isinstance(b, str)}
                sub = kv.get("name")
                if not sub:
                    continue
                sv = self._range(kv.get("volume", "1"), 1.0)[0]
                po = pos_override
                if "positionoverride" in kv:
                    po = max(0, min(7, int(float(kv["positionoverride"]))))
                ao = amb_override
                if "ambientpositionoverride" in kv:
                    ao = max(0, min(7, int(float(kv["ambientpositionoverride"]))))
                d2, els = self.flatten(sub, vol * sv, po, ao, depth + 1)
                out += els
                if dsp is None:
                    dsp = d2 if depth else None
        return dsp, out
