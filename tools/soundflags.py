"""Marks looping sounds in a pack's sound table: bit 0 of a sound's bank offset (offsets are multiples
of 32) is set when the sound has loop points (the flag in its header in vo.bin). The engine keeps a
looping sound going when it's out of earshot or its voice is needed, instead of losing it."""
import struct
import sys


def patch(hgm_path, bank_path):
    d = bytearray(open(hgm_path, "rb").read())
    n = struct.unpack_from(">I", d, 8)[0]
    loops = total = 0
    with open(bank_path, "rb") as bank:
        for i in range(n):
            tag = bytes(d[16 + i * 12:20 + i * 12])
            off, size = struct.unpack_from(">II", d, 20 + i * 12)
            if tag != b"SNDS":
                continue
            cnt = struct.unpack_from(">I", d, off)[0]
            for k in range(cnt):
                o = off + 4 + k * 12
                so, sv, sl = struct.unpack_from(">III", d, o)
                so &= ~31
                if not (sv & 0xFFFFFF) or not (sl & 0xFFFFFF):
                    continue
                bank.seek(so + 3)
                fl = bank.read(1)
                total += 1
                if fl and fl[0] & 1:
                    so |= 1
                    loops += 1
                struct.pack_into(">I", d, o, so)
    open(hgm_path, "wb").write(d)
    return loops, total


if __name__ == "__main__":
    for p in sys.argv[2:]:
        print(p, "%d looping of %d sounds" % patch(p, sys.argv[1]))
