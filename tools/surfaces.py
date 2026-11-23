"""
Surface properties (scripts/surfaceproperties*.txt): the footstep sounds of every material, like the
PC's CBasePlayer::PlayStepSound. Both converters build the same surface list from the game files,
so a map's per-material surface ids index weapons.hgm's footstep table.
"""
from sounds import parse_kv


class Surfaces:
    def __init__(self, fs):
        self.props = {}  # name -> {key: value}, with "base" resolved
        raw = fs.read("scripts/surfaceproperties_manifest.txt")
        files = []
        if raw:
            for _, block in parse_kv(raw.decode("latin-1")):
                if isinstance(block, list):
                    files += [v for k, v in block if k.lower() == "file"]
        if not files:
            files = ["scripts/surfaceproperties.txt", "scripts/surfaceproperties_hl2.txt"]
        for f in files:
            d = fs.read(f)
            if not d:
                continue
            for name, block in parse_kv(d.decode("latin-1")):
                if isinstance(block, list):
                    self.props[name.lower()] = {k.lower(): v for k, v in block if isinstance(v, str)}
        self.names = sorted(self.props) or ["default"]
        self.index = {n: i for i, n in enumerate(self.names)}

    def get(self, name, key, depth=0):
        p = self.props.get((name or "default").lower())
        if p is None:
            return None if name == "default" else self.get("default", key, depth + 1)
        if key in p:
            return p[key]
        if depth < 8 and "base" in p:
            return self.get(p["base"], key, depth + 1)
        return None if name == "default" else self.get("default", key, depth + 1)

    def id_of(self, name):
        return self.index.get((name or "default").lower(), self.index.get("default", 0))

    def steps(self):
        """per surface id: (stepleft, stepright) soundscript names"""
        return [(self.get(n, "stepleft") or "", self.get(n, "stepright") or "") for n in self.names]

    def impacts(self):
        """per surface id: (impacthard, impactsoft, bulletimpact, scraperough) soundscript names"""
        return [tuple(self.get(n, k) or "" for k in ("impacthard", "impactsoft", "bulletimpact", "scraperough"))
                for n in self.names]

    def materials(self):
        """per surface id: Source's gamematerial letter (C concrete, M metal, W wood, D dirt, Y glass,
        F flesh...), which picks the bullet decal and impact effect"""
        out = []
        for n in self.names:
            g = (self.get(n, "gamematerial") or "C").strip()[:1].upper() or "C"
            out.append(ord(g))
        return out
