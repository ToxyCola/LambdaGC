"""Encoders for GameCube GX texture formats (CMPR, RGB565, RGB5A3, I8), all big-endian and tiled."""
import numpy as np

GX_TF_I8 = 0x1
GX_TF_IA8 = 0x3
GX_TF_RGB565 = 0x4
GX_TF_RGB5A3 = 0x5
GX_TF_RGBA8 = 0x6
GX_TF_CMPR = 0xE

_REV2 = np.array([sum(((b >> (2 * i)) & 3) << (6 - 2 * i) for i in range(4)) for b in range(256)], np.uint8)


def dxt1_to_cmpr(raw, w, h):
    """Losslessly re-pack DXT1 blocks (w,h multiples of 8) into GX CMPR tile order."""
    bw, bh = w // 4, h // 4
    b = np.frombuffer(raw, np.uint8).reshape(bh, bw, 8).copy()
    # endpoints to big-endian
    b[..., [0, 1, 2, 3]] = b[..., [1, 0, 3, 2]]
    # index bytes: pixel 0 in the top bits on GX
    b[..., 4:8] = _REV2[b[..., 4:8]]
    # (bh/2, 2, bw/2, 2, 8) -> tiles (ty, tx, sy, sx, 8)
    t = b.reshape(bh // 2, 2, bw // 2, 2, 8).transpose(0, 2, 1, 3, 4)
    return t.tobytes()


def _blocks(img):
    h, w, c = img.shape
    return img.reshape(h // 4, 4, w // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, 16, c)


def _to565(rgb):
    rgb = rgb.astype(np.int32)
    return ((rgb[..., 0] * 31 + 127) // 255 << 11) | ((rgb[..., 1] * 63 + 127) // 255 << 5) | ((rgb[..., 2] * 31 + 127) // 255)


def _from565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def encode_cmpr(rgba, punch_alpha=False):
    """Simple bounding-box DXT1 encoder -> GX CMPR. rgba (h,w,4) with h,w multiples of 8."""
    h, w, _ = rgba.shape
    bl = _blocks(rgba).astype(np.int32)  # (bh,bw,16,4)
    rgb = bl[..., :3]
    transparent = (bl[..., 3] < 128) if punch_alpha else np.zeros(bl.shape[:3], bool)
    opaque_mask = ~transparent
    big = np.where(opaque_mask[..., None], rgb, 0)
    mx = np.where(opaque_mask[..., None], rgb, -1).max(2)
    mn = np.where(opaque_mask[..., None], rgb, 999).min(2)
    any_opaque = opaque_mask.any(2)
    mx = np.where(any_opaque[..., None], mx, 0)
    mn = np.where(any_opaque[..., None], mn, 0)
    # inset the bounding box slightly (standard trick to reduce error)
    inset = (mx - mn) // 16
    mx = np.clip(mx - inset, 0, 255)
    mn = np.clip(mn + inset, 0, 255)
    c0 = _to565(mx)
    c1 = _to565(mn)
    has_trans = transparent.any(2)
    # 4-colour mode requires c0 > c1; 3-colour+transparent requires c0 <= c1
    swap4 = (~has_trans) & (c0 < c1)
    c0, c1 = np.where(swap4, c1, c0), np.where(swap4, c0, c1)
    eq4 = (~has_trans) & (c0 == c1)
    swap3 = has_trans & (c0 > c1)
    c0, c1 = np.where(swap3, c1, c0), np.where(swap3, c0, c1)
    p0, p1 = _from565(c0), _from565(c1)
    four = ~has_trans
    p2 = np.where(four[..., None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[..., None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 2)  # bh,bw,4,3
    d = ((rgb[:, :, :, None, :] - pal[:, :, None, :, :]) ** 2).sum(-1)  # bh,bw,16,4
    d[..., 3] = np.where(four[..., None], d[..., 3], 1 << 30)
    idx = d.argmin(-1)
    idx = np.where(transparent, 3, idx)
    idx = np.where(eq4[..., None], 0, idx)
    out = np.zeros(bl.shape[:2] + (8,), np.uint8)
    out[..., 0] = c0 >> 8
    out[..., 1] = c0 & 255
    out[..., 2] = c1 >> 8
    out[..., 3] = c1 & 255
    rows = idx.reshape(idx.shape[:2] + (4, 4))
    out[..., 4:8] = (rows[..., 0] << 6) | (rows[..., 1] << 4) | (rows[..., 2] << 2) | rows[..., 3]
    bh, bw = out.shape[:2]
    return out.reshape(bh // 2, 2, bw // 2, 2, 8).transpose(0, 2, 1, 3, 4).tobytes()


def _tile4(px16, w, h):
    """px16: (h,w) uint16 -> 4x4 tiled big-endian bytes"""
    t = px16.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3)
    return t.astype(">u2").tobytes()


def encode_rgb565(rgba):
    h, w, _ = rgba.shape
    return _tile4(_to565(rgba[..., :3]).astype(np.uint16), w, h)


def encode_rgb5a3(rgba):
    h, w, _ = rgba.shape
    p = rgba.astype(np.uint32)
    opaque = p[..., 3] >= 0xE0
    v_op = 0x8000 | ((p[..., 0] >> 3) << 10) | ((p[..., 1] >> 3) << 5) | (p[..., 2] >> 3)
    v_tr = ((p[..., 3] >> 5) << 12) | ((p[..., 0] >> 4) << 8) | ((p[..., 1] >> 4) << 4) | (p[..., 2] >> 4)
    return _tile4(np.where(opaque, v_op, v_tr).astype(np.uint16), w, h)


def resize_box(rgba, w, h):
    """Box-filter downscale by an integer factor (sizes are powers of two)."""
    H, W, C = rgba.shape
    if (W, H) == (w, h):
        return rgba
    if W < w or H < h:  # upscale tiny textures (nearest) along the short axis first
        rgba = np.repeat(np.repeat(rgba, max(1, -(-h // H)), 0), max(1, -(-w // W)), 1)
        H, W, C = rgba.shape
    fx, fy = max(1, W // w), max(1, H // h)
    img = rgba[: h * fy, : w * fx].astype(np.float32)
    img = img.reshape(h, fy, w, fx, C).mean((1, 3))
    return np.clip(img + 0.5, 0, 255).astype(np.uint8)


def pot_floor(x):
    p = 1
    while p * 2 <= x:
        p *= 2
    return p


def encode_ia8(a, i):
    """two 8-bit planes (h,w) -> IA8 tiles: each texel is alpha then intensity"""
    h, w = a.shape
    return _tile4((a.astype(np.uint16) << 8 | i.astype(np.uint16)), w, h)
