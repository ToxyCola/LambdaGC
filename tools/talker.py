"""Half-Life 2's response system (scripts/talker/*.txt), reduced to what the GameCube engine uses:
for each NPC class, the lines it can say for a concept (TLK_USE, TLK_STARTFOLLOW, ...).

Rules match on criteria; here a rule counts for a class when one of its criteria tests the NPC's
classname, and for a concept when one tests "concept". Other criteria (map, health, squad...) are
ignored, so a class gets every line any of its rules for that concept could pick on PC."""
import re

CONCEPTS = ("TLK_USE", "TLK_STARTFOLLOW", "TLK_STOPFOLLOW", "TLK_HELLO")


def _tokens(text):
    text = re.sub(r"//[^\n]*", "", text)
    for m in re.finditer(r'"([^"]*)"|([{}])|([^\s{}"]+)', text):
        yield m.group(1) if m.group(1) is not None else (m.group(2) or m.group(3))


class Talker:
    def __init__(self, fs, log=print):
        self.criteria = {}   # name -> (key, value)
        self.responses = {}  # name -> [(kind, name)]
        self.rules = []      # (criteria names, response names)
        self._load(fs, "scripts/talker/response_rules.txt", set())
        self.lines = {}      # (classname, concept) -> [(kind, name)]
        for crit, resp in self.rules:
            classes, concepts = [], []
            for c in crit:
                k, v = self.criteria.get(c.lower(), ("", ""))
                if k == "classname":
                    classes.append(v.lower())
                elif k == "concept":
                    concepts.append(v.upper())
            for cls in classes:
                for con in concepts:
                    if con in CONCEPTS:
                        out = self.lines.setdefault((cls, con), [])
                        for r in resp:
                            for item in self.responses.get(r.lower(), []):
                                if item not in out:
                                    out.append(item)
        log(f"talker: {len(self.rules)} rules, {len(self.lines)} class/concept line sets")

    def _load(self, fs, path, seen):
        if path in seen:
            return
        seen.add(path)
        raw = fs.read(path)
        if not raw:
            return
        toks = list(_tokens(raw.decode("latin-1")))
        i = 0
        while i < len(toks):
            t = toks[i].lower()
            if t == "#include" and i + 1 < len(toks):
                self._load(fs, "scripts/" + toks[i + 1].replace("\\", "/"), seen)
                i += 2
            elif t == "criterion" and i + 3 < len(toks):
                self.criteria[toks[i + 1].lower()] = (toks[i + 2].lower(), toks[i + 3])
                i += 4
            elif t in ("response", "rule", "enumeration") and i + 2 < len(toks) and toks[i + 2] == "{":
                name = toks[i + 1]
                j, depth, body = i + 3, 1, []
                while j < len(toks) and depth:
                    if toks[j] == "{":
                        depth += 1
                    elif toks[j] == "}":
                        depth -= 1
                    if depth:
                        body.append(toks[j])
                    j += 1
                if t == "response":
                    items = []
                    for k in range(len(body) - 1):
                        if body[k].lower() in ("speak", "scene"):
                            items.append((body[k].lower(), body[k + 1]))
                            if k + 2 < len(body) and body[k + 2].lower() == "displayfirst":
                                getattr(self, "firsts", set()).add((name.lower(), body[k + 1]))
                    self.responses[name.lower()] = items
                    if body and body[0].lower() in ("norepeat", "sequential", "permitrepeats"):
                        getattr(self, "rflags", {})[name.lower()] = body[0].lower()
                elif t == "rule":
                    crit, resp, k = [], [], 0
                    while k < len(body):
                        w = body[k].lower()
                        if w == "criteria":
                            k += 1
                            while k < len(body) and body[k].lower() not in ("response", "criteria", "matchonce",
                                                                           "applycontext"):
                                crit.append(body[k])
                                k += 1
                            continue
                        if w == "response":
                            k += 1
                            while k < len(body) and body[k].lower() not in ("response", "criteria", "matchonce",
                                                                           "applycontext"):
                                resp.append(body[k])
                                k += 1
                            continue
                        k += 1
                    self.rules.append((crit, resp))
                i = j
            elif t == "response" and i + 2 < len(toks):
                # one-line response: response Name speak "x"
                self.responses.setdefault(toks[i + 1].lower(), []).append((toks[i + 2].lower(), toks[i + 3]
                                                                          if i + 3 < len(toks) else ""))
                i += 4
            else:
                i += 1

    def speech(self, cls, concept, gender, scenes):
        """soundscript names an NPC of this class can say for a concept ($gender01 resolved)."""
        out = []
        for kind, name in self.lines.get((cls.lower(), concept), []):
            if kind == "speak":
                out.append(name)
            elif kind == "scene":
                path = name.lower().replace("\\", "/").replace("$gender01", gender)
                try:
                    sc = scenes.scene(path)
                except Exception:  # noqa: BLE001
                    sc = None
                if not sc:
                    continue
                for _, ev in sc.all_events():
                    if ev.type == 5 and ev.p1:  # speak
                        out.append(ev.p1)
                        break
        return out


def speaker_lines(fs, path, concept):
    """env_speaker (CSpeaker): what its own rule script says for its concept. Returns [(kind, name, first)]
    and the response group's flag ("norepeat", "sequential", "permitrepeats" or "")."""
    t = Talker.__new__(Talker)
    t.criteria, t.responses, t.rules, t.firsts, t.rflags = {}, {}, [], set(), {}
    path = path.replace("\\", "/").lower()
    if not path.startswith("scripts/"):
        path = "scripts/" + path
    t._load(fs, path, set())
    out, flag = [], ""
    for crit, resp in t.rules:
        ok = False
        for c in crit:
            k, v = t.criteria.get(c.lower(), ("", ""))
            if k == "concept" and v.lower() == concept.lower():
                ok = True
        if not ok:
            continue
        for r in resp:
            flag = flag or t.rflags.get(r.lower(), "")
            for kind, name in t.responses.get(r.lower(), []):
                item = (kind, name, (r.lower(), name) in t.firsts)
                if item not in out:
                    out.append(item)
    return out, flag
