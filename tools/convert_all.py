#!/usr/bin/env python3
"""
Convert every Half-Life 2 map in the user's install to hl2gc packs.

usage: convert_all.py "<...>/Half-Life 2/hl2" <output folder> [--only d1_ d2_ ...]

The output folder should be <disc>/files/hl2gc/maps: vo.bin (voices) and font.bin are written next to it.
"""
import argparse
import os
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert import convert  # noqa: E402
from shared_data import Shared  # noqa: E402
from vpk import GameFS  # noqa: E402

MAX_PACK_BYTES = 13 * 1024 * 1024  # GameCube RAM: leaves room for weapons.hgm (2.7 MB), sounds, and the map's setup


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--only", nargs="*", default=["d1_", "d2_", "d3_"], help="map name prefixes")
    ap.add_argument("--tex-budget-kb", type=int, default=6144)
    ap.add_argument("--windows-fonts", default=None,
                    help=r"e.g. C:\Windows\Fonts: use Tahoma/Verdana/Trebuchet MS like the PC version")
    ap.add_argument("--maps", nargs="*", default=None, help="only these maps")
    ap.add_argument("--xbox-menu", default=None,
                    help="hl2OGXboxMenu.swf: the original Xbox menu's sounds and music for the main menu")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    maps = sorted(f[:-4] for f in os.listdir(os.path.join(a.game_dir, "maps"))
                  if f.endswith(".bsp") and f.startswith(tuple(a.only))
                  and os.path.getsize(os.path.join(a.game_dir, "maps", f)) > 0)
    failed = []
    shared = Shared(a.game_dir, os.path.dirname(os.path.abspath(a.out_dir.rstrip("/\\"))), GameFS(a.game_dir),
                    a.windows_fonts)
    # the main menu's background maps
    maps += sorted(f[:-4] for f in os.listdir(os.path.join(a.game_dir, "maps"))
                   if f.startswith("background") and f.endswith(".bsp") and f[:-4] not in maps)
    if a.maps:
        maps = [m for m in maps if m in a.maps]
    for m in maps:
        try:
            out = os.path.join(a.out_dir, m + ".hgm")
            convert(a.game_dir, m, out, a.tex_budget_kb, shared=shared)
            # NPCs add up to a couple of MB: keep every pack within the GameCube's memory budget
            budget = a.tex_budget_kb
            far = True
            while os.path.getsize(out) > MAX_PACK_BYTES and budget > 1536:
                if far:
                    # far LODs only save vertices: textures matter more, so they go first
                    far = False
                    print(f"  {m}: {os.path.getsize(out) / 1e6:.1f} MB is too big, dropping far LODs")
                else:
                    budget -= 768
                    print(f"  {m}: {os.path.getsize(out) / 1e6:.1f} MB is too big, textures -> {budget} KB")
                convert(a.game_dir, m, out, budget, shared=shared, far_lods=far)
            if os.path.getsize(out) > MAX_PACK_BYTES:
                # still too big with the smallest textures (Coast 3's many characters): their animations
                # baked at a lower rate (the engine interpolates between frames)
                import npcs
                keep = npcs.ANIM_FPS
                npcs.ANIM_FPS = 7.5
                print(f"  {m}: {os.path.getsize(out) / 1e6:.1f} MB is too big, animations -> 7.5 fps")
                try:
                    convert(a.game_dir, m, out, budget, shared=shared, far_lods=far)
                finally:
                    npcs.ANIM_FPS = keep
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            failed.append(m)
    # the Xbox menu's sounds and music
    swf = a.xbox_menu or next((p for p in (os.path.join(a.game_dir, "..", "hl2XboxMenu", "hl2OGXboxMenu.swf"),)
                               if os.path.exists(p)), None)
    if swf:
        try:
            import swfmenu
            swfmenu.build_xbox_menu(swf, shared.bank, os.path.dirname(os.path.abspath(a.out_dir.rstrip("/\\"))))
            shared.bank.save()
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            failed.append("xmenu.bin")
    # the player's weapons and items: one pack for every level
    try:
        import weapons
        weapons.build(a.game_dir, shared.fs, os.path.dirname(os.path.abspath(a.out_dir.rstrip("/\\"))), shared)
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        failed.append("weapons.hgm")
    # the startup movie and loading screens
    try:
        import media
        hl2gc_dir = os.path.dirname(os.path.abspath(a.out_dir.rstrip("/\\")))
        media.build_loading(shared.fs, hl2gc_dir)
        media.build_intro(a.game_dir, hl2gc_dir)
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        failed.append("intro.bin / loading.bin")
    # mark the looping sounds in every pack's sound table (the engine keeps those alive out of earshot)
    try:
        import soundflags
        hl2gc_dir = os.path.dirname(os.path.abspath(a.out_dir.rstrip("/\\")))
        bank = os.path.join(hl2gc_dir, "vo.bin")
        packs = [os.path.join(a.out_dir, m + ".hgm") for m in maps] + [os.path.join(hl2gc_dir, "weapons.hgm")]
        for pk in packs:
            if os.path.exists(pk) and os.path.exists(bank):
                soundflags.patch(pk, bank)
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        failed.append("loop flags")
    print(f"\n{len(maps) - len(failed)}/{len(maps)} maps converted" + (f"; failed: {failed}" if failed else ""))


if __name__ == "__main__":
    main()
