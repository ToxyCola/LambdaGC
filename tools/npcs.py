"""NPCs for hl2gc map packs: skinned models, skeletons, baked animations, choreographed scenes and
the voice lines they speak (sound bank + caption strings).

Skinning is rigid (each vertex follows its strongest bone) and drawn with GX's matrix palette: meshes
are split into batches that use at most 10 bones, so the GameCube transforms vertices in hardware.
"""
import hashlib
import re
import math
import struct
from strips import stripify
from collections import OrderedDict

import numpy as np

from anim import AnimatedModel, pose_matrices, skeleton_index
from material import load_material
from model import load_model, pack_normals
from props import parse_vec
from scenes import EV, scene_crc

NPC_VERT_BUDGET = 34000
# main characters keep full detail for close-ups and faces; crowds give way first
HERO_MODELS = ("gman", "alyx", "barney", "eli", "kleiner", "mossman", "breen", "monk", "vortigaunt", "odessa")
MAX_MTX = 10
FRAME_POS_SCALE = 64.0  # s16 positions, 1/64 inch
MAX_SEQ_FRAMES = 160
ANIM_FPS = 10.0

# models that aim by their aim_yaw / aim_pitch pose parameters (cameras, turrets)
AIM_MODELS = ("combine_camera", "ceiling_turret", "floor_turret", "ground_turret", "bunker_gun01")

NPC_MODELS = {
    "npc_barney": "models/barney.mdl", "npc_alyx": "models/alyx.mdl", "npc_eli": "models/eli.mdl",
    "npc_kleiner": "models/kleiner.mdl", "npc_mossman": "models/mossman.mdl", "npc_breen": "models/breen.mdl",
    "npc_gman": "models/gman.mdl", "npc_monk": "models/monk.mdl", "npc_vortigaunt": "models/vortigaunt.mdl",
    "npc_dog": "models/dog.mdl", "npc_metropolice": "models/police.mdl", "npc_combine_s": "models/combine_soldier.mdl",
    "npc_zombie": "models/zombie/classic.mdl", "npc_zombie_torso": "models/zombie/classic_torso.mdl",
    "npc_fastzombie": "models/zombie/fast.mdl", "npc_poisonzombie": "models/zombie/poison.mdl",
    "npc_headcrab": "models/headcrabclassic.mdl", "npc_headcrab_fast": "models/headcrab.mdl",
    "npc_headcrab_black": "models/headcrabblack.mdl", "npc_antlion": "models/antlion.mdl",
    "npc_antlionguard": "models/antlion_guard.mdl", "npc_crow": "models/crow.mdl", "npc_pigeon": "models/pigeon.mdl",
    "npc_seagull": "models/seagull.mdl", "npc_stalker": "models/stalker.mdl", "npc_cscanner": "models/combine_scanner.mdl",
    "npc_manhack": "models/manhack.mdl", "npc_rollermine": "models/roller.mdl", "npc_barnacle": "models/barnacle.mdl",
    "npc_turret_floor": "models/combine_turrets/floor_turret.mdl",
    "npc_turret_ceiling": "models/combine_turrets/ceiling_turret.mdl",
    "npc_turret_ground": "models/combine_turrets/ground_turret.mdl",
    "npc_combine_camera": "models/combine_camera/combine_camera.mdl",
    "combine_mine": "models/props_combine/combine_mine01.mdl",
    "npc_ichthyosaur": "models/ichthyosaur.mdl", "npc_fisherman": "models/lostcoast/fisherman/fisherman.mdl",
    # the big flyers (CNPC_AttackHelicopter, CNPC_CombineGunship, CNPC_CombineDropship) and the strider
    "npc_helicopter": "models/combine_helicopter.mdl", "npc_combinegunship": "models/gunship.mdl",
    "npc_combinedropship": "models/combine_dropship.mdl", "npc_strider": "models/combine_strider.mdl",
}
MODEL_KEY_CLASSES = ("generic_actor", "monster_generic", "cycler", "cycler_actor", "npc_furniture", "npc_citizen")
# Animation activities every NPC gets, in engine order: idle, walk, run
BASE_ACTS = ("ACT_IDLE", "ACT_WALK", "ACT_RUN", "ACT_DIESIMPLE")
SCRIPT_SEQ_KEYS = ("m_iszentry", "m_iszplay", "m_iszpostidle", "m_iszidle")

# chunk record layouts (big-endian), mirrored in engine/source/pack.h
SKEL_FMT = ">HHIIhhhh"          # nbones, nseqs, firstBone, firstSeq, seqIdle, seqWalk, seqRun, seqDie (20)
NMDL_FMT = ">HHIffff"           # skel, nbatch, firstBatch, eyeHeight, radius, height, pad           (24)
BONE_FMT = ">i12f"              # parent, poseToBone 3x4                                               (52)
BATCH_FMT = ">IHHIIB10sBBB2x"    # firstVert, numVerts, mat, firstIndex, numIndices, nbones, bones[10], flexed,
                                # eyeball (255 = none)                                                    (32)
NEYE_FMT = ">i3f3f3fff"         # bone, origin, up, forward (into the head), z offset, iris scale (bind space) (48)
# props whose models have several bones are drawn posed and animated like characters (window bars that
# break, rotating machinery, the Citadel's pod conveyors), not frozen in their bind pose
PROP_ACTOR_CLASSES = ("prop_dynamic", "prop_dynamic_override", "prop_dynamic_ornament", "prop_ragdoll",
                      "prop_physics", "prop_physics_override", "prop_physics_multiplayer", "prop_physics_respawnable",
                      "prop_vehicle_airboat", "prop_vehicle_jeep", "prop_vehicle_prisoner_pod", "prop_vehicle_apc",
                      "prop_vehicle_crane")
SEQ_FMT = ">IHHffIHHII"         # name, nframes, flags, fps, speed, firstFrame, maskBones, numLayers,
                                # firstMask, firstLayer                                                  (32)
SEQF_LOOP, SEQF_DELTA, SEQF_POST, SEQF_REALTIME = 1, 2, 4, 8
LAYER_FMT = ">hHffff"           # sequence (skeleton-local), autolayer flags, start, peak, tail, end    (20)
# head-turning bones for look-at, in order head, neck, spine (-1 when the skeleton lacks one)
LOOK_BONES = ("valvebiped.bip01_head1", "valvebiped.bip01_neck1", "valvebiped.bip01_spine4")  # bone names are lowercased
SCENE_FMT = ">IHHIf"            # name, numEvents, pad, firstEvent, duration                           (16)
SEVT_FMT = ">BB4H4H2x"          # gesture timing: numTags, pad, playback[4], original[4] (x4096)  (20)
SEV_FMT = ">BBhffIIIII"         # type, flags, sound, start, end, actor, p1, p2, p3, caption             (32)
SND_FMT = ">III"                # bank offset, volume<<24 | bank bytes, soundlevel<<24 | samples        (12)


def pack_sound(r):
    off, size, samples, (vol, lvl) = r
    return struct.pack(SND_FMT, off, (min(255, max(1, int(round(vol * 255)))) << 24) | (size & 0xFFFFFF),
                       (min(255, max(0, int(lvl))) << 24) | (samples & 0xFFFFFF))
NFMD_FMT = ">IHHIIIIII"         # ctl0, nctl, ndesc, rule0, nrule, flex0, nflex, att0, natt           (32)
NFCT_FMT = ">Iff"               # controller name, min, max                                            (12)
NFRL_FMT = ">HHI"               # flexdesc, numops, firstop                                            (8)
NFOP_FMT = ">If"                # op, value (index ops store the index as a float)                     (8)
NFLX_FMT = ">hh4fII"            # flexdesc, flexpair, targets[4], firstVA, numVA                       (28)
NFVA_FMT = ">I3hBx"             # NVTX vertex, delta * 4096, side                                      (12)
NATT_FMT = ">Ii12f"             # attachment name, bone, local 3x4                                     (56)
PHON_KEY_FMT = ">I"
SPHI_FMT = ">II"                # per sound: first phoneme record, count                              (8)
SPHR_FMT = ">HHff"              # phoneme index (PHON), pad, start, end                               (12)
SFEV_FMT = ">IHH"               # per scene event: first flex track, count, pad                        (8)
SFTR_FMT = ">IffII"             # controller name, min, max, first sample, count                       (20)
SFSM_FMT = ">ff"                # time, value                                                          (8)


def ragdoll_solids(fs, am):
    """The model's ragdoll from its .phy: [(bone, parent bone or -1, radius, swing degrees)]; [] if it has
    no ragdoll (one solid or none)."""
    import re as _re
    from model import parse_phy
    path = am.root.path
    if isinstance(path, (tuple, list)):
        path = path[0]
    phy = path[:-4] + ".phy" if str(path).lower().endswith(".mdl") else None
    raw = fs.read(phy) if phy else None
    if not raw or len(raw) < 16:
        return []
    try:
        hulls, owners = parse_phy(raw)
    except Exception:
        hulls, owners = [], []
    hdr_size, _id, nsolids = struct.unpack_from("<iii", raw, 0)
    if nsolids < 2:
        return []
    i = raw.find(b"solid {")
    if i < 0:
        return []
    text = raw[i:].split(b"\0")[0].decode("latin-1", "replace")
    solids, cons, ranges = {}, {}, {}
    for kind, body in _re.findall(r"(solid|ragdollconstraint)\s*\{([^}]*)\}", text):
        kv = dict(_re.findall(r'"([^"]+)"\s+"([^"]*)"', body))
        if kind == "solid" and "index" in kv:
            solids[int(kv["index"])] = (kv.get("name", "").lower(), kv.get("parent", "").lower())
        elif kind == "ragdollconstraint" and "child" in kv:
            def f(k):
                try:
                    return abs(float(kv.get(k, "0")))
                except ValueError:
                    return 0.0
            def g(k):
                try:
                    return float(kv.get(k, "0"))
                except ValueError:
                    return 0.0
            cons[int(kv["child"])] = max(f("xmin"), f("xmax"), f("ymin"), f("ymax"), f("zmin"), f("zmax"))
            ranges[int(kv["child"])] = tuple(g(a + m) for m in ("min", "max") for a in "xyz")
    names = [b.name.lower() for b in am.skel]
    # each solid's thickness: its hulls' smaller extents (a limb is long one way and thin the others)
    thick = {}
    for h, o in zip(hulls, owners):
        ext = sorted((h.max(0) - h.min(0)).tolist())
        r = (ext[0] + ext[1]) * 0.25
        thick[o] = max(thick.get(o, 0.0), r)
    out = []
    for idx in sorted(solids):
        nm, par = solids[idx]
        if nm not in names:
            continue
        b = names.index(nm)
        pb = names.index(par) if par in names and par != nm else -1
        rad = float(np.clip(thick.get(nm, 3.0), 1.5, 10.0))
        # (lo x, y, z, hi x, y, z; a root or unconstrained solid swings freely)
        lim = ranges.get(idx, (-180.0, -180.0, -180.0, 180.0, 180.0, 180.0))
        out.append((b, pb, rad, float(np.clip(cons.get(idx, 60.0), 5.0, 170.0)), lim))
    return out if len(out) >= 2 else []


def citizen_model(e, mapname, fs, idx):
    m = e.get("model", "").lower().replace("\\", "/")
    ctype = e.get("citizentype", "0").strip()
    if m.endswith(".mdl") and fs.exists(m) and ctype in ("", "0", "4"):
        return m
    group = {"1": "group01", "2": "group02", "3": "group03", "4": "group01"}.get(ctype)
    if not group:
        group = "group03" if mapname.startswith("d3_") else "group02" if mapname.startswith("d2_") else "group01"
    if m.endswith(".mdl"):  # template path for this citizen type: keep the head, change the group
        head = m.rsplit("/", 1)[-1]
        cand = f"models/humans/{group}/{head}"
        if fs.exists(cand):
            return cand
    h = int(hashlib.md5(f"{mapname}{idx}".encode()).hexdigest(), 16)
    heads = [f"male_0{i}" for i in range(1, 10)] + [f"female_0{i}" for i in (1, 2, 3, 4, 6, 7)]
    for k in range(len(heads)):
        cand = f"models/humans/{group}/{heads[(h + k) % len(heads)]}.mdl"
        if fs.exists(cand):
            return cand
    return None


def npc_model_path(e, mapname, fs, idx):
    cls = e.get("classname", "")
    if cls == "npc_citizen":
        return citizen_model(e, mapname, fs, idx)
    m = e.get("model", "").lower().replace("\\", "/")
    if cls in MODEL_KEY_CLASSES or (m.endswith(".mdl") and cls.startswith("npc_")):
        if m.endswith(".mdl") and fs.exists(m):
            return m
    p = NPC_MODELS.get(cls)
    return p if p and fs.exists(p) else None


def batch_triangles(tri_bones):
    """Greedy split of triangles into groups that reference at most MAX_MTX distinct bones."""
    order = sorted(range(len(tri_bones)), key=lambda t: tuple(sorted(tri_bones[t])))
    batches, cur, cur_set = [], [], set()
    for t in order:
        s = cur_set | tri_bones[t]
        if len(s) > MAX_MTX:
            batches.append((cur, cur_set))
            cur, cur_set = [t], set(tri_bones[t])
        else:
            cur.append(t)
            cur_set = s
    if cur:
        batches.append((cur, cur_set))
    return batches


class NPCs:
    def __init__(self, b, fs, mats, mapname, soundscripts, captions, scene_image, bank, log=print):
        self.b, self.fs, self.log = b, fs, log
        self.mapname = mapname
        self.ents = {}          # entity index -> (model path, NMDL index)
        self.actors = set()     # entity indices of multi-bone props
        paths = OrderedDict()
        nbones = {}
        for i, e in enumerate(b.entities):
            cls = e.get("classname", "")
            if cls in PROP_ACTOR_CLASSES:
                p = e.get("model", "").lower().replace("\\", "/")
                if not p.endswith(".mdl") or not fs.exists(p):
                    continue
                if p not in nbones:
                    raw = fs.read(p)
                    nbones[p] = struct.unpack_from("<i", raw, 156)[0] if raw and len(raw) > 160 else 0
                if nbones[p] < 2:
                    continue
                paths.setdefault(p, []).append(i)
                self.ents[i] = p
                self.actors.add(i)
                continue
            if cls == "env_headcrabcanister":
                # the canister (env_headcrabcanister.cpp's fixed model) and the headcrabs it lets out (_crab)
                kl = {k.lower(): v for k, v in e.items() if isinstance(v, str)}
                cp = "models/props_combine/headcrabcannister01a.mdl"
                paths.setdefault(cp, []).append(i)
                self.ents[i] = cp
                try:
                    ht = int(float(kl.get("headcrabtype", "0") or 0))
                except ValueError:
                    ht = 0
                fake = dict(e)
                fake["classname"] = ("npc_headcrab", "npc_headcrab_fast", "npc_headcrab_poison")[max(0, min(2, ht))]
                fake.pop("model", None)
                p2 = npc_model_path(fake, mapname, fs, i)
                if p2:
                    paths.setdefault(p2, [])
                    self.crab = getattr(self, "crab", {})
                    self.crab[i] = p2
                continue
            if not (cls.startswith("npc_") or cls in MODEL_KEY_CLASSES):
                continue
            if cls == "npc_maker":
                # npc_maker makes NPCs of its NPCType: it carries that model (the engine copies the maker,
                # with its keys, into a new NPC each time it spawns one)
                kl = {k.lower(): v for k, v in e.items() if isinstance(v, str)}
                fake = dict(e)
                fake["classname"] = kl.get("npctype", "").lower()
                p = npc_model_path(fake, mapname, fs, i) if fake["classname"] else None
                if p:
                    paths.setdefault(p, []).append(i)
                    self.ents[i] = p
                continue
            p = npc_model_path(e, mapname, fs, i)
            if p:
                paths.setdefault(p, []).append(i)
                self.ents[i] = p

        # ---- meshes, under a vertex budget
        self.models = OrderedDict()
        lods = {}
        hero = lambda p: any(h in p for h in HERO_MODELS)
        for p in paths:
            lod = 0 if hero(p) else 1
            m = load_model(fs, p, lod)
            if m and m.meshes and all(me.bones is not None for me in m.meshes):
                self.models[p] = m
                lods[p] = lod
        nv = lambda m: sum(len(me.positions) for me in m.meshes)
        total = sum(nv(m) for m in self.models.values())
        while total > NPC_VERT_BUDGET:
            cand = [p for p in self.models if lods[p] < 3]
            if not cand:
                break
            crowd = [p for p in cand if not hero(p)]
            p = max(crowd or cand, key=lambda k: nv(self.models[k]))
            m2 = load_model(fs, p, lods[p] + 1)
            lods[p] += 1
            if m2 and m2.meshes and nv(m2) < nv(self.models[p]):
                total += nv(m2) - nv(self.models[p])
                self.models[p] = m2
        for i in list(self.ents):
            if self.ents[i] not in self.models:
                del self.ents[i]

        # ---- materials
        self.mat_of = {}
        by_name = {}
        for p, m in self.models.items():
            for ti, tex in enumerate(m.textures):
                found = None
                for cd in m.cdpaths or [""]:
                    cand = (cd + tex).replace("//", "/").strip("/")
                    if fs.exists(f"materials/{cand}.vmt"):
                        found = cand
                        break
                key = found or tex
                if key not in by_name:
                    by_name[key] = len(mats)
                    mats.append(load_material(fs, key))
                self.mat_of[(p, ti)] = by_name[key]

        # ---- skeletons (models sharing bones and animation files share one skeleton + sequence set)
        self.anim_models = {}
        self.skel_key = {}
        self.own_ptb = {}            # model -> its own bind (poseToBone) per bone
        self.skels = OrderedDict()   # key -> AnimatedModel
        for p in self.models:
            try:
                am = AnimatedModel(fs, p)
            except Exception as ex:  # noqa: BLE001
                log(f"  npc {p}: no skeleton ({ex})")
                continue
            # models share a skeleton (and its sequences) only when their animations come from the same
            # include files; a model with its own sequences and no includes (a held suitcase, a prop) keeps
            # its own: sharing gave the suitcase another model's bind pose and idle
            inc = tuple(f.path for f in am.files[1:])
            key = (tuple(b_.name for b_ in am.skel), inc) if inc else (tuple(b_.name for b_ in am.skel), p)
            self.skel_key[p] = key
            self.skels.setdefault(key, am)
            self.own_ptb[p] = [np.array(b_.pose_to_bone, dtype=np.float64).reshape(3, 4) for b_ in am.skel]
        for i in list(self.ents):
            if self.ents[i] not in self.skel_key:
                del self.ents[i]
        self.model_index = {p: k for k, p in enumerate(p for p in self.models if p in self.skel_key)}

        # ---- scenes used by this map
        self.scene_image = scene_image
        self.scripts = soundscripts
        self.captions = captions
        self.bank = bank
        self.scene_files = OrderedDict()
        self.scene_ents = {}
        for i, e in enumerate(b.entities):
            if e.get("classname") != "logic_choreographed_scene":
                continue
            k = {a.lower(): v for a, v in e.items() if isinstance(v, str)}
            for key in ("scenefile", "resumescenefile"):
                f = k.get(key, "").strip().lower().replace("\\", "/")
                if f and scene_crc(f) in scene_image.entries:
                    self.scene_files.setdefault(f, None)
                    self.scene_ents.setdefault(f, k)
        # env_speaker (CSpeaker): the scenes and lines its own rule script answers its concept with
        self.speakers = {}
        try:
            from talker import speaker_lines
            for i, e in enumerate(b.entities):
                if e.get("classname") != "env_speaker":
                    continue
                k = {a.lower(): v for a, v in e.items() if isinstance(v, str)}
                items, flag = speaker_lines(self.fs, k.get("rulescript", ""), k.get("concept", ""))
                keep = []
                for kind, name, first in items:
                    if kind == "scene":
                        f = name.strip().lower().replace("\\", "/")
                        if scene_crc(f) not in scene_image.entries:
                            continue
                        self.scene_files.setdefault(f, None)
                        self.scene_ents.setdefault(f, k)
                        keep.append(("scene", f, first))
                    else:
                        keep.append(("speak", name, first))
                if keep:
                    self.speakers[i] = (keep, flag)
        except Exception as ex:  # noqa: BLE001
            log(f"  env_speaker: {ex}")
        for f in self.scene_files:
            try:
                self.scene_files[f] = scene_image.scene(f)
            except Exception as ex:  # noqa: BLE001
                log(f"  scene {f}: {ex}")
        self.scene_files = OrderedDict((f, s) for f, s in self.scene_files.items() if s is not None)

        # sequences referenced by name anywhere in the map
        self.wanted_seqs = set()
        for s in self.scene_files.values():
            for _, ev in s.all_events():
                if ev.type in (EV["sequence"], EV["gesture"]) and ev.p1:
                    self.wanted_seqs.add(ev.p1.lower())
        for e in b.entities:
            if e.get("classname") == "scripted_sequence":
                k = {a.lower(): v for a, v in e.items() if isinstance(v, str)}
                for key in SCRIPT_SEQ_KEYS:
                    if k.get(key):
                        self.wanted_seqs.add(k[key].lower())
            for key in ("DefaultAnim", "defaultanim"):
                if e.get(key):
                    self.wanted_seqs.add(e[key].lower())
            # act-busy spots: the sequence an NPC plays there (sitting on a bench, leaning on a fence), by
            # name or by activity (ACT_BUSY_SIT_CHAIR: the model's sequence for it, stored under that name)
            if e.get("classname") == "info_node_hint" and e.get("hintactivity") and \
                    e["hintactivity"].upper() != "ACT_BUSY_QUEUE":
                self.wanted_seqs.add(e["hintactivity"].lower())
                for nm in self.actbusy_of(e["hintactivity"]):
                    if nm:
                        self.wanted_seqs.add(nm.lower())
            for k, v in e.get("_pairs", []):
                parts = v.split(",") if isinstance(v, str) and k.lower().startswith("on") else []
                if len(parts) >= 3 and parts[1].lower() in ("setanimation", "setdefaultanimation", "setidleanimation"):
                    if parts[2]:
                        self.wanted_seqs.add(parts[2].lower())
        # armed characters fight with their weapon's activities (angry idle, attack, the fire gesture over
        # running, aimed run / walk): the engine finds them under their ACT_ names
        # only for the weapons this map's NPCs carry (their additionalequipment, else the class's default:
        # metrocops a pistol, soldiers an SMG): every gun's set on every armed map overflowed the bigger
        # maps' memory budget
        guns = set()
        melee = False
        defaults = {"npc_metropolice": "weapon_pistol", "npc_combine_s": "weapon_smg1"}
        for e in b.entities:
            cls = e.get("classname", "")
            if not cls.startswith("npc_"):
                continue
            eq = (e.get("additionalequipment") or "").lower()
            if eq in ("", "0"):
                eq = defaults.get(cls, "")
            for w in ("pistol", "smg1", "ar2", "shotgun"):
                if eq == "weapon_" + w:
                    guns.add(w)
            if eq in ("weapon_stunstick", "weapon_crowbar"):
                melee = True
        for e in b.entities:  # and what scripts hand them ("giveweapon" inputs)
            for k, v in e.get("_pairs", []):
                parts = v.split(",") if isinstance(v, str) and k.lower().startswith("on") else []
                if len(parts) >= 3 and parts[1].lower() == "giveweapon":
                    w = parts[2].lower().replace("weapon_", "")
                    if w in ("pistol", "smg1", "ar2", "shotgun"):
                        guns.add(w)
                    elif w in ("stunstick", "crowbar"):
                        melee = True
        # cameras and turrets (npc_combinecamera.cpp, npc_turret_*.cpp): their deploy / retract / alert / fire
        self.wanted_seqs.update(("idle", "deploy", "retract", "idlealert", "fire", "alertextend", "ai_retract", "reload",
                                 "carry_pose", "open", "idle_open"))
        if guns or melee:
            self.wanted_seqs.update(("act_idle_angry", "act_range_attack1", "act_gesture_range_attack1"))
        if melee:
            self.wanted_seqs.update(("act_idle_angry_melee", "act_melee_attack_swing", "act_melee_attack1",
                                     "act_gesture_melee_attack_swing"))
        for w in guns:
            self.wanted_seqs.update((f"act_range_attack_{w}", f"act_gesture_range_attack_{w}", f"act_idle_angry_{w}"))
        if guns & {"smg1", "ar2"}:
            self.wanted_seqs.update(("act_run_aim_rifle", "act_walk_aim_rifle"))
        if "pistol" in guns:
            self.wanted_seqs.update(("act_run_aim_pistol", "act_walk_aim_pistol"))
        if "shotgun" in guns:
            self.wanted_seqs.add("act_run_aim_shotgun")
        if guns:
            self.wanted_seqs.update(("act_run_aim", "act_walk_aim"))
        # npc_citizen parented to a func_tracktrain sits or stands there by code (CNPC_Citizen), and gets up
        # with the matching _Exit sequence when unparented
        trains = {e.get("targetname", "").lower() for e in b.entities if e.get("classname") == "func_tracktrain"}
        for e in b.entities:
            if e.get("classname") == "npc_citizen" and e.get("parentname", "").split(",")[0].lower() in trains - {""}:
                self.wanted_seqs.update(("d1_t01_trainride_sit_idle", "d1_t01_trainride_sit_exit",
                                         "d1_t01_trainride_stand", "d1_t01_trainride_stand_exit"))
        # props drawn as actors rest in their default animation, or (ragdolls) lying where they fell
        self.actor_pose = {}
        for i in self.actors:
            p = self.ents.get(i)
            if p not in self.skel_key:
                continue
            am = self.skels[self.skel_key[p]]
            e = b.entities[i]
            lab = None
            if e.get("classname") == "prop_ragdoll":
                lab = am.find("deathpose_back") or am.find("deathpose_front")
            if not lab and (e.get("DefaultAnim") or e.get("defaultanim")):
                lab = am.find(e.get("DefaultAnim") or e.get("defaultanim"))
            if not lab and am.seqs:
                lab = next(iter(am.seqs))
            if lab:
                self.wanted_seqs.add(lab)
                self.actor_pose[i] = lab
        log(f"  npcs: {len(self.ents)} entities, {len(self.model_index)} models ({total} verts), "
            f"{len(self.skels)} skeletons, {len(self.scene_files)} scenes")

    # ---------------------------------------------------------------- helpers
    def gender_of(self, actor, scene_keys):
        """'male01' / 'female01' for $gender01 substitution, from the actor's model."""
        name = actor.lower()
        if name.startswith("!target") and scene_keys:
            name = scene_keys.get("target" + name[7:], name).lower()
        for i, e in enumerate(self.b.entities):
            tn = e.get("targetname", "").lower()
            if tn and (tn == name or (tn.startswith(name.rstrip("*")) and name.endswith("*"))):
                p = self.ents.get(i, e.get("model", "")).lower()
                return "female01" if "female" in p else "male01"
        return "male01"

    _talker = None
    _soundscapes = None

    def sound_for_gender(self, name, gender, sounds, sound_index):
        waves = self.scripts.resolve(name)
        if not waves:
            return -1
        w = waves[0].replace("$gender01", gender)
        if w in sound_index:
            return sound_index[w]
        r = self.bank.add(self.fs, w)
        if not r:
            sound_index[w] = -1
            return -1
        sound_index[w] = len(sounds)
        sounds.append(tuple(r) + (self.scripts.meta(name),))
        self.sound_paths.append(w)
        return sound_index[w]

    def sound_for(self, name, actor, scene_keys, sounds, sound_index):
        waves = self.scripts.resolve(name)
        if not waves:
            return -1
        w = waves[0]
        if "$gender" in w:
            w = w.replace("$gender01", self.gender_of(actor, scene_keys))
        if w in sound_index:
            return sound_index[w]
        r = self.bank.add(self.fs, w)
        if not r:
            sound_index[w] = -1
            return -1
        sound_index[w] = len(sounds)
        sounds.append(tuple(r) + (self.scripts.meta(name),))
        self.sound_paths.append(w)
        return sound_index[w]

    _phon_cache = None

    def phonemes(self):
        """expressions/phonemes.txt: ([controller names], [(phoneme, [weight per controller, -1 = no say])])."""
        if NPCs._phon_cache is not None:
            return NPCs._phon_cache
        keys, out = [], []
        try:
            text = self.fs.read("expressions/phonemes.txt").decode("latin-1")
        except Exception:  # noqa: BLE001
            text = ""
        for line in text.splitlines():
            line = line.strip()
            if line.startswith("$keys"):
                keys = [k.lower() for k in line.split()[1:]]
            elif line.startswith('"'):
                parts = re.findall(r'"([^"]*)"|(\S+)', line)
                toks = [a or b for a, b in parts]
                name, nums = toks[0], toks[2:]
                vals = []
                for i in range(len(keys)):
                    try:
                        w, infl = float(nums[2 * i]), float(nums[2 * i + 1])
                    except (IndexError, ValueError):
                        w, infl = 0.0, 0.0
                    vals.append(w if infl > 0 else -1.0)
                out.append((name.lower(), vals))
        NPCs._phon_cache = (keys, out)
        return NPCs._phon_cache

    def phonemes_of(self, path):
        """Phoneme timings from a WAV's VDAT chunk: [(code, name, start, end)]."""
        try:
            data = self.fs.read(path)
        except Exception:  # noqa: BLE001
            return []
        if not data or data[:4] != b"RIFF":
            return []
        p = 12
        while p + 8 <= len(data):
            cid, sz = struct.unpack_from("<4sI", data, p)
            if cid == b"VDAT":
                out = []
                for line in data[p + 8:p + 8 + sz].decode("latin-1", "replace").splitlines():
                    f = line.split()
                    if len(f) >= 4 and f[0].isdigit():
                        try:
                            out.append((int(f[0]), f[1].lower(), float(f[2]), float(f[3])))
                        except ValueError:
                            pass
                return out
            p += 8 + sz + (sz & 1)
        return []

    # ---------------------------------------------------------------- chunks
    def chunks(self, chunk, s, tint_fn):
        """s: string-pool function shared with the entity chunks (ESTR); tint_fn(pos) -> (r, g, b)."""
        skel_rec, bone_rec, seq_rec, frames = [], [], [], []
        ppi_rec, ppc_rec, ppq_rec = [], [], []
        rgi_rec, rgr_rec, rgl_rec = [], [], []  # NPRI / NPRR: per skeleton its ragdoll solids (the model's .phy)
        mask_rec, layer_rec, rest_rec, skx_rec, sqev_rec = [], [], [], [], []
        move_rec = []  # NSQM: per sequence record, its root motion (dx, dy, dz, dyaw) over the sequence
        self.seq_sound_ev = []  # (global seq, cycle, sound name, run variant) resolved once sounds exist
        skel_index = {}
        self.seq_index = {}     # skeleton key -> {sequence label: index within that skeleton}
        nframe_bytes = 0
        for key, am in self.skels.items():
            skel_index[key] = len(skel_rec)
            first_bone = len(bone_rec)
            for bn in am.skel:
                bone_rec.append(struct.pack(BONE_FMT, bn.parent, *bn.pose_to_bone.reshape(-1)))
            names = []
            base = []
            for act in BASE_ACTS:
                lab = am.find(act)
                if not lab and act == "ACT_DIESIMPLE":
                    # HL2's characters ragdoll instead: fall into the pose the ragdoll settles in
                    lab = am.find("deathpose_back") or am.find("deathpose_front")
                base.append(lab)
                if lab and lab not in names:
                    names.append(lab)
            for nm in sorted(self.wanted_seqs):
                lab = am.find(nm)
                if lab and lab not in names:
                    names.append(lab)
            # layered sequences (gestures, postures) play their autolayers too: bake those as well
            k = 0
            while k < len(names):
                try:
                    for sub in am.layers(names[k]):
                        if sub[0] not in names:
                            names.append(sub[0])
                except Exception:  # noqa: BLE001
                    pass
                k += 1
            first_seq = len(seq_rec)
            seq_idx = {}
            seq_args = {}
            seq_move = {}
            baked = []
            for lab in names:
                try:
                    bk = am.bake_masked(lab, max_fps=ANIM_FPS, max_frames=MAX_SEQ_FRAMES)
                except Exception as ex:  # noqa: BLE001
                    self.log(f"  {lab}: {ex}")
                    continue
                seq_idx[lab] = len(baked)
                baked.append((lab, bk))
            for lab, bk in baked:
                F, n = bk["pos"].shape[:2]
                qf = bk["quat"] / np.maximum(np.linalg.norm(bk["quat"], axis=2, keepdims=True), 1e-12)
                qf = np.where(qf[..., 3:4] < 0, -qf, qf)  # w >= 0: the engine rebuilds w from x, y, z
                q = np.clip(np.round(qf[..., :3] * 32767), -32767, 32767).astype(">i2")
                pp = np.clip(np.round(bk["pos"] * FRAME_POS_SCALE), -32768, 32767).astype(">i2")
                rec = np.concatenate([q, pp], axis=2).astype(">i2")  # [F, n, 6] s16, big-endian
                lays = [(seq_idx[sub], fl, a, b_, c, d) for sub, fl, a, b_, c, d in bk["layers"] if sub in seq_idx]
                flags = ((SEQF_LOOP if bk["loop"] else 0) | (SEQF_DELTA if bk["delta"] else 0) |
                         (SEQF_POST if bk["post"] else 0) | (SEQF_REALTIME if bk["realtime"] else 0))
                seq_args[lab] = (F, flags, bk["fps"], bk["speed"], nframe_bytes // 12, n, len(lays), len(mask_rec),
                                 len(layer_rec))
                seq_rec.append(struct.pack(SEQ_FMT, s(lab), *seq_args[lab]))
                seq_move[lab] = struct.pack(">4f", *bk.get("move", (0.0, 0.0, 0.0, 0.0)))
                move_rec.append(seq_move[lab])
                mask_rec.extend(struct.pack(">BB", b_, max(1, min(255, int(round(w * 255))))) for b_, w in bk["mask"])
                layer_rec.extend(struct.pack(LAYER_FMT, *l) for l in lays)
                # SCRIPT_EVENT_FIREEVENT (1003) / FIRE_INPUT (1100): fires the scripted_sequence's OnScriptEventNN at that cycle
                for cyc, evn, _et, opts, ename in bk["events"]:
                    if evn in (1003, 1100) and opts.strip().isdigit():
                        sqev_rec.append(struct.pack(">IfHH", len(seq_rec) - 1, cyc, int(opts.strip()), 0))
                    # sounds: CL_EVENT_FOOTSTEP_LEFT/RIGHT ("<options>.FootstepLeft", run variant when fast),
                    # SCRIPT_EVENT_SOUND(_VOICE), AE_CL_PLAYSOUND (a soundscript), zombies' steps
                    o = opts.strip()
                    if evn in (6004, 6005):
                        side = "Left" if evn == 6004 else "Right"
                        fbase = o or "NPC_CombineS"
                        self.seq_sound_ev.append((len(seq_rec) - 1, cyc, f"{fbase}.Footstep{side}",
                                                  f"{fbase}.RunFootstep{side}"))
                    elif (evn in (1004, 1008, 5004) or ename == "AE_CL_PLAYSOUND") and o:
                        self.seq_sound_ev.append((len(seq_rec) - 1, cyc, o, ""))
                    elif ename in ("AE_ZOMBIE_STEP_LEFT", "AE_ZOMBIE_STEP_RIGHT"):
                        side = "Left" if ename.endswith("LEFT") else "Right"
                        self.seq_sound_ev.append((len(seq_rec) - 1, cyc, f"Zombie.Footstep{side}", ""))
                frames.append(rec.tobytes())
                nframe_bytes += rec.nbytes
            # activities named by act-busy hints: an alias record under the activity's name, sharing the
            # frames of the model's sequence for it (the engine looks sequences up by name)
            for nm in sorted(self.wanted_seqs):
                if not nm.startswith("act_"):
                    continue
                lab = am.find(nm)
                if lab and lab != nm and lab in seq_args and nm not in seq_idx:
                    seq_idx[nm] = len(seq_rec) - first_seq
                    seq_rec.append(struct.pack(SEQ_FMT, s(nm), *seq_args[lab]))
                    move_rec.append(seq_move[lab])
            # the skeleton's rest pose: bones a sequence's weight list leaves out stay there
            for bn in am.skel:
                qn = np.array(bn.quat, dtype=np.float64)
                qn = qn / max(np.linalg.norm(qn), 1e-12)
                if qn[3] < 0:
                    qn = -qn
                rest_rec.append(struct.pack(">6h", *[int(np.clip(round(v * 32767), -32767, 32767)) for v in qn[:3]],
                                            *[int(np.clip(round(v * FRAME_POS_SCALE), -32768, 32767)) for v in bn.pos]))
            look = []
            for nm in LOOK_BONES:
                i = skeleton_index(am.skel, nm)
                if i < 0:
                    # other rigs (vortigaunts, zombies...): the closest-named bone
                    part = nm.split("_")[-1].rstrip("1234")  # (not "key": that's this skeleton's, used below)
                    want = {"head": ("head1", "head"), "neck": ("neck1", "neck"), "spine": ("spine4", "spine3", "spine2")}[part]
                    i = next((k for w in want for k, b in enumerate(am.skel) if b.name.endswith(w)), -1)
                look.append(i)
            skx_rec.append(struct.pack(">4h", *look, -1))
            self.seq_index[key] = seq_idx
            ids = [seq_idx.get(l, -1) if l else -1 for l in base]
            skel_rec.append(struct.pack(SKEL_FMT, len(am.skel), len(seq_rec) - first_seq, first_bone, first_seq,
                                        *ids))
            # aiming (aim_yaw / aim_pitch) as bone turns, for the things that aim with them
            first_ppc = len(ppc_rec)
            if any(k in am.root.path for k in AIM_MODELS):
                for name, ps, pe, bone, qs, delta in am.pose_controls():
                    ppc_rec.append(struct.pack(">HBBBxxxffI", bone, 0 if name == "aim_yaw" else 1, len(qs), int(delta),
                                               ps, pe, len(ppq_rec)))
                    for q in qs:
                        ppq_rec.append(struct.pack(">4f", *q))
            ppi_rec.append(struct.pack(">HH", first_ppc, len(ppc_rec) - first_ppc))
            # the ragdoll (CBaseAnimating::BecomeRagdoll from the .phy): its solids' bones, each one's parent
            # solid, its thickness and how far its joint swings (the ragdollconstraint's widest range)
            first_rg = len(rgr_rec)
            for bone, parent, radius, swing, lim in ragdoll_solids(self.fs, am):
                rgr_rec.append(struct.pack(">Hhff", bone, parent, radius, swing))
                rgl_rec.append(struct.pack(">6f", *lim))
            rgi_rec.append(struct.pack(">HH", first_rg, len(rgr_rec) - first_rg))

        mdl_rec, batch_rec, verts, idx = [], [], [], []
        fmd_rec, fct_rec, frl_rec, fop_rec, flx_rec, fva_rec, att_rec = [], [], [], [], [], [], []
        eyei, eye_rec = [], []
        vcount = icount = 0
        vdt = np.dtype([("p", ">f4", 3), ("uv", ">i2", 2), ("m", "u1"), ("n", "i1", 3)])
        self.rebound = 0
        for p in self.model_index:
            m = self.models[p]
            first_batch = len(batch_rec)
            # Models sharing a skeleton (citizens male_01 and male_07...) are animated with the first one's
            # bones, but each has its own bind pose: Source skins each with its own poseToBone. The engine
            # uses the shared one, so move each vertex into the shared bind: v' = ptb_shared^-1 * ptb_own * v
            # (exact for rigid skinning), likewise normals, morph deltas and eyeballs.
            corr = None
            own = self.own_ptb.get(p)
            shared = self.skels[self.skel_key[p]].skel
            if own is not None and len(own) == len(shared):
                C = np.zeros((len(own), 3, 4))
                diff = 0.0
                for bi, b_ in enumerate(shared):
                    S = np.array(b_.pose_to_bone, dtype=np.float64).reshape(3, 4)
                    Si = np.linalg.inv(S[:, :3])
                    C[bi, :, :3] = Si @ own[bi][:, :3]
                    C[bi, :, 3] = Si @ (own[bi][:, 3] - S[:, 3])
                    diff = max(diff, float(np.abs(C[bi, :, :3] - np.eye(3)).max()), float(np.abs(C[bi, :, 3]).max()))
                if diff > 1e-3:
                    corr = C
                    self.rebound += 1
            meshes, eyeballs = m.meshes, getattr(m, "eyeballs", [])
            if corr is not None:
                import copy
                meshes = [copy.copy(me) for me in m.meshes]  # (models are cached across maps: never in place)
                lod0_bone = {}
                for me in meshes:
                    v0 = getattr(me, "vvd0", None)
                    if v0 is not None:
                        for u in range(len(me.bones)):
                            lod0_bone.setdefault(int(v0[u]), int(me.bones[u]))
                for me in meshes:
                    bb = np.asarray(me.bones, dtype=np.int64)
                    Cb = corr[bb]
                    me.positions = np.einsum("nij,nj->ni", Cb[:, :, :3], np.asarray(me.positions, dtype=np.float64)) + Cb[:, :, 3]
                    nn = np.einsum("nij,nj->ni", Cb[:, :, :3], np.asarray(me.normals, dtype=np.float64))
                    me.normals = nn / np.maximum(np.linalg.norm(nn, axis=1, keepdims=True), 1e-9)
                    fl = []
                    for desc, pair, tg, vidx, delta, side in getattr(me, "flexes", []):
                        rb = np.array([lod0_bone.get(int(x), 0) for x in vidx], dtype=np.int64)
                        delta = np.einsum("nij,nj->ni", corr[rb][:, :, :3], np.asarray(delta, dtype=np.float64))
                        fl.append((desc, pair, tg, vidx, delta, side))
                    if fl:
                        me.flexes = fl
                eb = []
                for bone, org, up, fwd, zoff, iris in eyeballs:
                    if 0 <= bone < len(corr):
                        Cb = corr[bone]
                        org = Cb[:, :3] @ np.asarray(org, dtype=np.float64) + Cb[:, 3]
                        up = Cb[:, :3] @ np.asarray(up, dtype=np.float64)
                        fwd = Cb[:, :3] @ np.asarray(fwd, dtype=np.float64)
                    eb.append((bone, org, up, fwd, zoff, iris))
                eyeballs = eb
            out_of = {}  # LOD0 vertex -> output vertex ids (a vertex can be in several batches)
            flexed_lod0 = set()
            for me in meshes:
                for f in getattr(me, "flexes", []):
                    flexed_lod0.update(int(x) for x in f[3])
            for me in meshes:
                tri = me.indices.reshape(-1, 3)
                tb = [set(int(me.bones[v]) for v in t) for t in tri]
                for tris, bones in batch_triangles(tb):
                    blist = sorted(bones)
                    slot = {bb: k for k, bb in enumerate(blist)}
                    used = np.unique(tri[tris].reshape(-1))
                    remap = {int(v): k for k, v in enumerate(used)}
                    v = np.zeros(len(used), dtype=vdt)
                    v["p"] = me.positions[used]
                    v["uv"] = np.clip(np.round(me.uvs[used] * 1024), -32768, 32767)
                    v["m"] = [slot[int(me.bones[u])] * 3 for u in used]
                    v["n"] = pack_normals(me.normals[used])
                    ind = np.array([remap[int(x)] for x in tri[tris].reshape(-1)], dtype=">u2")
                    strip = stripify(ind)
                    if strip is not None:
                        ind = strip.astype(">u2")
                    verts.append(v)
                    idx.append(ind)
                    flexed = 0
                    v0 = getattr(me, "vvd0", None)
                    if v0 is not None:
                        for k, u in enumerate(used):
                            key = int(v0[u])
                            out_of.setdefault(key, []).append(vcount + k)
                            if key in flexed_lod0:
                                flexed = 1
                    eye = getattr(me, "eyeball", -1)
                    batch_rec.append(struct.pack(BATCH_FMT, vcount, len(used), self.mat_of.get((p, me.material), 0),
                                                 icount, len(ind), len(blist), bytes(blist) + bytes(10 - len(blist)),
                                                 flexed, eye if 0 <= eye < 255 else 255, 1 if strip is not None else 0))
                    vcount += len(used)
                    icount += len(ind)
            # facial animation: controllers, rules, morph targets on the packed vertices; attachments
            ctl0, rule0, flex0, att0 = len(fct_rec), len(frl_rec), len(flx_rec), len(att_rec)
            for name, lo, hi in getattr(m, "flexctl", []):
                fct_rec.append(struct.pack(NFCT_FMT, s(name), lo, hi))
            for flexdesc, ops in getattr(m, "rules", []):
                frl_rec.append(struct.pack(NFRL_FMT, flexdesc, len(ops), len(fop_rec)))
                for op, val in ops:
                    fop_rec.append(struct.pack(NFOP_FMT, op, val))
            for me in meshes:
                for desc, pair, tg, vidx, delta, side in getattr(me, "flexes", []):
                    first = len(fva_rec)
                    d = np.clip(np.round(delta * 4096), -32767, 32767).astype(int)
                    big = np.abs(delta).max(axis=1) >= 0.02  # invisible offsets cost memory for nothing
                    for k in np.nonzero(big)[0]:
                        for out in out_of.get(int(vidx[k]), ()):
                            fva_rec.append(struct.pack(NFVA_FMT, out, d[k][0], d[k][1], d[k][2], int(side[k])))
                    if len(fva_rec) > first:
                        flx_rec.append(struct.pack(NFLX_FMT, desc, pair if pair else -1, *tg, first, len(fva_rec) - first))
            eyei.append(struct.pack(">HH", len(eye_rec), len(eyeballs)))
            for bone, org, up, fwd, zoff, iris in eyeballs:
                eye_rec.append(struct.pack(NEYE_FMT, bone, *org, *up, *fwd, zoff, iris))
            for name, bone, local in getattr(m, "attachments", []):
                att_rec.append(struct.pack(NATT_FMT, s(name), bone, *local.reshape(-1)))
            fmd_rec.append(struct.pack(NFMD_FMT, ctl0, len(fct_rec) - ctl0, getattr(m, "ndesc", 0), rule0,
                                       len(frl_rec) - rule0, flex0, len(flx_rec) - flex0, att0, len(att_rec) - att0))
            h = float(m.maxs[2] - m.mins[2])
            r = float(max(abs(m.mins[0]), abs(m.maxs[0]), abs(m.mins[1]), abs(m.maxs[1])))
            mdl_rec.append(struct.pack(NMDL_FMT, skel_index[self.skel_key[p]], len(batch_rec) - first_batch,
                                       first_batch, h * 0.9, r, h, 0.0))

        chunk(b"NSKL", struct.pack(">I", len(skel_rec)) + b"".join(skel_rec))
        chunk(b"NPRI", b"".join(rgi_rec))  # per skeleton: (first, count) of its ragdoll solids
        chunk(b"NPRR", b"".join(rgr_rec))
        chunk(b"NPRL", b"".join(rgl_rec))  # each solid's joint ranges (degrees about its x, y, z)
        chunk(b"NPPI", b"".join(ppi_rec))  # per skeleton: (first, count) of its aim controls
        chunk(b"NPPC", b"".join(ppc_rec))
        chunk(b"NPPQ", b"".join(ppq_rec))
        chunk(b"NBON", b"".join(bone_rec))
        chunk(b"NSEQ", b"".join(seq_rec))
        assert len(move_rec) == len(seq_rec)
        chunk(b"NSQM", b"".join(move_rec))
        chunk(b"NFRM", b"".join(frames))
        chunk(b"NMSK", b"".join(mask_rec))
        chunk(b"NLAY", b"".join(layer_rec))
        chunk(b"NRST", b"".join(rest_rec))
        chunk(b"NSKX", b"".join(skx_rec))
        chunk(b"NSQE", struct.pack(">I", len(sqev_rec)) + b"".join(sqev_rec))
        chunk(b"NMDL", struct.pack(">I", len(mdl_rec)) + b"".join(mdl_rec))
        chunk(b"NBAT", b"".join(batch_rec))
        chunk(b"NVTX", np.concatenate(verts).astype(vdt).tobytes() if verts else b"")
        chunk(b"NIDX", np.concatenate(idx).astype(">u2").tobytes() if idx else b"")
        chunk(b"NFMD", b"".join(fmd_rec))
        chunk(b"NFCT", b"".join(fct_rec))
        chunk(b"NFRL", b"".join(frl_rec))
        chunk(b"NFOP", b"".join(fop_rec))
        chunk(b"NFLX", b"".join(flx_rec))
        chunk(b"NFVA", b"".join(fva_rec))
        chunk(b"NATT", b"".join(att_rec))
        chunk(b"NEYI", b"".join(eyei))
        chunk(b"NEYE", b"".join(eye_rec))
        self.log(f"  faces: {len(flx_rec)} morph targets, {len(fva_rec)} vertex offsets, {len(att_rec)} attachments, {self.rebound} models rebound to a shared skeleton")
        self.log(f"  npc data: {len(seq_rec)} sequences ({nframe_bytes // 1024} KB frames), {len(batch_rec)} batches, "
                 f"{vcount} verts")

        # ---- scenes + sounds
        sounds, sound_index = [], {}
        self.sound_paths = []
        scene_rec, ev_rec = [], []
        sfev_rec, sftr_rec, sfsm_rec, sevt_rec = [], [], [], []
        self.scene_index = {}
        ncap = 0
        for f, sc in self.scene_files.items():
            keys = self.scene_ents.get(f, {})
            first = len(ev_rec)
            evs = sorted(sc.all_events(), key=lambda ae: ae[1].start)
            for actor, ev in evs:
                snd, cap = -1, 0
                if ev.type == EV["speak"]:
                    snd = self.sound_for(ev.p1, actor or "", keys, sounds, sound_index)
                    if ev.cc_type == 0:
                        text = self.captions.get(ev.cc_token or ev.p1)
                        if text:
                            cap = s(text)
                            ncap += 1
                elif ev.type == EV["loop"]:
                    snd = max(-1, min(32767, ev.loop_count))   # extra parameter: loop count
                elif ev.type == EV["moveto"]:
                    snd = max(0, min(32767, int(ev.dist)))     # extra parameter: stop distance
                flags = ev.flags & 0x3F
                ft0 = len(sftr_rec)
                for name, fl, lo, hi, tracks in (ev.flex or []):
                    if not (fl & 1) or not tracks or not tracks[0]:
                        continue  # inactive track
                    sm0 = len(sfsm_rec)
                    for t, val, _curve in tracks[0]:
                        sfsm_rec.append(struct.pack(SFSM_FMT, t, val))
                    sftr_rec.append(struct.pack(SFTR_FMT, s(name.lower()), lo, hi, sm0, len(sfsm_rec) - sm0))
                sfev_rec.append(struct.pack(SFEV_FMT, ft0, len(sftr_rec) - ft0, 0))
                # gestures: CChoreoEvent's playback -> original timing tags (where the sequence's apex,
                # loop and end land inside the event), so the engine can time the gesture like Source
                tp = to = []
                if ev.type == EV["gesture"] and len(ev.abs_tags) == 2 and len(ev.abs_tags[0]) == len(ev.abs_tags[1]):
                    tp = [v for _, v in ev.abs_tags[0]][:4]
                    to = [v for _, v in ev.abs_tags[1]][:4]
                q16 = lambda v: max(0, min(4096, int(round(v * 4096))))  # noqa: E731
                sevt_rec.append(struct.pack(SEVT_FMT, len(tp), 0, *([q16(v) for v in tp] + [0] * (4 - len(tp))),
                                            *([q16(v) for v in to] + [0] * (4 - len(to)))))
                ev_rec.append(struct.pack(SEV_FMT, ev.type, flags, snd, ev.start, ev.end, s(actor or ""),
                                          s(ev.p1 or ""), s(ev.p2 or ""), s(ev.p3 or ""), cap))
            self.scene_index[f] = len(scene_rec)
            scene_rec.append(struct.pack(SCENE_FMT, s(f), len(ev_rec) - first, 0, first, sc.duration))
        chunk(b"SCEN", struct.pack(">I", len(scene_rec)) + b"".join(scene_rec))
        chunk(b"SCEV", b"".join(ev_rec))
        chunk(b"SEVT", b"".join(sevt_rec))
        chunk(b"SFEV", b"".join(sfev_rec))
        chunk(b"SFTR", b"".join(sftr_rec))
        chunk(b"SFSM", b"".join(sfsm_rec))

        # ambient_generic sounds go in the same table
        self.ambient = {}
        for i, e in enumerate(self.b.entities):
            if e.get("classname") != "ambient_generic":
                continue
            msg = e.get("message", "")
            if not msg:
                continue
            k = self.sound_for(msg, "", {}, sounds, sound_index)
            if k >= 0:
                self.ambient[i] = k
        # door sounds: func_door* noise1/noise2/locked_sound, prop doors' overrides or the defaults
        self.door_snd = {}
        for i, e in enumerate(self.b.entities):
            cls = e.get("classname", "")
            if cls not in ("func_door", "func_door_rotating", "prop_door_rotating"):
                continue
            k = {a.lower(): v for a, v in e.items() if isinstance(v, str)}
            if cls == "prop_door_rotating":
                names = (k.get("soundmoveoverride") or k.get("soundopenoverride") or "DoorSound.DefaultMove",
                         k.get("soundcloseoverride") or "DoorSound.DefaultArrive",
                         k.get("soundlockedoverride") or "DoorSound.DefaultLocked")
            else:
                names = (k.get("noise1", ""), k.get("noise2", ""), k.get("locked_sound", ""))
            ids = tuple(self.sound_for(n, "", {}, sounds, sound_index) if n else -1 for n in names)
            if any(x >= 0 for x in ids):
                self.door_snd[i] = ids
        # animation sound events (NSQS: like NSQE, option = sound, pad = the run variant or 0xFFFF)
        sqs = []
        for sq, cyc, n1, n2 in self.seq_sound_ev:
            a = self.sound_for(n1, "", {}, sounds, sound_index)
            b_ = self.sound_for(n2, "", {}, sounds, sound_index) if n2 else -1
            if a < 0 and b_ >= 0:
                a = b_
            if a >= 0:
                sqs.append(struct.pack(">IfHH", sq, cyc, a, b_ if b_ >= 0 else 0xFFFF))
        chunk(b"NSQS", struct.pack(">I", len(sqs)) + b"".join(sqs))
        # moving things' sounds: trains (start, move loop, stop), func_movelinear, func_rotating (loop), buttons
        self.ent_snd = {}
        for i, e in enumerate(self.b.entities):
            cls = e.get("classname", "")
            k = {a.lower(): v for a, v in e.items() if isinstance(v, str)}
            names = None
            if cls == "func_tracktrain":
                names = (k.get("startsound", ""), k.get("movesound", ""), k.get("stopsound", ""))
            elif cls == "func_movelinear":
                names = (k.get("startsound", ""), "", k.get("stopsound", ""))
            elif cls == "func_rotating":
                names = ("", k.get("message", ""), "")
            elif cls in ("func_button", "func_rot_button"):
                try:
                    n = int(float(k.get("sounds", "0") or 0))
                except ValueError:
                    n = 0
                names = (f"Buttons.snd{n}" if n else "", "", "")
            # effect entities (Source's EnvSpark.cpp, tesla.cpp, fire.cpp, effects.cpp, explode.cpp)
            elif cls == "env_spark":
                names = ("DoSpark", "", "")
            elif cls == "point_tesla":
                names = (k.get("m_soundname") or "DoSpark", "", "")
            elif cls == "env_fire":
                names = ("Fire.Plasma" if k.get("firetype", "0") == "1" else "", "", "")
            elif cls == "env_splash":
                names = ("Splash.SplashSound", "", "")
            elif cls == "env_gunfire":
                names = (k.get("shootsound") or "Weapon_AR2.NPC_Single", "", "")
            elif cls == "env_explosion":
                names = ("BaseExplosionEffect.Sound", "", "")
            # cameras and turrets (npc_combinecamera.cpp, npc_turret_ceiling.cpp, npc_turret_ground.cpp)
            elif cls == "npc_combine_camera":
                names = ("NPC_CombineCamera.Active", "NPC_CombineCamera.Angry", "NPC_CombineCamera.Click")
            elif cls == "npc_turret_ceiling":
                names = ("NPC_CeilingTurret.Deploy", "NPC_CeilingTurret.ShotSounds", "NPC_CeilingTurret.Retire")
            elif cls == "npc_turret_ground":
                names = ("NPC_FloorTurret.Ping", "NPC_FloorTurret.ShotSounds", "DoSpark")
            # manhacks and scanners (npc_manhack.cpp, npc_scanner.cpp): engine / fly loop, attack, death; an
            # npc_maker of them carries them for the ones it makes
            mk = k.get("npctype", "").lower() if cls == "npc_maker" else cls
            if cls.startswith("func_tank") and cls not in ("func_tankcontrols", "func_tanktrain"):
                names = (("Weapon_Mortar.Single" if cls == "func_tankmortar" else "Weapon_RPG.Single" if cls == "func_tankrocket"
                          else "Weapon_functank.Single"), "Func_Tank.BeginUse", "")
            if cls == "env_headcrabcanister":
                names = ("HeadcrabCanister.LaunchSound", "HeadcrabCanister.Explosion", "HeadcrabCanister.Open")
            if cls == "combine_mine":
                names = ("NPC_CombineMine.Hop", "NPC_CombineMine.OpenHooks", "NPC_CombineMine.CloseHooks")
            if mk == "npc_manhack":
                names = ("NPC_Manhack.EngineNoise", "NPC_Manhack.Slice", "NPC_Manhack.Die")
            elif mk in ("npc_cscanner", "npc_clawscanner"):
                names = ("NPC_CScanner.FlyLoop", "NPC_CScanner.TakePhoto", "NPC_CScanner.Die")
            if names:
                ids = tuple(self.sound_for(n, "", {}, sounds, sound_index) if n else -1 for n in names)
                if any(x >= 0 for x in ids):
                    self.ent_snd[i] = ids
        # soundscapes (env_soundscape*): each entity's flattened soundscape, its dsp room and elements
        self.scape_of = {}
        scape_rec, elem_rec, wave_rec = [], [], []
        try:
            if NPCs._soundscapes is None:
                from sounds import Soundscapes
                NPCs._soundscapes = Soundscapes(self.fs)
            ss = NPCs._soundscapes
            named = {e.get("targetname", "").lower(): e for e in self.b.entities if e.get("targetname")}
            scape_ix = {}
            for i, e in enumerate(self.b.entities):
                cls = e.get("classname", "")
                if cls not in ("env_soundscape", "env_soundscape_triggerable", "env_soundscape_proxy"):
                    continue
                k = {a.lower(): v for a, v in e.items() if isinstance(v, str)}
                nm = k.get("soundscape", "")
                if cls == "env_soundscape_proxy":
                    main = named.get(k.get("mainsoundscapename", "").lower())
                    nm = main.get("soundscape", "") if main else ""
                nm = nm.lower()
                if not nm or nm not in ss.raw:
                    continue
                if nm not in scape_ix:
                    dsp, els = ss.flatten(nm)
                    first = len(elem_rec)
                    for typ, pos, lvl, v0, v1, t0, t1, waves in els:
                        ids = [self.sound_for(w if w.lower().endswith((".wav", ".mp3")) else w, "", {}, sounds,
                                             sound_index) for w in waves]
                        ids = [x for x in ids if x >= 0][:16]
                        if not ids:
                            continue
                        elem_rec.append(struct.pack(">BBBBHHffff", typ, pos, lvl, 0, len(wave_rec), len(ids),
                                                    v0, v1, t0, t1))
                        wave_rec += ids
                    scape_ix[nm] = len(scape_rec)
                    scape_rec.append(struct.pack(">BBHI", 1 if dsp is None else max(0, min(255, dsp)), 0,
                                                 len(elem_rec) - first, first))
                self.scape_of[i] = scape_ix[nm]
        except Exception as ex:  # noqa: BLE001
            self.log(f"  soundscapes: {ex}")
        chunk(b"SSCP", struct.pack(">III", len(scape_rec), len(elem_rec), len(wave_rec)) + b"".join(scape_rec) +
              b"".join(elem_rec) + b"".join(struct.pack(">h", w) for w in wave_rec) +
              b"\0" * (2 if len(wave_rec) % 2 else 0))
        self.log(f"  soundscapes: {len(scape_rec)} ({len(elem_rec)} elements, {len(wave_rec)} sounds), "
                 f"{len(self.scape_of)} entities; moving-thing sounds on {len(self.ent_snd)}")
        # what NPCs say when the player uses them or they start/stop following (the response system)
        self.talk = {}
        self.sound_caps = {}
        try:
            if NPCs._talker is None:
                from talker import Talker
                NPCs._talker = Talker(self.fs, log=self.log)
            tk = NPCs._talker
            for i, path in self.ents.items():
                e = self.b.entities[i]
                cls = e.get("classname", "")
                gender = "female01" if "female" in path.lower() else "male01"
                keys = {}
                for con, key in (("TLK_USE", "_use"), ("TLK_STARTFOLLOW", "_follow"), ("TLK_STOPFOLLOW", "_unfollow")):
                    ids = []
                    for name in tk.speech(cls, con, gender, self.scene_image)[:12]:
                        k = self.sound_for_gender(name, gender, sounds, sound_index)
                        if k >= 0 and k not in ids:
                            ids.append(k)
                            text = self.captions.get(name)
                            if text:
                                self.sound_caps[k] = s(text)
                    if ids:
                        keys[key] = " ".join(map(str, ids))
                if keys:
                    self.talk[i] = keys
        except Exception as ex:  # noqa: BLE001
            self.log(f"  talker: {ex}")
        # env_speaker: "c<scene>" / "s<sound>" items, "!" first for the one said first (displayfirst)
        for i, (items, flag) in getattr(self, "speakers", {}).items():
            out = []
            for kind, name, first in items:
                if kind == "scene":
                    if name in self.scene_index:
                        out.append(("!" if first else "") + "c%d" % self.scene_index[name])
                else:
                    k = self.sound_for(name, "", {}, sounds, sound_index)
                    if k >= 0:
                        out.append(("!" if first else "") + "s%d" % k)
                        text = self.captions.get(name)
                        if text:
                            self.sound_caps[k] = s(text)
            if out:
                self.talk.setdefault(i, {})["_spk"] = " ".join(out)
                if flag:
                    self.talk[i]["_spkf"] = flag
        # extra named sounds a caller wants in this table (weapon and item sounds)
        self.extra_sound_index = {}
        for n in getattr(self, "extra_sound_names", ()):
            k = self.sound_for(n, "", {}, sounds, sound_index)
            if k >= 0:
                self.extra_sound_index[n] = k
        # lip sync: the phoneme->mouth-shape table and each line's phoneme timings
        keys, phon = self.phonemes()
        pk = {n: i for i, (n, _) in enumerate(phon)}
        phon_rec = struct.pack(">HH", len(keys), len(phon)) + b"".join(struct.pack(">I", s(k)) for k in keys)
        for name, ws in phon:
            phon_rec += struct.pack(">I", s(name)) + b"".join(struct.pack(">f", w) for w in ws)
        chunk(b"PHON", phon_rec)
        sphi, sphr = [], []
        for path in self.sound_paths:
            first = len(sphr)
            for code, name, t0, t1 in self.phonemes_of(path):
                k = pk.get(name)
                if k is not None:
                    sphr.append(struct.pack(SPHR_FMT, k, 0, t0, t1))
            sphi.append(struct.pack(SPHI_FMT, first, len(sphr) - first))
        chunk(b"SPHI", b"".join(sphi))
        chunk(b"SPHR", b"".join(sphr))
        chunk(b"SNDS", struct.pack(">I", len(sounds)) + b"".join(pack_sound(r) for r in sounds))
        chunk(b"SCAP", b"".join(struct.pack(">I", self.sound_caps.get(k, 0)) for k in range(len(sounds))))
        self.log(f"  scenes: {len(scene_rec)} ({len(ev_rec)} events), sounds: {len(sounds)}, captions: {ncap}")

    def actbusy_of(self, name):
        """scripts/actbusy.txt: an act-busy's (busy, entry, exit) sequence or activity names ('' none)"""
        if not hasattr(self, "_actbusy"):
            self._actbusy = {}
            try:
                from sounds import parse_kv
                raw = self.fs.read("scripts/actbusy.txt")
                for _top, body in parse_kv(raw.decode("latin-1")):
                    for nm, kvs in body:
                        if not isinstance(kvs, list):
                            continue
                        k = {a.lower(): v for a, v in kvs if isinstance(v, str)}
                        self._actbusy.setdefault(nm.lower(), (
                            k.get("busy_sequence") or k.get("busy_anim") or "",
                            k.get("entry_sequence") or k.get("entry_anim") or "",
                            k.get("exit_sequence") or k.get("exit_anim") or ""))
            except Exception as ex:  # noqa: BLE001
                self.log(f"  actbusy.txt: {ex}")
        return self._actbusy.get((name or "").lower(), ("", "", ""))

    def extra_keys(self, i, e, tint_fn):
        """Keys added to entity i's key/values in ENTS."""
        out = []
        if e.get("classname") == "info_node_hint" and e.get("hintactivity"):
            busy, entry, exit_ = self.actbusy_of(e["hintactivity"])
            if busy:
                out.append(("_busy", busy.lower()))
            if entry:
                out.append(("_busyin", entry.lower()))
            if exit_:
                out.append(("_busyout", exit_.lower()))
        if i in getattr(self, "crab", {}) and self.crab[i] in self.model_index:
            out.append(("_crab", str(self.model_index[self.crab[i]])))
        if i in self.ents and self.ents[i] in self.model_index:
            out.append(("_npc", str(self.model_index[self.ents[i]])))
            if i in getattr(self, "actors", ()):
                out.append(("_actor", "1"))
                if i in getattr(self, "actor_pose", {}):
                    out.append(("_pose", self.actor_pose[i]))
            org = parse_vec(e.get("origin", "0 0 0"))
            out.append(("_tint", "%d %d %d" % tint_fn([org[0], org[1], org[2] + 40])))
        if e.get("classname") == "logic_choreographed_scene":
            f = {a.lower(): v for a, v in e.items() if isinstance(v, str)}.get("scenefile", "").strip().lower()
            f = f.replace("\\", "/")
            if f in getattr(self, "scene_index", {}):
                out.append(("_scene", str(self.scene_index[f])))
        if i in getattr(self, "ambient", {}):
            out.append(("_snd", str(self.ambient[i])))
        for k, v in getattr(self, "talk", {}).get(i, {}).items():
            out.append((k, v))
        if i in getattr(self, "door_snd", {}):
            out.append(("_doorsnd", "%d %d %d" % self.door_snd[i]))
        if i in getattr(self, "ent_snd", {}):
            out.append(("_esnd", "%d %d %d" % self.ent_snd[i]))
        if i in getattr(self, "scape_of", {}):
            out.append(("_scape", str(self.scape_of[i])))
        return out
