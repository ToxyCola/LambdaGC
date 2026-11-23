#!/usr/bin/env python3
"""Builds a GameCube disc image (.iso / GCM) from a disc folder (the same layout as make_iso.sh):
  <disc>/sys/boot.bin  <disc>/sys/bi2.bin  <disc>/sys/apploader.img  <disc>/sys/main.dol
  <disc>/files/...     (becomes the disc's file system)
Pure Python, so it runs anywhere. usage: make_iso.py <disc folder> <out.iso>"""
import os
import struct
import sys


def align(v, a):
    return (v + a - 1) // a * a


def build(disc, out):
    sysd = os.path.join(disc, "sys")
    for f in ("boot.bin", "bi2.bin", "apploader.img", "main.dol"):
        if not os.path.isfile(os.path.join(sysd, f)):
            raise SystemExit(f"make_iso: missing {os.path.join(sysd, f)}")

    # file system table: depth-first, entries sorted case-insensitively like Nintendo's tools
    entries = [{"dir": True, "name": "", "parent": 0, "next": 0}]

    def add_dir(path, parent):
        for name in sorted(os.listdir(path), key=lambda n: n.lower()):
            full = os.path.join(path, name)
            idx = len(entries)
            if os.path.isdir(full):
                entries.append({"dir": True, "name": name, "parent": parent, "next": 0})
                add_dir(full, idx)
                entries[idx]["next"] = len(entries)
            else:
                entries.append({"dir": False, "name": name, "path": full})

    files = os.path.join(disc, "files")
    if os.path.isdir(files):
        add_dir(files, 0)
    count = len(entries)
    entries[0]["next"] = count

    app = os.path.getsize(os.path.join(sysd, "apploader.img"))
    dol = os.path.getsize(os.path.join(sysd, "main.dol"))
    dol_off = align(0x2440 + app, 256)
    fst_off = align(dol_off + dol, 256)
    names = b"".join(e["name"].encode("ascii") + b"\0" for e in entries[1:])
    fst_size = count * 12 + len(names)
    pos = align(fst_off + fst_size, 32768)
    for e in entries:
        if not e["dir"]:
            e["off"], e["size"] = pos, os.path.getsize(e["path"])
            pos = align(pos + e["size"], 32768)
    total = pos

    fst = bytearray()
    nameoff = 0
    for i, e in enumerate(entries):
        if i == 0:
            fst += struct.pack(">III", 0x01000000, 0, count)
            continue
        if e["dir"]:
            fst += struct.pack(">III", (1 << 24) | nameoff, e["parent"], e["next"])
        else:
            fst += struct.pack(">III", nameoff, e["off"], e["size"])
        nameoff += len(e["name"]) + 1
    fst += names

    def put(f, src, off):
        f.seek(off)
        with open(src, "rb") as s:
            while True:
                b = s.read(1 << 20)
                if not b:
                    break
                f.write(b)

    with open(out, "wb") as f:
        f.truncate(total)
        put(f, os.path.join(sysd, "boot.bin"), 0)
        f.seek(0x420)
        f.write(struct.pack(">IIII", dol_off, fst_off, fst_size, fst_size))
        put(f, os.path.join(sysd, "bi2.bin"), 0x440)
        put(f, os.path.join(sysd, "apploader.img"), 0x2440)
        put(f, os.path.join(sysd, "main.dol"), dol_off)
        f.seek(fst_off)
        f.write(fst)
        for e in entries:
            if not e["dir"]:
                put(f, e["path"], e["off"])
    print(f"make_iso: {out}  {total // 1048576} MB, {count - 1} entries")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_iso.py <disc folder> <out.iso>")
    build(sys.argv[1], sys.argv[2])
