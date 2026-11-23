"""Material effects: Source's material proxies and the unlit / two-texture / self-illum shaders, compiled for
the engine (render.c, Fx_*). Monitors' scrolling scanlines and flicker (UnlitTwoTexture + TextureScroll +
Sine), animated screens (AnimatedTexture), TV static (the GaussianNoise / LessOrEqual / TextureTransform
chains of effects/tvscreen_noise*), scrolling water and dust...

Each material with an effect gets a small program of ops over its own float variables. Slots 0-14 are
fixed: 0-2 $color, 3 $alpha, 4 $frame, 5-9 $basetexturetransform and 10-14 $texture2transform (each as
scale s, scale t, rotation, translate s, translate t, about the centre). The VMT's own variables and the
constants the proxies use follow. The engine runs the program once a frame and draws with the result.

Chunks:
  MFXH  u32 count, then per effect: u16 mat, u8 flags, u8 frames, s16 frame0 tex, s16 tex2,
        u16 first op, u16 ops, u16 first var, u16 vars (16 bytes)
  MFXO  per op: u8 op, u8 dst, u8 a, u8 b, u8 c, u8 d, u16 pad, f32 k0, f32 k1 (16 bytes)
  MFXV  f32 initial value per variable
"""
import math
import re
import struct

from sounds import parse_kv as parse_kv_list

FX_UNLIT = 1      # no lightmap / no model lighting (UnlitGeneric, UnlitTwoTexture)
FX_TWOTEX = 2     # times $texture2 (UnlitTwoTexture)
FX_SELFILLUM = 4  # + base x base alpha ($selfillum)
FX_XFORM = 8      # $basetexturetransform is animated
FX_XFORM2 = 16    # $texture2transform is animated
FX_COLOR = 32     # $color / $alpha are animated

(OP_SINE, OP_SCROLL, OP_ANIM, OP_RAMP, OP_NOISE, OP_ABS, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_COPY,
 OP_CLAMP, OP_LESSEQ, OP_XFORM, OP_RAND) = range(15)

S_COLOR, S_ALPHA, S_FRAME, S_XF1, S_XF2, S_USER = 0, 3, 4, 5, 10, 15
MAX_FRAMES = 16
MAX_VARS = 250


def _num(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def _vec(v):
    """'[0 0]' / '{1 1 1}' / '0.5' -> list of floats"""
    if v is None:
        return None
    s = str(v).strip()
    if s[:1] in "[{":
        vals = [_num(x) for x in s.strip("[]{} ").split()]
        if s[0] == "{":
            vals = [x / 255.0 for x in vals]
        return vals or [0.0]
    return [_num(s)]


class Program:
    def __init__(self):
        self.vars = [1.0, 1.0, 1.0, 1.0, 0.0] + [1, 1, 0, 0, 0] * 2
        self.names = {"$color": (S_COLOR, 3), "$alpha": (S_ALPHA, 1), "$frame": (S_FRAME, 1),
                      "$basetexturetransform": (S_XF1, 5), "$texture2transform": (S_XF2, 5)}
        self.ops = []
        self.flags = 0

    def declare(self, name, value):
        name = name.lower()
        vals = _vec(value)
        if name in ("$color",):
            base, _ = self.names[name]
            for i, x in enumerate((vals * 3)[:3]):
                self.vars[base + i] = x
            return
        if name in ("$alpha", "$frame"):
            self.vars[self.names[name][0]] = vals[0]
            return
        if name in self.names or len(self.vars) + len(vals) > MAX_VARS:
            return
        self.names[name] = (len(self.vars), len(vals))
        self.vars.extend(vals)

    def const(self, x):
        self.vars.append(float(x))
        return len(self.vars) - 1

    def slot(self, ref, default=0.0):
        """'$var', '$var[1]' or a number -> a variable slot"""
        if ref is None:
            return self.const(default)
        r = str(ref).strip().lower()
        m = re.match(r"^(\$\w+)(?:\[(\d+)\])?$", r)
        if not m:
            return self.const(_num(r, default))
        nm, idx = m.group(1), int(m.group(2) or 0)
        if nm not in self.names:
            self.declare(nm, "0")
        base, n = self.names[nm]
        return base + min(idx, n - 1)

    def width(self, ref):
        r = str(ref or "").strip().lower()
        if re.match(r"^\$\w+$", r) and r in self.names:
            return self.names[r][1]
        return 1

    def op(self, code, dst, a=255, b=255, c=255, d=255, k0=0.0, k1=0.0):
        self.ops.append((code, dst, a, b, c, d, k0, k1))

    def mark(self, dst):
        if S_XF1 <= dst < S_XF1 + 5:
            self.flags |= FX_XFORM
        elif S_XF2 <= dst < S_XF2 + 5:
            self.flags |= FX_XFORM2
        elif dst <= S_ALPHA:
            self.flags |= FX_COLOR


def compile_material(raw_vmt, shader, frames_of):
    """-> (Program, base frame count, texture2 name) or None when nothing moves and nothing's special"""
    try:
        top = parse_kv_list(raw_vmt)
    except Exception:  # noqa: BLE001
        return None
    if not top:
        return None
    body = top[0][1] if isinstance(top[0][1], list) else []
    p = Program()
    proxies = []
    tex2 = None
    selfillum = False
    base = None
    for k, v in body:
        kl = k.lower()
        if isinstance(v, list):
            if kl == "proxies":
                proxies = v
            continue
        if kl == "$texture2":
            tex2 = v.lower().replace("\\", "/")
        elif kl == "$basetexture":
            base = v.lower().replace("\\", "/")
        elif kl == "$selfillum" and _num(v) != 0:
            selfillum = True
        if kl.startswith("$"):
            p.declare(kl, v)
    sh = shader.lower()
    if sh in ("unlitgeneric", "unlittwotexture"):
        p.flags |= FX_UNLIT
    if sh == "unlittwotexture" and tex2:
        p.flags |= FX_TWOTEX
    if selfillum and sh in ("vertexlitgeneric", "lightmappedgeneric"):
        p.flags |= FX_SELFILLUM
    if sh == "windowimposter":
        p.flags |= FX_UNLIT  # (the view out of a window isn't lit by the room)
    if sh == "refract":
        # Refract: the scene behind, blurred and tinted. Here: an unlit pane in $refracttint, as clear as
        # its blur allows ($bluramount: frosted glass hides more of what's behind)
        vals = {kk.lower(): vv for kk, vv in body if isinstance(vv, str)}
        tint = vals.get("$refracttint", "[1 1 1]").strip()
        nums = [float(x) for x in tint.strip("[]{}").split()[:3]] or [1.0, 1.0, 1.0]
        if tint.startswith("{"):
            nums = [x / 255.0 for x in nums]
        while len(nums) < 3:
            nums.append(nums[-1])
        p.flags |= FX_UNLIT
        for i in range(3):
            p.vars[S_COLOR + i] = max(0.0, min(2.0, nums[i]))
        blur = _num(vals.get("$bluramount"), 0)
        p.vars[S_ALPHA] = 0.45 if blur > 0 else 0.22
    nframes = 1
    for name, args in proxies:
        if not isinstance(args, list):
            continue
        a = {kk.lower(): vv for kk, vv in args if isinstance(vv, str)}
        n = name.lower()
        res = a.get("resultvar")
        if n == "sine":
            dst = p.slot(res)
            p.op(OP_SINE, dst, p.slot(a.get("sinemin"), -1), p.slot(a.get("sinemax"), 1),
                 p.slot(a.get("sineperiod"), 1), p.slot(a.get("timeoffset"), 0))
            p.mark(dst)
        elif n == "texturescroll":
            var = (a.get("texturescrollvar") or "$basetexturetransform").lower()
            dst = S_XF2 if var == "$texture2transform" else S_XF1
            rate, ang = _num(a.get("texturescrollrate"), 1), math.radians(_num(a.get("texturescrollangle"), 0))
            scale = _num(a.get("texturescale"), 1)
            p.op(OP_SCROLL, dst, k0=rate * math.cos(ang), k1=rate * math.sin(ang))
            p.vars[dst] = p.vars[dst + 1] = scale
            p.mark(dst)
        elif n == "animatedtexture":
            var = (a.get("animatedtexturevar") or "$basetexture").lower()
            if var != "$basetexture" or not base:
                continue
            nframes = max(1, min(MAX_FRAMES, frames_of(base)))
            p.op(OP_ANIM, S_FRAME, k0=_num(a.get("animatedtextureframerate"), 15), k1=nframes)
        elif n == "linearramp":
            dst = p.slot(res)
            p.op(OP_RAMP, dst, p.slot(a.get("rate"), 1), p.slot(a.get("initialvalue"), 0))
            p.mark(dst)
        elif n == "gaussiannoise":
            dst = p.slot(res)
            p.op(OP_NOISE, dst, p.slot(a.get("mean"), 0), p.slot(a.get("halfwidth"), 1),
                 p.slot(a.get("minval"), -1e9), p.slot(a.get("maxval"), 1e9))
            p.mark(dst)
        elif n == "uniformnoise":
            dst = p.slot(res)
            p.op(OP_RAND, dst, p.slot(a.get("minval"), 0), p.slot(a.get("maxval"), 1))
            p.mark(dst)
        elif n in ("abs", "equals"):
            dst = p.slot(res)
            p.op(OP_ABS if n == "abs" else OP_COPY, dst, p.slot(a.get("srcvar1")))
            p.mark(dst)
        elif n in ("add", "subtract", "multiply", "divide"):
            dst = p.slot(res)
            code = {"add": OP_ADD, "subtract": OP_SUB, "multiply": OP_MUL, "divide": OP_DIV}[n]
            p.op(code, dst, p.slot(a.get("srcvar1")), p.slot(a.get("srcvar2")))
            p.mark(dst)
        elif n == "clamp":
            dst = p.slot(res)
            p.op(OP_CLAMP, dst, p.slot(a.get("srcvar1")), p.slot(a.get("min"), 0), p.slot(a.get("max"), 1))
            p.mark(dst)
        elif n == "lessorequal":
            dst = p.slot(res)
            p.op(OP_LESSEQ, dst, p.slot(a.get("srcvar1")), p.slot(a.get("srcvar2")),
                 p.slot(a.get("lessequalvar")), p.slot(a.get("greatervar")))
            p.mark(dst)
        elif n == "texturetransform":
            var = (res or "$basetexturetransform").lower()
            dst = S_XF2 if var == "$texture2transform" else S_XF1
            tv, sv, rv = a.get("translatevar"), a.get("scalevar"), a.get("rotatevar")
            t0 = p.slot(tv) if tv else p.const(0)
            t1 = (t0 + 1 if p.width(tv) >= 2 else p.const(0)) if tv else p.const(0)
            s0 = p.slot(sv) if sv else p.const(1)
            s1 = (s0 + 1 if p.width(sv) >= 2 else s0) if sv else s0
            r = p.slot(rv) if rv else p.const(0)
            # (t0, t1 are consecutive in both cases above only when the var is a vector: store both)
            p.op(OP_XFORM, dst, t0, t1, s0, s1, k0=float(r))
            p.mark(dst)
        # (WaterLOD, MaterialModify, ToggleTexture...: nothing to do here)
    if not p.flags and nframes <= 1:
        return None
    return p, nframes, tex2 if p.flags & FX_TWOTEX else None


def chunks(chunk, fx_records):
    """fx_records: [(mat index, Program, frame0 tex, frames, tex2 tex)]"""
    head, ops, vars_ = [], [], []
    for mi, p, f0, nf, t2 in fx_records:
        head.append(struct.pack(">HBBhhHHHH", mi, p.flags, nf, f0, t2, len(ops), len(p.ops), len(vars_), len(p.vars)))
        for code, dst, a, b, c, d, k0, k1 in p.ops:
            ops.append(struct.pack(">BBBBBBHff", code, dst, a, b, c, d, 0, k0, k1))
        vars_.extend(p.vars)
    chunk(b"MFXH", struct.pack(">I", len(head)) + b"".join(head))
    chunk(b"MFXO", b"".join(ops))
    chunk(b"MFXV", struct.pack(f">{len(vars_)}f", *vars_))
