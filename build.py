#!/usr/bin/env python3
"""LambdaGC builder. Give it a Steam install of Half-Life 2 on the steam_legacy branch and it does the rest:

  1. sets up everything it needs in .tools/ next to this file (nothing is installed system-wide):
       - Python packages: numpy, scipy, pillow, zstandard, imageio-ffmpeg (ffmpeg for the startup movie)
       - devkitPro's GameCube toolchain (devkitPPC, libogc, libfat, gamecube-tools), downloaded from
         devkitPro's own package server and checked against its SHA-256 sums
  2. finds Half-Life 2 in your Steam libraries and checks it's on the steam_legacy branch
  3. converts the maps, models, sounds and fonts into disc/files/hl2gc     (roughly 20-40 minutes)
  4. compiles the engine and the disc's apploader
  5. writes LambdaGC.iso

On Windows, run build.bat (it fetches a private copy of Python first). On Linux / macOS, run ./build.sh.

usage: python build.py [--hl2 PATH] [--skip-convert] [--skip-engine] [--skip-branch-check]
                       [--system-devkitpro] [--maps NAME ...] [--out FILE.iso]
"""
import argparse
import glob
import hashlib
import io
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "tools")
DISC = os.path.join(HERE, "disc")
LOCAL = os.path.join(HERE, ".tools")
HL2_APPID = "220"
EXE = ".exe" if sys.platform == "win32" else ""

DKP_SERVER = "https://pkg.devkitpro.org/packages"
# what the engine needs from devkitPro: the PowerPC compiler, newlib and its startup files, libogc + libfat,
# and elf2dol (gamecube-tools)
DKP_PACKAGES = ["devkitppc-binutils", "devkitppc-gcc", "devkitppc-newlib", "devkitppc-crtls",
                "gamecube-tools", "libogc", "libfat-ogc"]
PY_PACKAGES = ["numpy>=1.22", "scipy>=1.8", "pillow>=9", "zstandard>=0.21", "imageio-ffmpeg>=0.5"]


def say(msg):
    print(f"[LambdaGC] {msg}", flush=True)


def fail(msg):
    print(f"\n[LambdaGC] ERROR: {msg}\n", file=sys.stderr, flush=True)
    sys.exit(1)


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        fail("this step failed: " + " ".join(str(c) for c in cmd))


# ---------------------------------------------------------------- downloads
def download(url, tries=4):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "LambdaGC-builder"})
            with urllib.request.urlopen(req, timeout=120) as r:
                total = int(r.headers.get("Content-Length") or 0)
                buf = io.BytesIO()
                got, shown = 0, 0
                while True:
                    b = r.read(1 << 16)
                    if not b:
                        break
                    buf.write(b)
                    got += len(b)
                    if total > 4 << 20 and got - shown > total // 10:
                        shown = got
                        print(f"      {got * 100 // total}%", flush=True)
                return buf.getvalue()
        except Exception as e:  # network hiccups: try again a few times
            last = e
            time.sleep(2 + 3 * i)
    fail(f"couldn't download {url}: {last}\nCheck your internet connection and run the builder again.")


# ---------------------------------------------------------------- Python packages
def ensure_python_packages():
    if sys.version_info < (3, 9):
        fail("Python 3.9 or newer is needed.")
    missing = []
    for mod in ("numpy", "scipy", "PIL", "zstandard", "imageio_ffmpeg"):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    if not missing:
        return
    if os.environ.get("LAMBDAGC_PIP_DONE"):
        fail(f"the Python packages were installed but can't be imported ({', '.join(missing)}).")
    say(f"installing Python packages ({', '.join(missing)}) ...")
    base = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--upgrade"]
    private = os.path.abspath(sys.prefix).startswith(os.path.abspath(LOCAL)) or sys.prefix != sys.base_prefix
    attempts = [base + PY_PACKAGES] if private else []
    attempts += [base + ["--user"] + PY_PACKAGES, base + ["--user", "--break-system-packages"] + PY_PACKAGES]
    for cmd in attempts:
        if subprocess.run(cmd).returncode == 0:
            break
    else:
        fail("pip couldn't install the Python packages. Run:\n  "
             f"{sys.executable} -m pip install {' '.join(PY_PACKAGES)}")
    # start again so the new packages are picked up however pip installed them
    os.environ["LAMBDAGC_PIP_DONE"] = "1"
    sys.exit(subprocess.run([sys.executable] + sys.argv).returncode)


def ffmpeg_path():
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return shutil.which("ffmpeg")


# ---------------------------------------------------------------- devkitPro
def dkp_complete(root):
    return (os.path.isfile(os.path.join(root, "devkitPPC", "bin", "powerpc-eabi-gcc" + EXE))
            and os.path.isfile(os.path.join(root, "libogc", "include", "ogcsys.h"))
            and os.path.isfile(os.path.join(root, "libogc", "lib", "cube", "libogc.a"))
            and os.path.isfile(os.path.join(root, "libogc", "lib", "cube", "libfat.a"))
            and os.path.isfile(os.path.join(root, "tools", "bin", "elf2dol" + EXE)))


def dkp_host_repo():
    m = platform.machine().lower()
    arm = m in ("arm64", "aarch64")
    if sys.platform == "win32":
        return f"{DKP_SERVER}/windows/x86_64", "dkp-windows"
    if sys.platform == "darwin":
        return f"{DKP_SERVER}/osx/{'arm64' if arm else 'x86_64'}", "dkp-osx"
    return f"{DKP_SERVER}/linux/{'aarch64' if arm else 'x86_64'}", "dkp-linux"


def read_repo_db(url):
    """a pacman repository database: package name -> (file name, sha256)"""
    pkgs = {}
    with tarfile.open(fileobj=io.BytesIO(download(url)), mode="r:*") as t:
        for m in t.getmembers():
            if not m.name.endswith("/desc"):
                continue
            text = t.extractfile(m).read().decode("utf-8", "replace")
            fields = {k: v.strip() for k, v in re.findall(r"%([A-Z0-9]+)%\n(.*?)(?:\n\n|\Z)", text, re.S)}
            if "NAME" in fields and "FILENAME" in fields:
                pkgs[fields["NAME"]] = (fields["FILENAME"], fields.get("SHA256SUM", ""))
    return pkgs


def pkg_path(name):
    """where a file in a devkitPro package goes: opt/devkitpro/... -> ..., pacman's metadata -> None"""
    name = name.replace("\\", "/")
    while name.startswith("./"):
        name = name[2:]
    if not name or name.startswith("."):
        return None  # .PKGINFO, .MTREE, .BUILDINFO, .INSTALL
    for pre in ("opt/devkitpro/", "devkitpro/"):
        if name.startswith(pre):
            name = name[len(pre):]
            break
    parts = [p for p in name.split("/") if p]
    if not parts or ".." in parts or name.startswith("/"):
        return None
    return parts


def extract_pkg(data, dest):
    """unpack a .pkg.tar.zst into dest; links become plain copies, so it works the same on every system"""
    import zstandard
    stream = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(data))
    links = []
    with tarfile.open(fileobj=stream, mode="r|") as t:  # streamed: the compiler unpacks to ~500 MB
        for m in t:
            parts = pkg_path(m.name)
            if not parts:
                continue
            out = os.path.join(dest, *parts)
            if m.isdir():
                os.makedirs(out, exist_ok=True)
            elif m.isfile():
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with open(out, "wb") as f:
                    shutil.copyfileobj(t.extractfile(m), f)
                if sys.platform != "win32":
                    os.chmod(out, 0o755 if m.mode & 0o111 else 0o644)
            elif m.issym():
                links.append((out, os.path.normpath(os.path.join(os.path.dirname(out), m.linkname))))
            elif m.islnk():
                tp = pkg_path(m.linkname)
                if tp:
                    links.append((out, os.path.join(dest, *tp)))
    for _ in range(4):  # links to links
        for out, src in links:
            if os.path.isfile(src) and not os.path.exists(out):
                os.makedirs(os.path.dirname(out), exist_ok=True)
                shutil.copy2(src, out)
            elif os.path.isdir(src) and not os.path.exists(out):
                shutil.copytree(src, out)


def ensure_devkitpro(use_system):
    """our own copy of the toolchain in .tools (or an existing devkitPro install, if asked for)"""
    own = os.path.join(LOCAL, "devkitpro")
    cands = []
    if use_system:
        cands += [os.environ.get("DEVKITPRO", ""), r"C:\devkitPro" if sys.platform == "win32" else "/opt/devkitpro"]
    cands.append(own)
    for c in cands:
        if c and dkp_complete(c):
            return c
    say("downloading devkitPro's GameCube toolchain (about 100 MB, only the first time) ...")
    host_url, host_repo = dkp_host_repo()
    repos = [(read_repo_db(f"{host_url}/{host_repo}.db"), host_url),
             (read_repo_db(f"{DKP_SERVER}/dkp-libs.db"), DKP_SERVER)]
    tmp = own + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    for name in DKP_PACKAGES:
        for db, base in repos:
            if name in db:
                fn, sha = db[name]
                say(f"  {fn}")
                data = download(f"{base}/{fn}")
                if sha and hashlib.sha256(data).hexdigest() != sha:
                    fail(f"{fn} didn't match devkitPro's checksum; run the builder again.")
                extract_pkg(data, tmp)
                break
        else:
            fail(f"devkitPro's package server has no '{name}' for this system.")
    shutil.rmtree(own, ignore_errors=True)
    os.replace(tmp, own)
    if not dkp_complete(own):
        fail("devkitPro's toolchain was downloaded but is incomplete. Delete the .tools folder and try again.")
    return own


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
        m = re.search(r'"BetaKey"\s+"([^"]*)"', f.read())
    return m.group(1) if m else ""


# ---------------------------------------------------------------- the engine
def build_engine(dkp):
    bin_ = os.path.join(dkp, "devkitPPC", "bin")
    gcc = os.path.join(bin_, "powerpc-eabi-gcc" + EXE)
    objcopy = os.path.join(bin_, "powerpc-eabi-objcopy" + EXE)
    elf2dol = os.path.join(dkp, "tools", "bin", "elf2dol" + EXE)
    env = dict(os.environ, DEVKITPRO=dkp, DEVKITPPC=os.path.join(dkp, "devkitPPC"),
               PATH=bin_ + os.pathsep + os.environ.get("PATH", ""))
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
             "-c", c, "-o", o], env=env)
        objs.append(o)
    elf = os.path.join(HERE, "engine", "hl2gc.elf")
    dol = os.path.join(HERE, "engine", "hl2gc.dol")
    run([gcc, *objs, *m, "-L" + ogc_lib, "-lfat", "-lasnd", "-logc", "-lm",
         "-Wl,-Map=" + os.path.join(HERE, "engine", "hl2gc.map"), "-o", elf], env=env)
    run([elf2dol, elf, dol], env=env)
    ap = os.path.join(HERE, "engine", "apploader")
    a = ["-O2", "-mcpu=750", "-meabi", "-msoft-float", "-G0", "-msdata=none", "-ffreestanding", "-fno-builtin",
         "-fno-pic", "-nostdlib", "-nostartfiles"]
    run([gcc, *a, "-c", os.path.join(ap, "apploader.c"), "-o", os.path.join(out, "apploader.o")], env=env)
    run([gcc, *a, "-c", os.path.join(ap, "header.S"), "-o", os.path.join(out, "aphdr.o")], env=env)
    run([gcc, *a, "-T", os.path.join(ap, "apploader.ld"), os.path.join(out, "aphdr.o"),
         os.path.join(out, "apploader.o"), "-o", os.path.join(out, "apploader.elf")], env=env)
    os.makedirs(os.path.join(DISC, "sys"), exist_ok=True)
    run([objcopy, "-O", "binary", os.path.join(out, "apploader.elf"),
         os.path.join(DISC, "sys", "apploader.img")], env=env)
    shutil.copyfile(dol, os.path.join(DISC, "sys", "main.dol"))


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Build LambdaGC.iso from your Half-Life 2 install (steam_legacy).")
    ap.add_argument("--hl2", help='the "Half-Life 2" folder (steamapps/common/Half-Life 2); found automatically')
    ap.add_argument("--skip-convert", action="store_true", help="keep the already converted disc/files")
    ap.add_argument("--skip-engine", action="store_true", help="keep the already built engine")
    ap.add_argument("--skip-branch-check", action="store_true",
                    help="don't insist on the steam_legacy branch (conversion will probably fail)")
    ap.add_argument("--system-devkitpro", action="store_true",
                    help="use an existing devkitPro install (DEVKITPRO, C:\\devkitPro or /opt/devkitpro)")
    ap.add_argument("--maps", nargs="*", help="convert only these maps (testing)")
    ap.add_argument("--out", default=os.path.join(HERE, "LambdaGC.iso"))
    a = ap.parse_args()

    say("LambdaGC is discontinued; this builder is provided as-is.")
    t0 = time.time()

    # Half-Life 2 first, so a wrong branch is reported before anything is downloaded
    game = None
    if not a.skip_convert:
        if a.hl2:
            game, acf = os.path.normpath(a.hl2), manifest_for(a.hl2)
        else:
            game, acf = find_hl2()
            if not game:
                fail("Half-Life 2 wasn't found in your Steam libraries. Install it (and switch it to the "
                     "steam_legacy branch), or pass --hl2 \"<...>/steamapps/common/Half-Life 2\".")
        if not os.path.isdir(os.path.join(game, "hl2", "maps")):
            fail(f"{game} doesn't look like a Half-Life 2 install (no hl2/maps folder).")
        say(f"Half-Life 2: {game}")
        if not a.skip_branch_check:
            if not acf:
                fail("couldn't find Steam's appmanifest_220.acf to check the branch. If this is the steam_legacy "
                     "version, run again with --skip-branch-check.")
            br = branch_of(acf)
            if br != "steam_legacy":
                fail("Half-Life 2 must be on the steam_legacy beta branch (the pre-20th Anniversary version).\n"
                     "  In Steam: right-click Half-Life 2 > Properties > Betas > choose \"steam_legacy\",\n"
                     f"  let it finish downloading, then run the builder again. (Current branch: {br or 'default'})")
            say("branch: steam_legacy")

    say("step 1/4: prerequisites")
    ensure_python_packages()
    ff = ffmpeg_path()
    if ff:
        os.environ["LAMBDAGC_FFMPEG"] = ff
    dkp = None if a.skip_engine else ensure_devkitpro(a.system_devkitpro)
    if dkp:
        say(f"devkitPro: {dkp}")

    if not a.skip_convert:
        say("step 2/4: converting Half-Life 2 (this takes a while) ...")
        maps_out = os.path.join(DISC, "files", "hl2gc", "maps")
        os.makedirs(maps_out, exist_ok=True)
        cmd = [sys.executable, os.path.join(TOOLS, "convert_all.py"), os.path.join(game, "hl2"), maps_out,
               "--only", "d1_", "d2_", "d3_", "background"]
        fonts = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
        if sys.platform == "win32" and os.path.isdir(fonts):
            cmd += ["--windows-fonts", fonts]  # the PC version's menu fonts
        if a.maps:
            cmd += ["--maps", *a.maps]
        run(cmd)
        if not glob.glob(os.path.join(maps_out, "*.hgm")):
            fail("the conversion produced no maps; see the messages above.")

    run([sys.executable, os.path.join(TOOLS, "make_disc_headers.py"), DISC])
    if not a.skip_engine:
        say("step 3/4: compiling the engine ...")
        build_engine(dkp)
    if not os.path.isfile(os.path.join(DISC, "sys", "main.dol")):
        fail("no engine built yet (run without --skip-engine).")
    if not glob.glob(os.path.join(DISC, "files", "hl2gc", "maps", "*.hgm")):
        fail("no converted maps in disc/files/hl2gc/maps (run without --skip-convert).")
    say("step 4/4: writing the disc image ...")
    sys.path.insert(0, TOOLS)
    import make_iso
    make_iso.build(DISC, a.out)
    say(f"done in {int(time.time() - t0) // 60} min: {a.out}")
    say("Play it in Dolphin (File > Open). For real hardware: copy disc/files/hl2gc to the SD card root and launch "
        "engine/hl2gc.dol with Swiss.")


if __name__ == "__main__":
    main()
