"""Global voice/sound bank for the disc (hl2gc/vo.bin).

Each sound is mono IMA ADPCM at 22050 Hz: a 12-byte header (s16 predictor, u8 step index, u8 flags
(1 = loops), u32 loop start sample, s16 predictor and u8 step index at the loop start, u8 0) followed by
the 4-bit codes, high nibble first. A WAV loops when it has a cue point, like in Source: from the cue
to the end. The bank grows as maps are converted; vo_index.json remembers
what is already inside so every map refers to the same copy of a shared line."""
import ctypes
import json
import os
import struct
import subprocess
import tempfile

import numpy as np

RATE = 22050
MAX_SECONDS = 600  # music runs several minutes; the engine streams long sounds from the disc
HEADER = 12

_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
         97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
         724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
         4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
         18500, 20350, 22385, 24623, 27086, 29794, 32767]
_IDX = [-1, -1, -1, -1, 2, 4, 6, 8]

_C_SRC = r"""
static const int STEP[89] = {%s};
static const int IDX[8] = {-1,-1,-1,-1,2,4,6,8};
void ima_encode(const short *in, int n, unsigned char *out) {
    int pred = 0, index = 0, i;
    for (i = 0; i < n; i++) {
        int step = STEP[index], diff = in[i] - pred, code = 0, vpdiff = step >> 3;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 1; vpdiff += step; }
        if (code & 8) pred -= vpdiff; else pred += vpdiff;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        index += IDX[code & 7];
        if (index < 0) index = 0; else if (index > 88) index = 88;
        if (i & 1) out[i >> 1] |= (unsigned char)code; else out[i >> 1] = (unsigned char)(code << 4);
    }
}
""" % ",".join(map(str, _STEP))

_lib = None


def _clib():
    global _lib
    if _lib is not None:
        return _lib or None
    try:
        d = os.path.join(tempfile.gettempdir(), "hl2gc_adpcm")
        os.makedirs(d, exist_ok=True)
        src, so = os.path.join(d, "ima.c"), os.path.join(d, "ima.so")
        if not os.path.exists(so):
            open(src, "w").write(_C_SRC)
            subprocess.run(["cc", "-O2", "-shared", "-fPIC", "-o", so, src], check=True, capture_output=True)
        lib = ctypes.CDLL(so)
        lib.ima_encode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
        _lib = lib
    except Exception:  # noqa: BLE001 (no compiler: pure Python fallback)
        _lib = False
    return _lib or None


def ima_encode(pcm):
    pcm = np.ascontiguousarray(pcm, dtype=np.int16)
    out = np.zeros((len(pcm) + 1) // 2, dtype=np.uint8)
    lib = _clib()
    if lib:
        lib.ima_encode(pcm.ctypes.data, len(pcm), out.ctypes.data)
        return out.tobytes()
    pred = index = 0
    for i, s in enumerate(pcm.tolist()):
        step, diff, code = _STEP[index], s - pred, 0
        vp = step >> 3
        if diff < 0:
            code, diff = 8, -diff
        for bit, sh in ((4, 0), (2, 1), (1, 2)):
            st = step >> sh
            if diff >= st:
                code |= bit
                diff -= st
                vp += st
        pred = max(-32768, min(32767, pred - vp if code & 8 else pred + vp))
        index = max(0, min(88, index + _IDX[code & 7]))
        out[i >> 1] |= code if i & 1 else code << 4
    return out.tobytes()


def ima_decode(data, n):
    """Reference decoder (mirrors the engine's) for tests."""
    pred = index = 0
    out = np.zeros(n, dtype=np.int16)
    for i in range(n):
        b = data[i >> 1]
        code = b & 15 if i & 1 else b >> 4
        step = _STEP[index]
        vp = step >> 3
        if code & 4:
            vp += step
        if code & 2:
            vp += step >> 1
        if code & 1:
            vp += step >> 2
        pred = max(-32768, min(32767, pred - vp if code & 8 else pred + vp))
        index = max(0, min(88, index + _IDX[code & 7]))
        out[i] = pred
    return out


def decode_audio(data):
    """WAV bytes (PCM8/16, or anything ffmpeg understands, e.g. MP3-in-WAV / MS-ADPCM) -> mono int16 @ RATE."""
    pcm = None
    if data[:4] == b"RIFF":
        p, fmt = 12, None
        while p + 8 <= len(data):
            cid, sz = struct.unpack_from("<4sI", data, p)
            if cid == b"fmt ":
                fmt = struct.unpack_from("<HHIIHH", data, p + 8)
            elif cid == b"data" and fmt:
                tag, ch, rate, _br, _ba, bits = fmt
                body = data[p + 8:p + 8 + sz]
                if tag == 1 and bits == 16:
                    a = np.frombuffer(body[: len(body) // 2 * 2], dtype="<i2").astype(np.float32)
                elif tag == 1 and bits == 8:
                    a = (np.frombuffer(body, dtype=np.uint8).astype(np.float32) - 128) * 256
                else:
                    break
                if ch > 1:
                    a = a[: len(a) // ch * ch].reshape(-1, ch).mean(1)
                pcm = _resample(a, rate)
                break
            p += 8 + sz + (sz & 1)
    if pcm is None:
        r = subprocess.run(["ffmpeg", "-v", "quiet", "-i", "pipe:0", "-f", "s16le", "-ac", "1", "-ar", str(RATE),
                            "pipe:1"], input=data, capture_output=True)
        if r.returncode != 0 or not r.stdout:
            return None
        pcm = np.frombuffer(r.stdout, dtype="<i2").astype(np.float32)
    pcm = pcm[: RATE * MAX_SECONDS]
    return np.clip(np.round(pcm), -32768, 32767).astype(np.int16)


def wav_loop_start(data):
    """Sample (at RATE) where a WAV's loop starts, from its cue point; None if it doesn't loop."""
    if not data or data[:4] != b"RIFF":
        return None
    p, rate, cue = 12, None, None
    while p + 8 <= len(data):
        cid, sz = struct.unpack_from("<4sI", data, p)
        if cid == b"fmt " and sz >= 8:
            rate = struct.unpack_from("<I", data, p + 12)[0]
        elif cid == b"cue " and sz >= 4:
            n = struct.unpack_from("<I", data, p + 8)[0]
            if n:
                cue = struct.unpack_from("<I", data, p + 12 + 20)[0]  # dwSampleOffset of the first point
        elif cid == b"smpl" and sz >= 36 and cue is None:
            # the sampler chunk's first loop (Source's ParseSamplerChunk reads it as the loop too: vehicle
            # engines, many ambient loops); dwStart of loop 0
            n = struct.unpack_from("<I", data, p + 8 + 28)[0]
            if n and sz >= 36 + 24:
                cue = struct.unpack_from("<I", data, p + 8 + 36 + 8)[0]
        p += 8 + sz + (sz & 1)
    if cue is None or not rate:
        return None
    return int(cue * RATE / rate)


def ima_state_at(data, k):
    """Decoder state (predictor, step index) before sample k."""
    pred = index = 0
    for i in range(k):
        b = data[i >> 1]
        code = b & 15 if i & 1 else b >> 4
        step = _STEP[index]
        vp = step >> 3
        if code & 4:
            vp += step
        if code & 2:
            vp += step >> 1
        if code & 1:
            vp += step >> 2
        pred = max(-32768, min(32767, pred - vp if code & 8 else pred + vp))
        index = max(0, min(88, index + _IDX[code & 7]))
    return pred, index


def _resample(a, rate):
    if rate == RATE:
        return a
    from scipy.signal import resample_poly
    from math import gcd
    g = gcd(int(rate), RATE)
    return resample_poly(a, RATE // g, int(rate) // g)


class SoundBank:
    def __init__(self, out_dir):
        self.bin_path = os.path.join(out_dir, "vo.bin")
        self.idx_path = os.path.join(out_dir, "vo_index.json")
        self.index = {}
        if os.path.exists(self.idx_path) and os.path.exists(self.bin_path):
            self.index = json.load(open(self.idx_path))
        elif os.path.exists(self.bin_path):
            os.remove(self.bin_path)

    def add(self, fs, path):
        """'sound/...' path -> (offset, size, samples) or None."""
        if path in self.index:
            return tuple(self.index[path]) if self.index[path] else None
        try:
            data = fs.read(path)
        except Exception:  # noqa: BLE001 (archive missing)
            data = None
        pcm = decode_audio(data) if data else None
        if pcm is None or len(pcm) == 0:
            self.index[path] = None
            return None
        codes = ima_encode(pcm)
        loop = wav_loop_start(data)
        flags, lp, lpred, lidx = 0, 0, 0, 0
        if loop is not None and loop < len(pcm) - 64:
            flags, lp = 1, loop
            lpred, lidx = ima_state_at(codes, lp) if lp else (0, 0)
        body = struct.pack(">hBBIhBx", 0, 0, flags, lp, lpred, lidx) + codes
        body += b"\0" * (-len(body) % 32)
        with open(self.bin_path, "ab") as f:
            off = f.tell()
            f.write(body)
        self.index[path] = [off, len(body), int(len(pcm))]
        return tuple(self.index[path])

    def add_pcm(self, key, pcm, loop_start=None):
        """Mono int16 PCM at RATE under a made-up key (menu sounds) -> (offset, size, samples)."""
        if key in self.index and self.index[key]:
            return tuple(self.index[key])
        codes = ima_encode(pcm)
        flags, lp, lpred, lidx = 0, 0, 0, 0
        if loop_start is not None:
            flags, lp = 1, loop_start
            lpred, lidx = ima_state_at(codes, lp) if lp else (0, 0)
        body = struct.pack(">hBBIhBx", 0, 0, flags, lp, lpred, lidx) + codes
        body += b"\0" * (-len(body) % 32)
        with open(self.bin_path, "ab") as f:
            off = f.tell()
            f.write(body)
        self.index[key] = [off, len(body), int(len(pcm))]
        return tuple(self.index[key])

    def save(self):
        json.dump(self.index, open(self.idx_path, "w"))
