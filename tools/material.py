"""VMT (KeyValues) parsing and resolution down to what a fixed-function GPU can draw."""
import re

_TOKEN = re.compile(r'"([^"]*)"|(//[^\n]*)|([{}])|([^\s{}"]+)')


def parse_kv(text):
    """Minimal KeyValues parser. Returns (root_name, dict) with lowercase keys."""
    toks = []
    for m in _TOKEN.finditer(text):
        if m.group(2):
            continue
        if m.group(3):
            toks.append(m.group(3))
        elif m.group(1) is not None:
            toks.append(("s", m.group(1)))
        else:
            toks.append(("s", m.group(4)))
    pos = 0

    def block():
        nonlocal pos
        d = {}
        while pos < len(toks):
            t = toks[pos]
            if t == "}":
                pos += 1
                return d
            if t == "{":  # stray brace
                pos += 1
                continue
            key = t[1].lower()
            pos += 1
            if pos >= len(toks):
                break
            v = toks[pos]
            if v == "{":
                pos += 1
                d[key] = block()
            elif v == "}":
                continue
            else:
                d.setdefault(key, v[1])
                pos += 1
        return d

    if not toks:
        return "", {}
    name = toks[0][1] if toks[0] not in ("{", "}") else ""
    pos = 1
    while pos < len(toks) and toks[pos] != "{":
        pos += 1
    pos += 1
    return name.lower(), block()


# Material kinds the GameCube renderer understands
KIND_OPAQUE = 0
KIND_ALPHATEST = 1
KIND_TRANSLUCENT = 2
KIND_ADDITIVE = 3
KIND_WATER = 4
KIND_SKY = 5
KIND_NODRAW = 6
VISIBLE_TOOLS = ("tools/toolsblack", "tools/toolsblack_noportal", "tools/toolswhite")
KIND_BLEND = 7  # WorldVertexTransition (two textures blended by vertex alpha)
KIND_CAMERA = 8
KIND_EYE = 9     # "eyes"/"eyerefract": sclera with mesh UVs, iris projected along where the eye looks  # monitor screens: $basetexture _rt_Camera, filled at run time by a point_camera


class Material:
    def __init__(self, name):
        self.name = name
        self.shader = ""
        self.basetexture = None
        self.basetexture2 = None
        self.kind = KIND_OPAQUE
        self.water = None
        self.color = (255, 255, 255)
        self.missing = False
        self.surfaceprop = None
        self.envmap = None      # $envmap (WindowImposter: the cubemap it shows)

    def __repr__(self):
        return f"<Material {self.name} {self.shader} {self.basetexture} kind={self.kind}>"


def _truthy(v):
    try:
        return float(v) != 0
    except (TypeError, ValueError):
        return False


def _float(v, d):
    try:
        return float(str(v).strip("[]{} "))
    except (TypeError, ValueError):
        return d


def _color(v, d):
    """Source colours: "{r g b}" in 0..255 or "[r g b]" in 0..1."""
    if not isinstance(v, str):
        return d
    s = v.strip()
    try:
        parts = [float(x) for x in s.strip("[]{} ").split()[:3]]
    except ValueError:
        return d
    if len(parts) < 3:
        return d
    if s.startswith("["):
        parts = [x * 255 for x in parts]
    return tuple(int(max(0, min(255, round(x)))) for x in parts)


def load_material(fs, name, depth=0):
    name = name.lower().replace("\\", "/")
    m = Material(name)
    raw = fs.read(f"materials/{name}.vmt")
    if raw is None and name.startswith("maps/") and depth < 4:
        # vbsp's per-map cubemap copy (maps/<map>/<material>_<x>_<y>_<z>, kept in the map's pakfile): a patch of
        # the original material with the map's cubemap, so the original is what it looks like
        import re
        base = re.sub(r"_-?\d+_-?\d+_-?\d+$", "", name.split("/", 2)[2] if name.count("/") >= 2 else name)
        if base != name:
            return load_material(fs, base, depth + 1)
    if raw is None:
        m.missing = True
        return m
    shader, kv = parse_kv(raw.decode("latin-1", "replace"))
    if shader == "patch" and depth < 4:
        base = load_material(fs, kv.get("include", "").lower().replace("materials/", "").replace(".vmt", ""), depth + 1)
        base.name = name
        for sect in ("insert", "replace"):
            for k, v in kv.get(sect, {}).items():
                if k == "$basetexture" and isinstance(v, str):
                    base.basetexture = v.lower()
                if k == "$surfaceprop" and isinstance(v, str):
                    base.surfaceprop = v.lower()
                if k == "$envmap" and isinstance(v, str):
                    base.envmap = v.lower().replace("\\", "/")
        return base
    m.shader = shader
    sp = kv.get("$surfaceprop")
    if isinstance(sp, str):
        m.surfaceprop = sp.lower()
    bt = kv.get("$basetexture")
    if isinstance(bt, str):
        m.basetexture = bt.lower().replace("\\", "/")
    bt2 = kv.get("$basetexture2")
    if isinstance(bt2, str):
        m.basetexture2 = bt2.lower().replace("\\", "/")

    iris = kv.get("$iris")
    if shader in ("eyes", "eyerefract") and isinstance(iris, str):
        m.kind = KIND_EYE
        m.basetexture2 = iris.lower().replace("\\", "/")
        if not m.basetexture:
            m.basetexture = m.basetexture2
        return m
    if m.basetexture and m.basetexture.startswith("_rt_camera"):
        m.kind = KIND_CAMERA
        m.basetexture = None
    elif shader in ("water",) or _truthy(kv.get("%compileslime")) or (
            _truthy(kv.get("%compilewater")) and not m.basetexture):
        m.kind = KIND_WATER
        m.color = _color(kv.get("$fogcolor"), (40, 60, 60))
        nm = kv.get("$normalmap") or kv.get("$bumpmap")
        for sub in kv.values():  # dx9 blocks hold the bump maps of the slime materials
            if not nm and isinstance(sub, dict):
                nm = sub.get("$normalmap") or sub.get("$bumpmap")
        m.water = {
            "fog": m.color,
            "fogstart": _float(kv.get("$fogstart"), 0.0),
            "fogend": _float(kv.get("$fogend"), 400.0),
            "refracttint": _color(kv.get("$refracttint"), (255, 255, 255)),
            "reflect": min(1.0, _float(kv.get("$reflectamount"), 0.4)),
            "normal": nm.lower().replace("\\", "/") if isinstance(nm, str) else "dev/water_normal",
            "slime": _truthy(kv.get("%compileslime")),
        }
    elif shader == "windowimposter":
        # a window onto somewhere that isn't built: Source draws the view out of it from the map's cubemap
        # (convert.py gives it the cubemap's outward face as a texture, projected from inside the room)
        ev = kv.get("$envmap")
        if isinstance(ev, str) and ev.lower() != "env_cubemap":
            m.envmap = ev.lower().replace("\\", "/")
    elif shader == "refract":
        # glass that bends what's behind it (door panes, the lab's lenses, frosted windows): drawn as a
        # see-through pane in its $refracttint (matfx gives it the colour and how clear it is)
        m.kind = KIND_TRANSLUCENT
        m.color = _color(kv.get("$refracttint"), (255, 255, 255))
    elif shader in ("sky",) or name.startswith("tools/toolsskybox"):
        m.kind = KIND_SKY
    elif name in VISIBLE_TOOLS:
        pass  # (drawn, black: the G-Man's void in the intro is a room of these; script_intro needs it black)
    elif name.startswith("tools/") or shader in ("spritecard",):
        m.kind = KIND_NODRAW
    elif shader == "worldvertextransition":
        m.kind = KIND_BLEND
    elif _truthy(kv.get("$additive")):
        m.kind = KIND_ADDITIVE
    elif _truthy(kv.get("$translucent")):
        m.kind = KIND_TRANSLUCENT
    elif _truthy(kv.get("$alphatest")):
        m.kind = KIND_ALPHATEST
    return m
