# LambdaGC (Cancelled)

> **This project is discontinued.** It is no longer developed or supported, and no further releases
> will be made. The code and the builder are published as-is, for anyone curious about how it worked.

LambdaGC (developed as *hl2gc*) is an engine that runs Half-Life 2's campaign on the Nintendo GameCube.
It reads converted copies of the original game's maps, models, textures, sounds and scripts and
re-implements the game logic (entity I/O, triggers, doors and trains, scripted sequences and
choreographed scenes, NPCs, weapons, physics, vehicles, HUD and menus) in C against libogc, using
Valve's Source SDK 2013 as the behavioural reference.

**No game content is included in this repository or its releases.** The builder converts *your own*
installed copy of Half-Life 2 on your own computer.

## Requirements

- **Half-Life 2 on Steam, on the `steam_legacy` branch.** This is the only thing you need to provide. The converter
  only understands the pre-20th Anniversary version of the game's files. In Steam: right-click *Half-Life 2* →
  *Properties* → *Betas* → choose **`steam_legacy`** and let it finish downloading. The builder checks this and
  stops if Half-Life 2 is on any other branch.
- An internet connection the first time (about 200 MB of tools are downloaded) and about 4 GB of free disk space.

Everything else is fetched and set up automatically, inside a `.tools` folder next to the builder. Nothing is
installed system-wide, and deleting the folder removes it all:

- **Python** (Windows only: a private copy of Python 3.13 from python.org; on Linux/macOS the system's Python 3 is
  used, and `build.sh` installs it with your package manager if it's missing)
- **numpy, scipy, pillow, zstandard** and **imageio-ffmpeg** (which brings ffmpeg, to decode the startup movie)
- **devkitPro's GameCube toolchain** (devkitPPC, libogc, libfat, gamecube-tools), downloaded from devkitPro's
  own package server and checked against its SHA-256 sums

## Building

Windows: double-click **`build.bat`**. Linux / macOS: run **`./build.sh`**.

That's all. The builder:

1. finds Half-Life 2 in your Steam libraries and checks that it's on the `steam_legacy` branch,
2. sets up the prerequisites above (only the first time),
3. converts the game into `disc/files/hl2gc/` (roughly 20–40 minutes),
4. compiles the engine and the disc's apploader,
5. writes **`LambdaGC.iso`**.

Options (rarely needed): `--hl2 "<...>/steamapps/common/Half-Life 2"` if Half-Life 2 isn't found
automatically, `--system-devkitpro` to use a devkitPro install you already have, `--skip-convert` /
`--skip-engine` to redo only one half, `--maps d1_trainstation_01 ...` to convert only some maps. Run
`build.bat --help` (or `./build.sh --help`) for the full list.

## Running

- **Dolphin:** File → Open → `LambdaGC.iso`.
- **Real hardware with an SD adapter:** copy `disc/files/hl2gc` to the SD card root and launch
  `engine/hl2gc.dol` with Swiss.

| Control | Action |
|---|---|
| Control stick / C-stick | move / look |
| A | jump |
| B (hold) | sprint |
| X | use, pick up / drop objects |
| R / L | primary / secondary fire |
| Y | reload |
| Z (hold) | crouch |
| D-pad left / right / down / up | previous / next / last weapon / flashlight |
| Start | menu |

## State at cancellation

The whole Half-Life 2 campaign converts and loads, and most story sequences play. Known gaps at the time
the project stopped include: the helicopter and gunship don't shoot and dropships don't land or deploy
soldiers; police guard zones and assault points aren't implemented; striders don't follow their paths;
several physics constraints (slide, pulley, keep-upright, thrusters) and the combine ball spawners are
missing; prop bodygroups and skins aren't supported; saving and loading aren't implemented. Some larger
maps have their texture resolution reduced to fit the GameCube's memory.

## Repository layout

```
build.py, build.bat, build.sh   the builder
engine/source/                  the GameCube engine (C, libogc)
engine/apploader/               the disc's apploader
tools/                          the converter (Python) and the disc image writer
tools/assets/                   the project's own GameCube A / B button icons for the menu
test/                           host-side tests (see test/bt.sh; they need libogc's headers in test/gxv/inc,
                                made with test/gxv/make_inc.py)
docs/DEVELOPMENT_NOTES.md       the original development notes
```

## Legal

Half-Life 2 and its content are © Valve Corporation. This project is not affiliated with or endorsed by
Valve or Nintendo. Never distribute the converted files (`*.hgm`, `vo.bin`, `font*.bin`, `menu.bin`,
`xmenu.bin`, `intro.bin`, `loading.bin`) or a built `LambdaGC.iso`: they contain converted game assets.
Behaviour was modelled on Valve's Source SDK 2013, which is under the Source 1 SDK Licence (free,
non-commercial use only), so this project must stay free and non-commercial.
