"""Reader for Valve VPK (v1/v2) archives, as shipped in a user's Half-Life 2 install."""
import os
import struct


class VPKEntry:
    __slots__ = ("path", "crc", "preload", "archive", "offset", "length")

    def __init__(self, path, crc, preload, archive, offset, length):
        self.path = path
        self.crc = crc
        self.preload = preload
        self.archive = archive
        self.offset = offset
        self.length = length

    @property
    def size(self):
        return len(self.preload) + self.length


class VPK:
    """Opens <name>_dir.vpk. Content archives <name>_NNN.vpk are read lazily from the same folder."""

    def __init__(self, dir_path):
        self.dir_path = dir_path
        self.base = dir_path[: -len("_dir.vpk")]
        self.entries = {}
        with open(dir_path, "rb") as f:
            data = f.read()
        sig, ver, tree_size = struct.unpack_from("<III", data, 0)
        if sig != 0x55AA1234:
            raise ValueError(f"{dir_path}: not a VPK")
        header = 12 if ver == 1 else 28
        self.data_start = header + tree_size
        self._dir_data = data
        p = header

        def cstr():
            nonlocal p
            e = data.index(b"\0", p)
            s = data[p:e].decode("latin-1")
            p = e + 1
            return s

        while True:
            ext = cstr()
            if not ext:
                break
            while True:
                path = cstr()
                if not path:
                    break
                while True:
                    name = cstr()
                    if not name:
                        break
                    crc, pre_len, arch, off, length, term = struct.unpack_from("<IHHIIH", data, p)
                    p += 18
                    preload = data[p : p + pre_len]
                    p += pre_len
                    full = name if path == " " else f"{path}/{name}"
                    if ext != " ":
                        full += "." + ext
                    self.entries[full.lower()] = VPKEntry(full, crc, preload, arch, off, length)

    def __contains__(self, path):
        return path.lower().replace("\\", "/") in self.entries

    def archive_path(self, idx):
        return f"{self.base}_{idx:03d}.vpk"

    def read(self, path):
        e = self.entries[path.lower().replace("\\", "/")]
        if e.length == 0:
            return e.preload
        if e.archive == 0x7FFF:
            start = self.data_start + e.offset
            return e.preload + self._dir_data[start : start + e.length]
        with open(self.archive_path(e.archive), "rb") as f:
            f.seek(e.offset)
            return e.preload + f.read(e.length)


class GameFS:
    """Search path like the engine's: loose files first, then VPKs, then the map's own pakfile."""

    def __init__(self, game_dir, vpk_names=("hl2_textures", "hl2_misc", "hl2_pak", "hl2_sound_vo_english", "hl2_sound_misc")):
        self.game_dir = game_dir
        self.vpks = []
        for n in vpk_names:
            p = os.path.join(game_dir, n + "_dir.vpk")
            if os.path.exists(p):
                self.vpks.append(VPK(p))
        self.pak = {}  # filled from the BSP's embedded zip

    def exists(self, path):
        path = path.lower().replace("\\", "/")
        if path in self.pak:
            return True
        if os.path.exists(os.path.join(self.game_dir, path)):
            return True
        return any(path in v for v in self.vpks)

    def locate(self, path):
        """Return (vpk, entry) or None — used to plan which archives must be present."""
        path = path.lower().replace("\\", "/")
        for v in self.vpks:
            if path in v.entries:
                return v, v.entries[path]
        return None

    def read(self, path):
        path = path.lower().replace("\\", "/")
        if path in self.pak:
            return self.pak[path]
        loose = os.path.join(self.game_dir, path)
        if os.path.exists(loose):
            with open(loose, "rb") as f:
                return f.read()
        for v in self.vpks:
            if path in v:
                return v.read(path)
        return None
