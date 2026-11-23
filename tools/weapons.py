"""hl2gc/weapons.hgm: the player's weapons (first-person viewmodels with their animations, world
models), pickup items, and their sounds - one pack the engine keeps loaded across levels.

Weapon numbers (damage, fire rate, clip sizes) are Half-Life 2's defaults on Normal; sounds come from
each weapon's script in the install."""
import os
import re
import struct

from anim import AnimatedModel
from convert import KIND_ADDITIVE, KIND_ALPHATEST, KIND_NODRAW, KIND_SKY, KIND_TRANSLUCENT, PACK_VERSION, align, \
    build_texture, checker
from npcs import NPCs
from sounds import parse_kv
from vtf import VTF

F_MELEE, F_AUTO, F_PHYS, F_THROW, F_ROCKET, F_SHOTGUN = 1, 2, 4, 8, 16, 32
AMMO = ["", "pistol", "357", "smg1", "ar2", "buckshot", "xbowbolt", "grenade", "rpg_round", "smg1_grenade", "ar2altfire"]
AMMO_MAX = [0, 150, 12, 225, 60, 30, 10, 5, 3, 3, 3]

# class, viewmodel, world model, slot, clip, ammo, damage, seconds between shots, pellets, spread, flags
WEAPONS = [
    ("weapon_crowbar", "v_crowbar", "w_crowbar", 0, -1, "", 10, 0.4, 1, 0, F_MELEE),
    ("weapon_physcannon", "v_physcannon", "w_physics", 0, -1, "", 0, 0.5, 1, 0, F_PHYS),
    ("weapon_pistol", "v_pistol", "w_pistol", 1, 18, "pistol", 8, 0.12, 1, 0.01, 0),
    ("weapon_357", "v_357", "w_357", 1, 6, "357", 40, 0.75, 1, 0, 0),
    ("weapon_smg1", "v_smg1", "w_smg1", 2, 45, "smg1", 4, 0.075, 1, 0.04, F_AUTO),
    ("weapon_ar2", "v_irifle", "w_irifle", 2, 30, "ar2", 8, 0.1, 1, 0.025, F_AUTO),
    ("weapon_shotgun", "v_shotgun", "w_shotgun", 3, 6, "buckshot", 8, 0.7, 7, 0.09, F_SHOTGUN),
    ("weapon_crossbow", "v_crossbow", "w_crossbow", 3, 1, "xbowbolt", 100, 1.6, 1, 0, 0),
    ("weapon_frag", "v_grenade", "w_grenade", 4, -1, "grenade", 150, 1.2, 1, 0, F_THROW),
    ("weapon_rpg", "v_rpg", "w_rocket_launcher", 4, -1, "rpg_round", 150, 2.0, 1, 0, F_ROCKET),
    ("weapon_bugbait", "v_bugbait", "w_bugbait", 5, -1, "", 0, 1.0, 1, 0, F_THROW),
]
# activities the engine plays, in WEAP record order
VM_ACTS = [("ACT_VM_IDLE",), ("ACT_VM_DRAW",), ("ACT_VM_HOLSTER",), ("ACT_VM_PRIMARYATTACK", "ACT_VM_HITCENTER"),
           ("ACT_VM_SECONDARYATTACK",), ("ACT_VM_RELOAD", "ACT_SHOTGUN_RELOAD_START"), ("ACT_VM_DRYFIRE",),
           ("ACT_VM_MISSCENTER", "ACT_VM_THROW")]
SOUND_KEYS = ["single_shot", "reload", "empty", "melee_hit"]
# sounds the engine plays by name (index order = WSND table order)
EXTRA_SOUNDS = ["Weapon_PhysCannon.Pickup", "Weapon_PhysCannon.Drop", "Weapon_PhysCannon.Launch",
                "Weapon_PhysCannon.DryFire", "Weapon_PhysCannon.TooHeavy", "Weapon_Crowbar.Single",
                "Weapon_Crowbar.Melee_HitWorld", "BaseExplosionEffect.Sound", "Missile.Ignite",
                "Player.PickupWeapon", "SuitRecharge.Deny", "HL2Player.FlashlightOn", "Breakable.MatWood",
                "Breakable.MatGlass", "Breakable.MatMetal", "Breakable.MatConcrete", "Physics.WaterSplash",
                "Player.DrownStart", "Airboat_engine_start", "Airboat_engine_idle", "Airboat_engine_fullthrottle",
                "Airboat_engine_stop", "Airboat_impact_hard", "ATV_engine_start", "ATV_engine_idle",
                "ATV_thirdgear_noshift", "ATV_engine_stop", "ATV_impact_heavy", "ATV_turbo_on"]

EXTRA_SOUNDS += ["Geiger.BeepHigh", "Geiger.BeepLow", "Water.StepLeft", "Water.StepRight", "Wade.StepLeft",
                 "Wade.StepRight", "Player.Swim"]
# NPC combat (npc.c) and vehicle guns (vehicle.c)
EXTRA_SOUNDS += ["Weapon_Pistol.NPC_Single", "Weapon_SMG1.NPC_Single", "Weapon_AR2.NPC_Single",
                 "Weapon_Shotgun.NPC_Single", "Weapon_StunStick.Melee_Hit", "Zombie.AttackHit", "NPC_HeadCrab.Bite",
                 "NPC_Antlion.MeleeAttack", "PropJeep.FireCannon", "PropJeep.FireChargedCannon", "Airboat.FireGunHeavy"]

# HEV suit voice: scripts/sentences.txt entries the suit says (CBasePlayer::OnTakeDamage,
# CHL2_Player::ApplyBattery, CItemSuit). Order must match the engine's SUIT_* enum.
SUIT_SENTENCES = (["HEV_AAx", "HEV_DMG0", "HEV_DMG1", "HEV_DMG2", "HEV_DMG3", "HEV_DMG4", "HEV_DMG5", "HEV_DMG6",
                   "HEV_DMG7", "HEV_HLTH1", "HEV_HLTH2", "HEV_HLTH3", "HEV_MED1", "HEV_HEAL7", "HEV_DET0",
                   "HEV_DET1", "HEV_DET2"] + ["HEV_%dP" % i for i in range(20)])
PAUSE = "(pause)"


def sentence_words(fs):
    """name -> list of wav paths (relative to sound/) and PAUSE markers."""
    raw = fs.read("scripts/sentences.txt")
    out = {}
    if not raw:
        return out
    for line in raw.decode("latin-1").splitlines():
        line = line.split("//")[0].strip()
        if not line:
            continue
        parts = line.split()
        name = parts[0].upper()
        if name not in (n.upper() for n in SUIT_SENTENCES):
            continue
        words, folder = [], ""
        text = " ".join(parts[1:])
        text = re.sub(r"\{[^}]*\}", " ", text)  # closecaption length block
        text = re.sub(r"\([^)]*\)", " ", text)  # (p100) (e30) options
        for tok in text.split():
            if "/" in tok:
                folder, tok = tok.rsplit("/", 1)
                folder += "/"
            pause = tok.endswith((",", "."))
            tok = tok.rstrip(",.")
            if tok and not re.fullmatch(r"[\d.]+", tok):
                words.append(f"{folder}{tok}.wav")
            if pause:
                words.append(PAUSE)
        out[name] = words
    return out


K_HEALTH, K_SUIT, K_AMMO, K_WEAPON, K_HEV, K_CRATE = 0, 1, 2, 3, 4, 5
K_NPCHELD = 9  # only ever held by NPCs (their weapon scripts' world models): not a pickup
NPC_HELD = [("weapon_stunstick", "models/weapons/w_stunbaton.mdl"),
            ("weapon_citizensuitcase", "models/weapons/w_suitcase_passenger.mdl"),
            ("weapon_citizenpackage", "models/weapons/w_package.mdl"),
            ("weapon_alyxgun", "models/weapons/w_alyx_gun.mdl"),
            ("weapon_annabelle", "models/weapons/w_annabelle.mdl"),
            # what the player's weapons fire, drawn as the game's own models
            ("proj_rocket", "models/weapons/w_missile.mdl"),
            ("proj_bolt", "models/crossbow_bolt.mdl"),
            ("proj_smgnade", "models/weapons/ar2_grenade.mdl")]
# The game's own effect textures (the engine's FX_* slots, in this order): (material texture, mode, size cap,
# frame). Muzzle flashes, bullet-hole decals per surface (decals_subrect.txt's Impact.*), blood, the
# explosion sprite's frames, smoke, flecks, sparks, splashes, the AR2's energy ball, a glow.
FX_TEXTURES = (
    [(f"effects/muzzleflash{i}", "opaque", 128, 0) for i in (1, 2, 3, 4)] +
    [(f"effects/combinemuzzle{i}", "opaque", 64, 0) for i in (1, 2)] +
    [(f"decals/concrete/shot{i}", "opaque", 64, 0) for i in (1, 3, 5)] +
    [(f"decals/metal/shot{i}", "opaque", 64, 0) for i in (1, 3, 5)] +
    [(f"decals/wood/shot{i}", "alpha", 64, 0) for i in (1, 3, 5)] +
    [(f"decals/glass/shot{i}", "opaque", 64, 0) for i in (1, 3, 5)] +
    [("decals/blood1", "opaque", 64, 0), ("decals/blood3", "opaque", 64, 0), ("decals/yblood1", "opaque", 64, 0)] +
    [("sprites/zerogxplode", "opaque", 64, f) for f in range(15)] +
    [("particle/particle_smokegrenade", "alpha", 64, 0),
     ("effects/fleck_cement1", "alpha", 32, 0), ("effects/fleck_cement2", "alpha", 32, 0),
     ("effects/fleck_wood1", "alpha", 32, 0), ("effects/fleck_wood2", "alpha", 32, 0),
     ("effects/spark", "opaque", 64, 0), ("effects/blood_core", "alpha", 64, 0), ("effects/splash2", "alpha", 64, 0),
     ("effects/comball/comball", "opaque", 64, 0), ("sprites/glow04", "opaque", 64, 0)] +
    # env_fire's flames (fire.cpp: sprites/flamelet1-5), env_beam / point_tesla / env_laser beams,
    # the combine camera's eye glow
    [(f"sprites/flamelet{i}", "opaque", 64, 0) for i in (1, 2, 3, 4, 5)] +
    [("sprites/laserbeam", "opaque", 64, 0), ("sprites/physbeam", "opaque", 64, 0), ("sprites/lgtning", "opaque", 64, 0),
     ("sprites/light_glow03", "opaque", 64, 0)])
# class, model, kind, ammo type or weapon index, amount, pickup sound
ITEMS = [
    ("item_dynamic_resupply", "models/items/healthkit.mdl", K_HEALTH, 0, 25, "HealthKit.Touch"),
    ("item_healthkit", "models/items/healthkit.mdl", K_HEALTH, 0, 25, "HealthKit.Touch"),
    ("item_healthvial", "models/healthvial.mdl", K_HEALTH, 0, 10, "HealthVial.Touch"),
    ("item_battery", "models/items/battery.mdl", K_SUIT, 0, 15, "ItemBattery.Touch"),
    ("item_suit", "models/items/hevsuit.mdl", K_HEV, 0, 0, "Item.Pickup"),
    ("item_ammo_pistol", "models/items/boxsrounds.mdl", K_AMMO, 1, 20, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_pistol_large", "models/items/boxsrounds.mdl", K_AMMO, 1, 100, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_357", "models/items/357ammo.mdl", K_AMMO, 2, 6, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_357_large", "models/items/357ammobox.mdl", K_AMMO, 2, 12, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_smg1", "models/items/boxmrounds.mdl", K_AMMO, 3, 45, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_smg1_large", "models/items/boxmrounds.mdl", K_AMMO, 3, 225, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_ar2", "models/items/combine_rifle_cartridge01.mdl", K_AMMO, 4, 20, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_ar2_large", "models/items/combine_rifle_ammo01.mdl", K_AMMO, 4, 60, "BaseCombatCharacter.AmmoPickup"),
    ("item_box_buckshot", "models/items/boxbuckshot.mdl", K_AMMO, 5, 20, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_crossbow", "models/items/crossbowrounds.mdl", K_AMMO, 6, 6, "BaseCombatCharacter.AmmoPickup"),
    ("item_rpg_round", "models/weapons/w_missile_closed.mdl", K_AMMO, 8, 1, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_smg1_grenade", "models/items/ar2_grenade.mdl", K_AMMO, 9, 1, "BaseCombatCharacter.AmmoPickup"),
    ("item_ammo_ar2_altfire", "models/items/combine_rifle_ammo01.mdl", K_AMMO, 10, 1, "BaseCombatCharacter.AmmoPickup"),
    ("item_box_srounds", "models/items/boxsrounds.mdl", K_AMMO, 1, 20, "BaseCombatCharacter.AmmoPickup"),
    ("item_box_mrounds", "models/items/boxmrounds.mdl", K_AMMO, 3, 45, "BaseCombatCharacter.AmmoPickup"),
    ("item_box_lrounds", "models/items/combine_rifle_cartridge01.mdl", K_AMMO, 4, 20, "BaseCombatCharacter.AmmoPickup"),
    ("item_ar2_grenade", "models/items/ar2_grenade.mdl", K_AMMO, 9, 1, "BaseCombatCharacter.AmmoPickup"),
]
WEAP_FMT = ">IhhBBhBB2xfff8h4h"  # 52 (C layout: floats 4-aligned)
ITEM_FMT = ">IhBBhh"            # 12


class _FakeBSP:
    """Just enough of a map for the NPC converter: one generic_actor per model."""

    def __init__(self, paths):
        self.entities = [{"classname": "generic_actor", "model": p, "origin": "0 0 0",
                          "_pairs": [("classname", "generic_actor"), ("model", p)]} for p in paths]


def weapon_sounds(fs, cls):
    try:
        t = fs.read(f"scripts/{cls}.txt").decode("latin-1")
    except Exception:  # noqa: BLE001
        t = None
    out = {}
    if not t:
        return out
    for _, body in parse_kv(t):
        if isinstance(body, list):
            for k, v in body:
                if k.lower() == "sounddata" and isinstance(v, list):
                    out.update({a.lower(): b for a, b in v if isinstance(b, str)})
    return out


def build(game_dir, fs, out_dir, shared, log=print):
    vm_paths = [f"models/weapons/{w[1]}.mdl" for w in WEAPONS]
    wm_paths = [f"models/weapons/{w[2]}.mdl" for w in WEAPONS]
    it_paths = [i[1] for i in ITEMS] + [h[1] for h in NPC_HELD]
    paths = []
    for p in vm_paths + wm_paths + it_paths:
        if p not in paths and fs.exists(p):
            paths.append(p)
    mats = []
    npcs = NPCs.__new__(NPCs)
    import npcs as npcmod
    old_budget, old_hero = npcmod.NPC_VERT_BUDGET, npcmod.HERO_MODELS
    npcmod.NPC_VERT_BUDGET, npcmod.HERO_MODELS = 10 ** 9, ("weapons/", "items/", "healthvial")
    try:
        NPCs.__init__(npcs, _FakeBSP(paths), fs, mats, "weapons", shared.scripts, shared.captions, shared.scenes,
                      shared.bank, log)
    finally:
        npcmod.NPC_VERT_BUDGET, npcmod.HERO_MODELS = old_budget, old_hero
    # world models and items rest in their first sequence
    for p in wm_paths + it_paths:
        if p in npcs.skel_key:
            am = npcs.skels[npcs.skel_key[p]]
            if am.seqs:
                npcs.wanted_seqs.add(next(iter(am.seqs)))
    # every viewmodel animation the engine plays
    for p in vm_paths:
        if p in npcs.skel_key:
            am = npcs.skels[npcs.skel_key[p]]
            for acts in VM_ACTS:
                for a in acts:
                    lab = am.find(a)
                    if lab:
                        npcs.wanted_seqs.add(lab)
                        break

    pool = bytearray(b"\0")
    pool_index = {"": 0}

    def s(x):
        if x not in pool_index:
            pool_index[x] = len(pool)
            pool.extend(x.encode("latin-1", "replace") + b"\0")
        return pool_index[x]

    chunks = []

    def chunk(tag, payload):
        chunks.append((tag, payload))

    # weapon and item sounds go into the same sound table as the NPC chunks' sounds
    names = set()
    for w in WEAPONS:
        names.update(weapon_sounds(fs, w[0]).get(k, "") for k in SOUND_KEYS)
    names.update(i[5] for i in ITEMS)
    names.add("Player.PickupWeapon")
    names.update(EXTRA_SOUNDS)
    from surfaces import Surfaces
    surf = Surfaces(fs)
    steps = surf.steps()
    for l, r in steps:
        names.update((l, r))
    impacts = surf.impacts()
    for row in impacts:
        names.update(row)
    sentences = sentence_words(fs)
    for ws in sentences.values():
        names.update(w for w in ws if w != PAUSE)
    names.discard("")
    npcs.extra_sound_names = sorted(names)
    npcs.chunks(chunk, s, lambda pos: (128, 128, 128))
    wsnd = npcs.extra_sound_index

    def vm_seq(p, acts):
        if p not in npcs.skel_key:
            return -1
        am = npcs.skels[npcs.skel_key[p]]
        idx = npcs.seq_index.get(npcs.skel_key[p], {})
        for a in acts:
            lab = am.find(a)
            if lab and lab in idx:
                return idx[lab]
        return -1

    mi = npcs.model_index
    weap = []
    for k, (cls, vm, wm, slot, clip, ammo, dmg, rate, pellets, spread, flags) in enumerate(WEAPONS):
        vp, wp = f"models/weapons/{vm}.mdl", f"models/weapons/{wm}.mdl"
        sd = weapon_sounds(fs, cls)
        seqs = [vm_seq(vp, acts) for acts in VM_ACTS]
        snds = [wsnd.get(sd.get(key, ""), -1) for key in SOUND_KEYS]
        weap.append(struct.pack(WEAP_FMT, s(cls), mi.get(vp, -1), mi.get(wp, -1), slot, flags, clip,
                                AMMO.index(ammo) if ammo in AMMO else 0, pellets, float(dmg), float(rate),
                                float(spread), *seqs, *snds))
    items = []
    for cls, path, kind, arg, amount, snd in ITEMS:
        items.append(struct.pack(ITEM_FMT, s(cls), mi.get(path, -1), kind, arg, amount, wsnd.get(snd, -1)))
    for cls, path in NPC_HELD:
        items.append(struct.pack(ITEM_FMT, s(cls), mi.get(path, -1), K_NPCHELD, 0, 0, -1))
    # weapons lying in the world are items too
    for k, w in enumerate(WEAPONS):
        items.append(struct.pack(ITEM_FMT, s(w[0]), mi.get(f"models/weapons/{w[2]}.mdl", -1), K_WEAPON, k, 0,
                                 wsnd.get("Player.PickupWeapon", -1)))
    chunk(b"WEAP", struct.pack(">I", len(weap)) + b"".join(weap))
    # WICN: per weapon (WEAP order), its HUD icon glyphs in halflife2.ttf (font_icons.bin): the weapon,
    # its ammo, its secondary ammo (the scripts' TextureData "weapon" / "ammo" / "ammo2"; 0 = none)
    def icon_chars(cls):
        raw = fs.read(f"scripts/{cls}.txt")
        t = raw.decode("latin-1") if raw else ""
        out = []
        for key in ("weapon", "ammo", "ammo2", "crosshair"):
            m = re.search(r'"%s"\s*\{[^{}]*?"character"\s*"(.)"' % key, t)
            out.append(ord(m.group(1)) if m else 0)
        return out
    # (4th: the weapon's crosshair, a glyph of the Crosshairs font, halflife2.ttf)
    chunk(b"WICN", b"".join(bytes(icon_chars(w[0])) for w in WEAPONS))
    chunk(b"ITEM", struct.pack(">I", len(items)) + b"".join(items))
    chunk(b"WSND", b"".join(struct.pack(">h", wsnd.get(n, -1)) for n in EXTRA_SOUNDS))
    # HSEN: per suit sentence, u16 word count then s16 sound index per word (-2 = pause)
    hsen = bytearray(struct.pack(">H", len(SUIT_SENTENCES)))
    nwords = 0
    for name in SUIT_SENTENCES:
        ws = sentences.get(name.upper(), [])
        ids = [-2 if w == PAUSE else wsnd.get(w, -1) for w in ws][:255]
        ids = [i for i in ids if i != -1]
        nwords += len(ids)
        hsen += struct.pack(">H", len(ids)) + b"".join(struct.pack(">h", i) for i in ids)
    chunk(b"HSEN", bytes(hsen))
    # STEP: u16 count, u16 default surface, then per surface id (tools/surfaces.py order) left and right sound
    chunk(b"STEP", struct.pack(">HH", len(steps), surf.id_of("default")) + b"".join(struct.pack(">hh", wsnd.get(l, -1), wsnd.get(r, -1))
                                                          for l, r in steps))
    # SURF: u16 count, default surface, flesh surface, pad; then per surface id: impact hard, impact soft,
    # bullet impact, scrape (sound indices)
    chunk(b"SURF", struct.pack(">HHHH", len(impacts), surf.id_of("default"), surf.id_of("flesh"), 0) + b"".join(struct.pack(">4h", *[wsnd.get(x, -1) for x in row])
                                                              for row in impacts))
    # SMAT: u16 count, then per surface id its gamematerial letter (bullet decals and impact effects)
    smat = surf.materials()
    chunk(b"SMAT", struct.pack(">H", len(smat)) + bytes(smat))
    log(f"  footsteps: {len(steps)} surfaces, {sum(1 for l, r in steps if wsnd.get(l, -1) >= 0)} with sounds")
    log(f"  suit voice: {sum(1 for n in SUIT_SENTENCES if sentences.get(n.upper()))} sentences, {nwords} words")
    chunk(b"AMMX", struct.pack(f">{len(AMMO_MAX)}h", *AMMO_MAX))

    # textures for every model material
    want = {}
    for m in mats:
        if m.kind in (KIND_NODRAW, KIND_SKY) or not m.basetexture:
            continue
        want[m.basetexture] = {KIND_ALPHATEST: "punch", KIND_TRANSLUCENT: "alpha",
                               KIND_ADDITIVE: "alpha"}.get(m.kind, "opaque")
    tex_list, tex_index = [checker()], {}
    for path, mode in want.items():
        raw = fs.read(f"materials/{path}.vtf")
        if raw is None:
            continue
        try:
            t = build_texture(VTF(raw), mode, 256)
        except Exception as ex:  # noqa: BLE001
            log(f"  warn: {path}: {ex}")
            continue
        tex_index[path] = len(tex_list)
        tex_list.append(t)
    # FXTX: u16 count, then per effect slot its texture index (-1: the game data doesn't have it)
    fx_idx = []
    for path, mode, cap, frame in FX_TEXTURES:
        raw = fs.read(f"materials/{path}.vtf")
        t = None
        if raw is not None:
            try:
                v = VTF(raw)
                if frame < max(1, getattr(v, "frames", 1)):
                    t = build_texture(v, mode, cap, frame)
            except Exception as ex:  # noqa: BLE001
                log(f"  warn: {path}: {ex}")
        if t is None:
            fx_idx.append(-1)
            continue
        t.clamp = True
        fx_idx.append(len(tex_list))
        tex_list.append(t)
    chunk(b"FXTX", struct.pack(">H", len(fx_idx)) + b"".join(struct.pack(">h", i) for i in fx_idx))
    log(f"  effects: {sum(1 for i in fx_idx if i >= 0)} of {len(fx_idx)} textures")
    recs = [(tex_index.get(m.basetexture, 0) if m.basetexture else -1, -1, m.kind, *m.color) for m in mats]
    texdata, texinfo = bytearray(), bytearray(struct.pack(">I", len(tex_list)))
    offs = []
    for t in tex_list:
        offs.append(len(texdata))
        texdata += t.data
        texdata += b"\0" * (align(len(texdata)) - len(texdata))
    for t, off in zip(tex_list, offs):
        texinfo += struct.pack(">HHBBBBII", t.w, t.h, t.fmt, t.levels, 1 if t.clamp else 0, 0, off, len(t.data))
    chunk(b"TEXI", bytes(texinfo))
    chunk(b"TEXD", bytes(texdata))
    chunk(b"MATS", struct.pack(">I", len(recs)) + b"".join(struct.pack(">hhBBBB", *r) for r in recs))
    chunk(b"ESTR", bytes(pool))

    out = bytearray(align(16 + 12 * len(chunks)))
    struct.pack_into(">4sII", out, 0, b"HGM1", PACK_VERSION, len(chunks))
    for i, (tag, payload) in enumerate(chunks):
        struct.pack_into(">4sII", out, 16 + i * 12, tag, len(out), len(payload))
        out += payload
        out += b"\0" * (align(len(out)) - len(out))
    path = os.path.join(out_dir, "weapons.hgm")
    open(path, "wb").write(out)
    shared.bank.save()
    log(f"weapons: {len(weap)} weapons, {len(items)} items, {len(mi)} models, {len(tex_list)} textures, "
        f"{len(out) / 1048576:.2f} MB")
