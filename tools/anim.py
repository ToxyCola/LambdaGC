"""Skeletons and sequences from Source MDL files (v44-v49), including $includemodel files and
external .ani animation blocks. Animations are decoded into per-frame local bone rotations/positions."""
import math
import struct

import numpy as np

BONE_SIZE = 216
ANIMDESC_SIZE = 100
SEQDESC_SIZE = 212

ANIM_RAWPOS, ANIM_RAWROT, ANIM_ANIMPOS, ANIM_ANIMROT, ANIM_DELTA, ANIM_RAWROT2 = 1, 2, 4, 8, 16, 32
SEQ_LOOPING, SEQ_DELTA, SEQ_POST, SEQ_REALTIME = 0x1, 0x4, 0x10, 0x100
# mstudioautolayer_t flags
AL_POST, AL_SPLINE, AL_XFADE, AL_NOBLEND, AL_LOCAL, AL_POSE = 0x10, 0x40, 0x80, 0x200, 0x1000, 0x4000


def _cstr(d, o):
    e = d.index(b"\0", o)
    return d[o:e].decode("latin-1")


def _f16(h):
    return float(np.frombuffer(struct.pack("<H", h), dtype="<f2")[0])


def quat48(d, o):
    x, y, zw = struct.unpack_from("<HHH", d, o)
    fx = (x - 32768) / 32768.0
    fy = (y - 32768) / 32768.0
    fz = ((zw & 0x7FFF) - 16384) / 16384.0
    w = math.sqrt(max(0.0, 1.0 - fx * fx - fy * fy - fz * fz))
    if zw >> 15:
        w = -w
    return (fx, fy, fz, w)


def quat64(d, o):
    v = struct.unpack_from("<Q", d, o)[0]
    x = v & 0x1FFFFF
    y = (v >> 21) & 0x1FFFFF
    z = (v >> 42) & 0x1FFFFF
    wneg = v >> 63
    fx = (x - 1048576) / 1048576.5
    fy = (y - 1048576) / 1048576.5
    fz = (z - 1048576) / 1048576.5
    w = math.sqrt(max(0.0, 1.0 - fx * fx - fy * fy - fz * fz))
    return (fx, fy, fz, -w if wneg else w)


def angle_quat(x, y, z):
    """RadianEuler -> Quaternion (Source AngleQuaternion(RadianEuler))."""
    sr, cr = math.sin(x * 0.5), math.cos(x * 0.5)
    sp, cp = math.sin(y * 0.5), math.cos(y * 0.5)
    sy, cy = math.sin(z * 0.5), math.cos(z * 0.5)
    srXcp, crXsp = sr * cp, cr * sp
    crXcp, srXsp = cr * cp, sr * sp
    return (srXcp * cy - crXsp * sy, crXsp * cy + srXcp * sy, crXcp * sy - srXsp * cy, crXcp * cy + srXsp * sy)


def anim_value(d, o, frame, scale):
    """ExtractAnimValue (single value) for the RLE stream at offset o (o == 0 -> no data)."""
    if not o:
        return 0.0
    k = frame
    while True:
        valid, total = d[o], d[o + 1]
        if total == 0:
            return 0.0
        if total > k:
            break
        k -= total
        o += (valid + 1) * 2
    if valid > k:
        return struct.unpack_from("<h", d, o + (k + 1) * 2)[0] * scale
    return struct.unpack_from("<h", d, o + valid * 2)[0] * scale


class Bone:
    __slots__ = ("name", "parent", "pos", "quat", "rot", "posscale", "rotscale", "pose_to_bone")


def read_bones(d):
    n, idx = struct.unpack_from("<ii", d, 156)
    out = []
    for b in range(n):
        o = idx + b * BONE_SIZE
        bn = Bone()
        bn.name = _cstr(d, o + struct.unpack_from("<i", d, o)[0]).lower()
        bn.parent = struct.unpack_from("<i", d, o + 4)[0]
        bn.pos = struct.unpack_from("<3f", d, o + 32)
        bn.quat = struct.unpack_from("<4f", d, o + 44)
        bn.rot = struct.unpack_from("<3f", d, o + 60)
        bn.posscale = struct.unpack_from("<3f", d, o + 72)
        bn.rotscale = struct.unpack_from("<3f", d, o + 84)
        bn.pose_to_bone = np.array(struct.unpack_from("<12f", d, o + 96), dtype=np.float64).reshape(3, 4)
        out.append(bn)
    return out


class MDLFile:
    """One .mdl (plus its .ani if it has external anim blocks)."""

    def __init__(self, fs, path):
        self.path = path
        self.d = fs.read(path)
        if not self.d or self.d[:4] != b"IDST":
            raise ValueError(path)
        d = self.d
        self.version = struct.unpack_from("<i", d, 4)[0]
        self.bones = read_bones(d)
        self.bone_index = {b.name: i for i, b in enumerate(self.bones)}
        nb, ab_idx = struct.unpack_from("<ii", d, 352)
        self.ani = None
        self.animblocks = []
        if nb > 1:
            ani_name = _cstr(d, struct.unpack_from("<i", d, 348)[0]).replace("\\", "/").lower()
            try:
                self.ani = fs.read(ani_name)
            except Exception:  # noqa: BLE001
                self.ani = fs.read(path[:-4] + ".ani") if fs.exists(path[:-4] + ".ani") else None
            self.animblocks = [struct.unpack_from("<ii", d, ab_idx + i * 8) for i in range(nb)]
        nposes, pidx = struct.unpack_from("<ii", d, 300)
        self.poseparams = []
        for i in range(nposes):
            o = pidx + i * 20
            nm = _cstr(d, o + struct.unpack_from("<i", d, o)[0]).lower()
            self.poseparams.append((nm,) + struct.unpack_from("<i3f", d, o + 4))
        ninc, iidx = struct.unpack_from("<ii", d, 336)
        self.includes = []
        for i in range(ninc):
            o = iidx + i * 8
            self.includes.append(_cstr(d, o + struct.unpack_from("<i", d, o + 4)[0]).replace("\\", "/").lower())

    # --- anim descs
    def animdesc(self, i):
        d = self.d
        n, idx = struct.unpack_from("<ii", d, 180)
        o = idx + i * ANIMDESC_SIZE
        fps, flags, nframes, nmov, movidx = struct.unpack_from("<fiiii", d, o + 8)
        animblock, animindex = struct.unpack_from("<ii", d, o + 52)
        sectionindex, sectionframes = struct.unpack_from("<ii", d, o + 80)
        return dict(off=o, fps=fps, flags=flags, nframes=nframes, nmov=nmov, movidx=movidx,
                    animblock=animblock, animindex=animindex, sectionindex=sectionindex,
                    sectionframes=sectionframes, name=_cstr(d, o + struct.unpack_from("<i", d, o + 4)[0]))

    def _anim_ptr(self, ad, frame):
        """mstudioanimdesc_t::pAnim -> (buffer, offset, local frame) or None."""
        block, index = ad["animblock"], ad["animindex"]
        if ad["sectionframes"]:
            sf = ad["sectionframes"]
            if ad["nframes"] > sf and frame == ad["nframes"] - 1:
                frame, section = 0, ad["nframes"] // sf + 1
            else:
                section = frame // sf
                frame -= section * sf
            so = ad["off"] + ad["sectionindex"] + section * 8
            block, index = struct.unpack_from("<ii", self.d, so)
        if block == -1:
            return None
        if block == 0:
            return self.d, ad["off"] + index, frame
        if not self.ani or block >= len(self.animblocks):
            return None
        return self.ani, self.animblocks[block][0] + index, frame

    def sample(self, ad, frame, skeleton, raw_delta=False):
        """Local (pos[n,3], quat[n,4]) for one frame of this animdesc, mapped onto `skeleton` bones
        (a list of Bone); bones the animation doesn't touch keep the skeleton's rest pose."""
        n = len(skeleton)
        delta = bool(ad["flags"] & SEQ_DELTA)
        if raw_delta and delta:
            # a delta animation: bones it doesn't touch add nothing
            pos = np.zeros((n, 3))
            quat = np.tile(np.array([0.0, 0.0, 0.0, 1.0]), (n, 1))
        else:
            pos = np.array([b.pos for b in skeleton], dtype=np.float64)
            quat = np.array([b.quat for b in skeleton], dtype=np.float64)
        p = self._anim_ptr(ad, frame)
        if p is None:
            return pos, quat
        buf, o, fr = p
        guard = 0
        while guard < 512:
            guard += 1
            bone, flags, nxt = struct.unpack_from("<BBh", buf, o)
            src = self.bones[bone] if bone < len(self.bones) else None
            dst = skeleton_index(skeleton, src.name) if src else -1
            data = o + 4
            if src is not None and dst >= 0:
                b = src
                # rotation
                if flags & ANIM_RAWROT:
                    q = quat48(buf, data)
                elif flags & ANIM_RAWROT2:
                    q = quat64(buf, data)
                elif flags & ANIM_ANIMROT:
                    vp = data
                    offs = struct.unpack_from("<3h", buf, vp)
                    a = [anim_value(buf, vp + offs[i] if offs[i] > 0 else 0, fr, b.rotscale[i]) for i in range(3)]
                    if not (flags & ANIM_DELTA):
                        a = [a[i] + b.rot[i] for i in range(3)]
                    q = angle_quat(*a)
                else:
                    q = (0, 0, 0, 1) if flags & ANIM_DELTA else b.quat
                # position
                if flags & ANIM_RAWPOS:
                    po = data + (6 if flags & ANIM_RAWROT else 0) + (8 if flags & ANIM_RAWROT2 else 0)
                    v = [_f16(h) for h in struct.unpack_from("<3H", buf, po)]
                elif flags & ANIM_ANIMPOS:
                    vp = data + (6 if flags & ANIM_ANIMROT else 0)
                    offs = struct.unpack_from("<3h", buf, vp)
                    v = [anim_value(buf, vp + offs[i] if offs[i] > 0 else 0, fr, b.posscale[i]) for i in range(3)]
                    if not (flags & ANIM_DELTA):
                        v = [v[i] + b.pos[i] for i in range(3)]
                else:
                    v = (0, 0, 0) if flags & ANIM_DELTA else b.pos
                if not delta or raw_delta:
                    quat[dst] = q
                    pos[dst] = v
            if nxt == 0:
                break
            o += nxt
        return pos, quat

    def movement_speed(self, ad):
        """Average linear speed (units/sec) from the animation's movement blocks."""
        if not ad["nmov"] or ad["nframes"] < 2 or ad["fps"] <= 0:
            return 0.0
        o = ad["off"] + ad["movidx"] + (ad["nmov"] - 1) * 44
        px, py, pz = struct.unpack_from("<3f", self.d, o + 32)
        dur = (ad["nframes"] - 1) / ad["fps"]
        return math.hypot(px, py) / dur if dur > 0 else 0.0

    def movement_total(self, ad):
        """(dx, dy, dz, dyaw): where the animation's motion ends up (Studio_AnimMovement at the last frame:
        the last movement piece's cumulative position and yaw), in the model's frame"""
        if not ad["nmov"]:
            return (0.0, 0.0, 0.0, 0.0)
        o = ad["off"] + ad["movidx"] + (ad["nmov"] - 1) * 44
        ang = struct.unpack_from("<f", self.d, o + 16)[0]
        px, py, pz = struct.unpack_from("<3f", self.d, o + 32)
        return (px, py, pz, ang)

    # --- sequences
    def sequences(self):
        d = self.d
        n, idx = struct.unpack_from("<ii", d, 188)
        out = []
        for i in range(n):
            o = idx + i * SEQDESC_SIZE
            label = _cstr(d, o + struct.unpack_from("<i", d, o + 4)[0]).lower()
            act = _cstr(d, o + struct.unpack_from("<i", d, o + 8)[0]).upper()
            flags, activity, actweight = struct.unpack_from("<iii", d, o + 12)
            numblends, aii = struct.unpack_from("<ii", d, o + 56)
            gs = struct.unpack_from("<2i", d, o + 68)
            pidx = struct.unpack_from("<2i", d, o + 76)
            pstart = struct.unpack_from("<2f", d, o + 84)
            pend = struct.unpack_from("<2f", d, o + 92)
            grid = struct.unpack_from(f"<{max(1, gs[0] * gs[1])}h", d, o + aii)
            nev, evi = struct.unpack_from("<ii", d, o + 24)
            events = []
            for k in range(max(0, min(nev, 64))):
                eo = o + evi + k * 80
                cyc, evn, etype = struct.unpack_from("<fii", d, eo)
                opts = d[eo + 12:eo + 76].split(b"\0")[0].decode("latin-1", "replace")
                name = ""
                if etype & 0x400:  # AE_TYPE_NEWEVENTSYSTEM: the event is named
                    szi = struct.unpack_from("<i", d, eo + 76)[0]
                    if szi:
                        name = d[eo + szi:eo + szi + 64].split(b"\0")[0].decode("latin-1", "replace")
                events.append((cyc, evn, etype, opts, name))
            nal, ali, wli = struct.unpack_from("<iii", d, o + 148)
            weights = struct.unpack_from(f"<{len(self.bones)}f", d, o + wli) if wli else (1.0,) * len(self.bones)
            layers = [struct.unpack_from("<hhi4f", d, o + ali + k * 24) for k in range(max(0, nal))]
            out.append(dict(label=label, act=act, flags=flags, actweight=actweight, groupsize=gs,
                            paramindex=pidx, paramstart=pstart, paramend=pend, grid=grid,
                            weights=weights, layers=layers, events=events))
        return out

    def seq_anim(self, seq):
        """Pick the blend-grid entry closest to the neutral pose parameter (e.g. move_yaw = 0)."""
        gx, gy = max(1, seq["groupsize"][0]), max(1, seq["groupsize"][1])
        # Source starts every pose parameter at 0 (in its own units): pick the blend entry for 0 when 0 is in
        # the parameter's range, else its start. Taking the start for head_pitch / aim_pitch / body_pitch
        # (which run from looking up to looking down) left NPCs staring at the sky.
        def entry(axis, count):
            if count <= 1:
                return 0
            s_, e_ = seq["paramstart"][axis], seq["paramend"][axis]
            neutral = 0.0 if min(s_, e_) <= 0.0 <= max(s_, e_) else s_
            v = int(round((neutral - s_) / (e_ - s_) * (count - 1))) if e_ != s_ else 0
            return max(0, min(count - 1, v))
        x = entry(0, gx)
        y = entry(1, gy)
        return seq["grid"][y * gx + x]


def skeleton_index(skel, name):
    idx = getattr(skel, "_index", None)
    if idx is None:
        return next((i for i, b in enumerate(skel) if b.name == name), -1)
    return idx.get(name, -1)


class Skel(list):
    def __init__(self, bones):
        super().__init__(bones)
        self._index = {b.name: i for i, b in enumerate(bones)}


class AnimatedModel:
    """A model with its include models merged into one sequence table (virtual model)."""

    def __init__(self, fs, path):
        self.root = MDLFile(fs, path)
        self.skel = Skel(self.root.bones)
        self.files = [self.root]
        seen = {path}
        q = list(self.root.includes)
        while q:
            p = q.pop(0)
            if p in seen:
                continue
            seen.add(p)
            try:
                f = MDLFile(fs, p)
            except Exception:  # noqa: BLE001
                continue
            self.files.append(f)
            q += f.includes
        self.seqs = {}   # label -> (file, seq)
        self.acts = {}   # ACT_ name -> list of (weight, label)
        self.local_labels = {}  # file path -> labels by the file's own sequence index (autolayers use those)
        for f in self.files:
            fs_ = f.sequences()
            self.local_labels[f.path] = [x["label"] for x in fs_]
            for s in fs_:
                if s["label"] in self.seqs:
                    continue
                self.seqs[s["label"]] = (f, s)
                if s["act"]:
                    self.acts.setdefault(s["act"], []).append((s["actweight"], s["label"]))

    def find(self, name):
        n = name.lower()
        if n in self.seqs:
            return n
        a = name.upper()
        if a in self.acts:
            return max(self.acts[a])[1]
        return None

    def bake(self, label, max_fps=15.0, max_frames=240):
        """-> dict(fps, loop, speed, pos[F,n,3], quat[F,n,4]) local transforms, root yaw motion removed."""
        f, s = self.seqs[label]
        ai = f.seq_anim(s)
        ad = f.animdesc(ai)
        nf = max(1, ad["nframes"])
        fps = ad["fps"] if ad["fps"] > 0 else 30.0
        dur = (nf - 1) / fps if nf > 1 else 0.0
        out_fps = min(fps, max_fps)
        count = max(1, min(max_frames, int(round(dur * out_fps)) + 1))
        if count > 1:
            out_fps = (count - 1) / dur if dur > 0 else out_fps
        P = np.zeros((count, len(self.skel), 3))
        Q = np.zeros((count, len(self.skel), 4))
        for i in range(count):
            fr = min(nf - 1, int(round(i * fps / out_fps))) if count > 1 else 0
            P[i], Q[i] = f.sample(ad, fr, self.skel)
        return dict(fps=out_fps, loop=bool(s["flags"] & SEQ_LOOPING), speed=f.movement_speed(ad),
                    dur=dur, pos=P, quat=Q, delta=bool(s["flags"] & SEQ_DELTA))


    def pose_controls(self, names=("aim_yaw", "aim_pitch")):
        """The aiming pose parameters as bone rotations: Source blends a grid of animations by them (a camera's
        or turret's aim_yaw / aim_pitch, often delta layers); here each parameter turns the bones it moves through
        the grid's entries along its axis (evenly spaced from start to end), the other parameter at its neutral.
        -> [(param, start, end, bone, [quat per entry], delta)]"""
        out, done = [], set()
        for label, (f, s) in self.seqs.items():
            gx, gy = max(1, s["groupsize"][0]), max(1, s["groupsize"][1])
            for axis in (0, 1):
                pi, g = s["paramindex"][axis], (gx, gy)[axis]
                if pi < 0 or g < 2 or pi >= len(f.poseparams):
                    continue
                name = f.poseparams[pi][0]
                if name not in names or name in done:
                    continue

                def neutral(ax, count):
                    if count <= 1:
                        return 0
                    s_, e_ = s["paramstart"][ax], s["paramend"][ax]
                    nv = 0.0 if min(s_, e_) <= 0.0 <= max(s_, e_) else s_
                    return max(0, min(count - 1, int(round((nv - s_) / (e_ - s_) * (count - 1))) if e_ != s_ else 0))
                nx, ny = neutral(0, gx), neutral(1, gy)
                entries = [s["grid"][ny * gx + k] if axis == 0 else s["grid"][k * gx + nx] for k in range(g)]
                # a delta layer adds before the pose (Source's QuaternionSM) or, STUDIO_POST, after it (QuaternionMA)
                delta = (2 if s["flags"] & 0x10 else 1) if s["flags"] & SEQ_DELTA else 0
                try:
                    qs = [f.sample(f.animdesc(a_), 0, self.skel, raw_delta=bool(delta))[1] for a_ in entries]
                except Exception:  # noqa: BLE001
                    continue
                for bo in range(len(self.skel)):
                    moves = any(abs(float(np.dot(qs[0][bo], q[bo]))) < 0.9995 for q in qs[1:])
                    if moves:
                        out.append((name, s["paramstart"][axis], s["paramend"][axis], bo,
                                    [tuple(float(x) for x in q[bo]) for q in qs], delta))
                done.add(name)
        return out

    def layers(self, label):
        """Autolayers of a sequence as (sub label, flags, start, peak, tail, end). Layers driven by a pose
        parameter or local to IK aren't reproduced."""
        f, s = self.seqs[label]
        names = self.local_labels[f.path]
        out = []
        for iseq, _ipose, flags, start, peak, tail, end in s["layers"]:
            if flags & (AL_POSE | AL_LOCAL) or not (0 <= iseq < len(names)):
                continue
            sub = names[iseq]
            if sub in self.seqs and sub != label:
                out.append((sub, flags, start, peak, tail, end))
        return out

    def mask(self, label):
        """(skeleton bone, weight) for the bones the sequence's weight list lets it touch."""
        f, s = self.seqs[label]
        out = []
        for bi, w in enumerate(s["weights"]):
            if w > 0.0 and bi < len(f.bones):
                dst = skeleton_index(self.skel, f.bones[bi].name)
                if dst >= 0:
                    out.append((dst, min(1.0, w)))
        out.sort()
        return out

    def bake_masked(self, label, max_fps=15.0, max_frames=240):
        """Like bake(), but only the bones in the sequence's weight list, with delta animations kept as
        deltas, plus everything the engine needs to layer it the way Source's AccumulatePose does."""
        f, s = self.seqs[label]
        ai = f.seq_anim(s)
        ad = f.animdesc(ai)
        nf = max(1, ad["nframes"])
        fps = ad["fps"] if ad["fps"] > 0 else 30.0
        dur = (nf - 1) / fps if nf > 1 else 0.0
        out_fps = min(fps, max_fps)
        count = max(1, min(max_frames, int(round(dur * out_fps)) + 1))
        if count > 1:
            out_fps = (count - 1) / dur if dur > 0 else out_fps
        m = self.mask(label)
        idx = [b for b, _ in m]
        P = np.zeros((count, len(idx), 3))
        Q = np.zeros((count, len(idx), 4))
        for i in range(count):
            fr = min(nf - 1, int(round(i * (nf - 1) / (count - 1)))) if count > 1 else 0
            pp, qq = f.sample(ad, fr, self.skel, raw_delta=True)
            P[i], Q[i] = pp[idx], qq[idx]
        return dict(fps=out_fps, loop=bool(s["flags"] & SEQ_LOOPING), speed=f.movement_speed(ad), dur=dur,
                    move=f.movement_total(ad),
                    pos=P, quat=Q, delta=bool(s["flags"] & SEQ_DELTA), post=bool(s["flags"] & SEQ_POST),
                    realtime=bool(s["flags"] & SEQ_REALTIME), mask=m, layers=self.layers(label),
                    events=s.get("events", []),
                    rest_pos=np.array([b.pos for b in self.skel]), rest_quat=np.array([b.quat for b in self.skel]))


def quat_to_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def pose_matrices(skel, pos, quat):
    """Skinning matrices (bone world * poseToBone) for one frame: [n,3,4]."""
    n = len(skel)
    world = np.zeros((n, 3, 4))
    out = np.zeros((n, 3, 4))
    for i, b in enumerate(skel):
        local = np.zeros((3, 4))
        local[:, :3] = quat_to_mat(quat[i])
        local[:, 3] = pos[i]
        if b.parent >= 0:
            pw = world[b.parent]
            world[i, :, :3] = pw[:, :3] @ local[:, :3]
            world[i, :, 3] = pw[:, :3] @ local[:, 3] + pw[:, 3]
        else:
            world[i] = local
        ptb = b.pose_to_bone
        out[i, :, :3] = world[i, :, :3] @ ptb[:, :3]
        out[i, :, 3] = world[i, :, :3] @ ptb[:, 3] + world[i, :, 3]
    return out
