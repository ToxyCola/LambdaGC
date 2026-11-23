"""
The PC version's startup and loading visuals, for the GameCube.

intro.bin   - the startup movie (media/startupvids.txt, normally media/valve.bik): every frame as a GX CMPR
              texture, plus its soundtrack as 16-bit big-endian stereo PCM at 32 kHz (ASND's native rate).
              Decoded with ffmpeg, which reads Bink.
loading.bin - materials/console/loading (the glowing "Loading..." the engine draws in the corner during a
              level change) and console/startup_loading (the box shown while the game starts), RGB5A3.

intro.bin layout (big-endian):
  0  "HGV1"
  4  u16 width, u16 height, u32 frames, f32 fps
  16 u32 frame bytes (each frame padded to 32), u32 first frame offset
  24 u32 audio offset, u32 audio bytes, u32 audio rate, u32 channels
  40 .. 64 zero
"""
import os
import shutil
import struct
import subprocess

import numpy as np

from gxtex import encode_cmpr, encode_rgb5a3

VID_W, VID_H = 384, 288  # 4:3 like the 1024x768 movie; 55 KB a frame, 1.3 MB/s at 24 fps
RATE = 32000


def _ffmpeg():
    # the builder's own copy (LAMBDAGC_FFMPEG), one on the PATH, or the one imageio-ffmpeg carries
    exe = os.environ.get("LAMBDAGC_FFMPEG") or shutil.which("ffmpeg")
    if not exe:
        try:
            import imageio_ffmpeg
            exe = imageio_ffmpeg.get_ffmpeg_exe()
        except Exception:
            exe = None
    if not exe:
        raise RuntimeError("no ffmpeg found (needed to decode the startup movie)")
    return exe


def _startup_movie(game_dir):
    names = []
    p = os.path.join(game_dir, "media", "startupvids.txt")
    if os.path.exists(p):
        for line in open(p, "rb").read().decode("latin-1").splitlines():
            line = line.strip()
            if line and not line.startswith("//"):
                names.append(line)
    names.append("media/valve.bik")
    for n in names:
        base = os.path.splitext(os.path.join(game_dir, n.replace("\\", "/")))[0]
        for ext in (".bik", ".avi", ".mp4", ".webm"):  # Source plays the .bik when the .avi isn't there
            if os.path.exists(base + ext):
                return base + ext
    return None


def _probe_fps(ff, path):
    out = subprocess.run([ff, "-hide_banner", "-i", path], capture_output=True, text=True).stderr
    import re
    m = re.search(r"([\d.]+) fps", out)
    return float(m.group(1)) if m else 24.0


def build_intro(game_dir, out_dir, log=print):
    movie = _startup_movie(game_dir)
    if not movie:
        log("  intro: no startup movie in media/")
        return False
    ff = _ffmpeg()
    fps = _probe_fps(ff, movie)
    raw = subprocess.run([ff, "-v", "error", "-i", movie, "-vf", f"scale={VID_W}:{VID_H}:flags=lanczos",
                          "-f", "rawvideo", "-pix_fmt", "rgba", "-"], capture_output=True, check=True).stdout
    fsize = VID_W * VID_H * 4
    n = len(raw) // fsize
    pcm = subprocess.run([ff, "-v", "error", "-i", movie, "-vn", "-ac", "2", "-ar", str(RATE),
                          "-f", "s16be", "-"], capture_output=True).stdout
    frame_bytes = (VID_W * VID_H // 2 + 31) & ~31
    first = 64
    audio_off = first + n * frame_bytes
    path = os.path.join(out_dir, "intro.bin")
    with open(path, "wb") as f:
        hdr = struct.pack(">4sHHIfIIIIII", b"HGV1", VID_W, VID_H, n, fps, frame_bytes, first,
                          audio_off, len(pcm), RATE, 2)
        f.write(hdr + b"\0" * (first - len(hdr)))
        for i in range(n):
            img = np.frombuffer(raw, np.uint8, fsize, i * fsize).reshape(VID_H, VID_W, 4).copy()
            img[..., 3] = 255
            data = encode_cmpr(img)
            f.write(data + b"\0" * (frame_bytes - len(data)))
        f.write(pcm)
        f.write(b"\0" * (-f.tell() % 32))
    log(f"  intro: {os.path.basename(movie)}, {n} frames at {fps:.2f} fps, {len(pcm) / (RATE * 4):.1f} s of sound, "
        f"{os.path.getsize(path) / 1e6:.1f} MB")
    return True


def build_loading(fs, out_dir, log=print):
    """loading.bin: "HGLD", u16 count, then per image u16 w, u16 h, u32 offset; RGB5A3 data 32-aligned."""
    from vtf import VTF
    from PIL import Image
    images = []
    for name in ("console/loading", "console/startup_loading"):
        data = fs.read(f"materials/{name}.vtf")
        if not data:
            log(f"  loading: materials/{name}.vtf missing")
            rgba = np.zeros((64, 128, 4), np.uint8)
        else:
            v = VTF(data)
            rgba = np.array(Image.fromarray(v.rgba(0)).convert("RGBA"))
        h, w = rgba.shape[:2]
        w4, h4 = (w + 3) & ~3, (h + 3) & ~3
        if (w4, h4) != (w, h):
            pad = np.zeros((h4, w4, 4), np.uint8)
            pad[:h, :w] = rgba
            rgba = pad
        images.append((w4, h4, encode_rgb5a3(rgba)))
    head = struct.pack(">4sH2x", b"HGLD", len(images))
    off = 32
    table = b""
    blobs = b""
    for w, h, d in images:
        table += struct.pack(">HHI", w, h, off + len(blobs))
        blobs += d + b"\0" * (-len(d) % 32)
    out = head + table
    out += b"\0" * (32 - len(out))
    with open(os.path.join(out_dir, "loading.bin"), "wb") as f:
        f.write(out + blobs)
    log(f"  loading screens: {len(images)} images")
