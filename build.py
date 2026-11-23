#!/usr/bin/env python3
"""LambdaGC builder: turns your own Steam install of Half-Life 2 (the steam_legacy branch) into a GameCube
disc image, LambdaGC.iso.

  1. finds Half-Life 2 (or takes --hl2 "<...>/steamapps/common/Half-Life 2")
  2. checks it's on the steam_legacy beta branch
  3. converts the maps, models, sounds and fonts into disc/files/hl2gc  (about 20-40 minutes)
  4. compiles the engine and the disc's apploader with devkitPro (devkitPPC + libogc)
  5. writes LambdaGC.iso

Nothing from the game is in this repository: every player converts their own copy.

usage: python build.py [--hl2 PATH] [--skip-convert] [--skip-engine] [--skip-branch-check]
                       [--windows-fonts C:\\Windows\\Fonts] [--maps d1_trainstation_01 ...] [--out LambdaGC.iso]
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "tools")
DISC = os.path.join(HERE, "disc")
HL2_APPID = "220"


def say(msg):
    print(f"[LambdaGC] {msg}", flush=True)


def fail(msg):
    print(f"\n[LambdaGC] ERROR: {msg}\n", file=sys.stderr, flush=True)
    sys.exit(1)


# ---------------------------------------------------------------- finding Half-Life 2
def steam_roots():
    roots = []
    if sys.platform == "win32":
        try:
            import winreg
            for hive, key, val in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                                   (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                                   (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath")):
                try:
                    with winreg.OpenKey(hive, key) as k:
                        roots.append(winreg.QueryValueEx(k, val)[0])
                except OSError:
                    pass
        except ImportError:
            pass
        roots += [r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"]
    else:
        home = os.path.expanduser("~")
        roots += [os.path.join(home, ".steam", "steam"), os.path.join(home, ".local", "share", "Steam"),
                  os.path.join(home, "Library", "Application Support", "Steam"),
                  os.path.join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam")]
    out = []
    for r in roots:
        r = os.path.normpath(r)
        if os.path.isdir(r) and r not in out:
            out.append(r)
    return out


def steam_libraries():
    libs = []
    for root in steam_roots():
        libs.append(root)
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            with open(vdf, encoding="utf-8", errors="replace") as f:
                for m in re.finditer(r'"path"\s+"([^"]+)"', f.read()):
                    libs.append(os.path.normpath(m.group(1).replace("\\\\", "\\")))
    seen = []
    for lib in libs:
        if lib not in seen and os.path.isdir(lib):
            seen.append(lib)
    return seen


def find_hl2():
    for lib in steam_libraries():
        acf = os.path.join(lib, "steamapps", f"appmanifest_{HL2_APPID}.acf")
        if os.path.isfile(acf):
            with open(acf, encoding="utf-8", errors="replace") as f:
                m = re.search(r'"installdir"\s+"([^"]+)"', f.read())
            game = os.path.join(lib, "steamapps", "common", m.group(1) if m else "Half-Life 2")
            if os.path.isdir(game):
                return game, acf
    return None, None


def manifest_for(game_root):
    steamapps = os.path.dirname(os.path.dirname(os.path.normpath(game_root)))
    acf = os.path.join(steamapps, f"appmanifest_{HL2_APPID}.acf")
    return acf if os.path.isfile(acf) else None


def branch_of(acf):
    with open(acf, encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = re.search(r'"BetaKey"\s+"([^"]*)"', text)
    return m.group(1) if m else ""


# ---------------------------------------------------------------- python packages, ffmpeg
def check_python():
    if sys.version_info < (3, 8):
        fail("Python 3.8 or newer is needed (https://www.python.org/downloads/).")
    missing = []
    for mod in ("numpy", "scipy", "PIL"):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    if missing:
        say(f"installing Python packages ({', '.join(missing)}) from tools/requirements.txt ...")
        r = subprocess.run([sys.executable, "-m", "pip", "install", "--user", "-r",
                            os.path.join(TOOLS, "requirements.txt")])
        if r.returncode != 0:
            fail("pip couldn't install numpy, scipy and pillow. Run:\n"
                 f"  {sys.executable} -m pip install -r tools/requirements.txt")
    if not shutil.which("ffmpeg"):
        say("note: ffmpeg isn't on the PATH, so the startup movie (intro.bin) is skipped. "
            "Install ffmpeg (https://ffmpeg.org/download.html) to include it.")


# ---------------------------------------------------------------- devkitPro
def devkitpro():
    cands = []
    env = os.environ.get("DEVKITPRO", "")
    if env:
        cands.append(env)
        if env.startswith("/opt/") and sys.platform == "win32":
            cands.append(r"C:\devkitPro")
    cands += [r"C:\devkitPro", "/opt/devkitpro"]
    exe = ".exe" if sys.platform == "win32" else ""
    for c in cands:
        gcc = os.path.join(c, "devkitPPC", "bin", "powerpc-eabi-gcc" + exe)
        if os.path.isfile(gcc) and os.path.isdir(os.path.join(c, "libogc", "include")):
            return c, exe
    fail("devkitPro (devkitPPC + libogc) wasn't found. Install it from "
         "https://github.com/devkitPro/installer/releases and tick \"GameCube development\" "
         "(or on Linux/macOS: dkp-pacman -S gamecube-dev), or set DEVKITPRO.")


def run(cmd):
    r = subprocess.run(cmd)
    if r.returncode != 0:
        fail("command failed: " + " ".join(cmd))


def build_engine(dkp, exe):
    bin_ = os.path.join(dkp, "devkitPPC", "bin")
    gcc = os.path.join(bin_, "powerpc-eabi-gcc" + exe)
    objcopy = os.path.join(bin_, "powerpc-eabi-objcopy" + exe)
    elf2dol = os.path.join(dkp, "tools", "bin", "elf2dol" + exe)
    if not os.path.isfile(elf2dol):
        fail(f"elf2dol wasn't found at {elf2dol} (it comes with devkitPro's gamecube-tools).")
    ogc_inc, ogc_lib = os.path.join(dkp, "libogc", "include"), os.path.join(dkp, "libogc", "lib", "cube")
    m = ["-mogc", "-mcpu=750", "-meabi", "-mhard-float"]
    src = os.path.join(HERE, "engine", "source")
    out = os.path.join(HERE, "engine", "build")
    os.makedirs(out, exist_ok=True)
    objs = []
    for c in sorted(glob.glob(os.path.join(src, "*.c"))):
        o = os.path.join(out, os.path.basename(c)[:-2] + ".o")
        print("  cc", os.path.basename(c), flush=True)
        run([gcc, "-O2", "-Wall", "-std=gnu11", "-ffast-math", "-DGEKKO", *m, "-I" + src, "-I" + ogc_inc,
             "-c", c, "-o", o])
        objs.append(o)
    elf = os.path.join(HERE, "engine", "hl2gc.elf")
    dol = os.path.join(HERE, "engine", "hl2gc.dol")
    run([gcc, *objs, *m, "-L" + ogc_lib, "-lfat", "-lasnd", "-logc", "-lm",
         "-Wl,-Map=" + os.path.join(HERE, "engine", "hl2gc.map"), "-o", elf])
    run([elf2dol, elf, dol])
    # the disc's apploader
    ap = os.path.join(HERE, "engine", "apploader")
    a = ["-O2", "-mcpu=750", "-meabi", "-msoft-float", "-G0", "-msdata=none", "-ffreestanding", "-fno-builtin",
         "-fno-pic", "-nostdlib", "-nostartfiles"]
    run([gcc, *a, "-c", os.path.join(ap, "apploader.c"), "-o", os.path.join(out, "apploader.o")])
    run([gcc, *a, "-c", os.path.join(ap, "header.S"), "-o", os.path.join(out, "aphdr.o")])
    run([gcc, *a, "-T", os.path.join(ap, "apploader.ld"), os.path.join(out, "aphdr.o"),
         os.path.join(out, "apploader.o"), "-o", os.path.join(out, "apploader.elf")])
    os.makedirs(os.path.join(DISC, "sys"), exist_ok=True)
    run([objcopy, "-O", "binary", os.path.join(out, "apploader.elf"), os.path.join(DISC, "sys", "apploader.img")])
    shutil.copyfile(dol, os.path.join(DISC, "sys", "main.dol"))


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Build LambdaGC.iso from your Half-Life 2 install.")
    ap.add_argument("--hl2", help='the "Half-Life 2" folder (steamapps/common/Half-Life 2); found automatically')
    ap.add_argument("--skip-convert", action="store_true", help="keep the already converted disc/files")
    ap.add_argument("--skip-engine", action="store_true", help="keep the already built engine")
    ap.add_argument("--skip-branch-check", action="store_true",
                    help="don't insist on the steam_legacy branch (conversion will probably fail)")
    ap.add_argument("--windows-fonts", help=r"e.g. C:\Windows\Fonts: the PC version's menu fonts")
    ap.add_argument("--maps", nargs="*", help="convert only these maps (testing)")
    ap.add_argument("--out", default=os.path.join(HERE, "LambdaGC.iso"))
    a = ap.parse_args()

    say("LambdaGC is discontinued. This builder is provided as-is.")
    check_python()
    dkp, exe = (None, None) if a.skip_engine else devkitpro()

    if not a.skip_convert:
        if a.hl2:
            game, acf = os.path.normpath(a.hl2), manifest_for(a.hl2)
        else:
            game, acf = find_hl2()
            if not game:
                fail("Half-Life 2 wasn't found in your Steam libraries. Install it, or pass "
                     '--hl2 "<...>/steamapps/common/Half-Life 2".')
        hl2 = os.path.join(game, "hl2")
        if not os.path.isdir(os.path.join(hl2, "maps")):
            fail(f"{hl2} doesn't look like Half-Life 2's game folder (no maps folder).")
        say(f"Half-Life 2: {game}")
        if not a.skip_branch_check:
            if not acf:
                fail("couldn't find Steam's appmanifest_220.acf to check the branch. If you're sure this is the "
                     "steam_legacy version, run again with --skip-branch-check.")
            br = branch_of(acf)
            if br != "steam_legacy":
                fail("Half-Life 2 must be on the steam_legacy beta branch (the pre-20th Anniversary version).\n"
                     "  In Steam: Half-Life 2 > Properties > Betas > choose \"steam_legacy\", let it download,\n"
                     f"  then run the builder again. (Current branch: {br or 'default'})")
            say("branch: steam_legacy")

        maps_out = os.path.join(DISC, "files", "hl2gc", "maps")
        os.makedirs(maps_out, exist_ok=True)
        say("converting the game (this takes a while) ...")
        cmd = [sys.executable, os.path.join(TOOLS, "convert_all.py"), hl2, maps_out,
               "--only", "d1_", "d2_", "d3_", "background"]
        if a.windows_fonts:
            cmd += ["--windows-fonts", a.windows_fonts]
        if a.maps:
            cmd += ["--maps", *a.maps]
        run(cmd)

    run([sys.executable, os.path.join(TOOLS, "make_disc_headers.py"), DISC])
    if not a.skip_engine:
        say("compiling the engine ...")
        build_engine(dkp, exe)
    if not os.path.isfile(os.path.join(DISC, "sys", "main.dol")):
        fail("no engine built yet (run without --skip-engine).")
    if not glob.glob(os.path.join(DISC, "files", "hl2gc", "maps", "*.hgm")):
        fail("no converted maps in disc/files/hl2gc/maps (run without --skip-convert).")
    say("writing the disc image ...")
    sys.path.insert(0, TOOLS)
    import make_iso
    make_iso.build(DISC, a.out)
    say(f"done: {a.out}")
    say("Dolphin: File > Open > LambdaGC.iso. Real hardware: copy disc/files/hl2gc to the SD card root and "
        "launch engine/hl2gc.dol with Swiss.")


if __name__ == "__main__":
    main()
