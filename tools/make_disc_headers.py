#!/usr/bin/env python3
"""Writes sys/boot.bin and sys/bi2.bin for an hl2gc disc folder (Dolphin's extracted-disc layout).
Dolphin fills in the DOL and file-table offsets itself when it builds the virtual disc."""
import os
import struct
import sys


def main(out_dir):
    sys_dir = os.path.join(out_dir, "sys")
    os.makedirs(sys_dir, exist_ok=True)
    boot = bytearray(0x440)
    boot[0:6] = b"GH2E01"[:4] + b"HB"         # game code GH2E (homebrew, NTSC-U), maker HB
    struct.pack_into(">I", boot, 0x1C, 0xC2339F3D)  # GameCube disc magic
    name = b"LambdaGC - Half-Life 2 GameCube engine"
    boot[0x20:0x20 + len(name)] = name
    with open(os.path.join(sys_dir, "boot.bin"), "wb") as f:
        f.write(boot)
    bi2 = bytearray(0x2000)
    struct.pack_into(">I", bi2, 0x04, 0x01800000)  # simulated memory size: 24 MB
    struct.pack_into(">I", bi2, 0x18, 1)           # region: NTSC-U
    with open(os.path.join(sys_dir, "bi2.bin"), "wb") as f:
        f.write(bi2)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "disc")
