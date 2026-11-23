> **Note:** these are the original development notes, kept for reference. Paths and some tools mentioned here (the Dolphin debugging bridge, `hl2gc.iso`) are not part of this repository; see README.md for how to build.

# hl2gc — Half-Life 2 on the Nintendo GameCube

A new GameCube engine for Half-Life 2, fed by a converter that reads **your own** Steam install.
No Valve code or content ships with this project: the engine is written from scratch against
libogc, and every map pack is generated on your PC from the game files you own.

**Status: milestone 7 (HEV suit, water, breakables, vehicles, PC-accurate fixes), on top of milestones 4-6 (NPCs, dialogue, menu, physics, weapons).** Walk through every campaign map with Half-Life 2's
player movement and collision, props with their .phy collision hulls, and each map's logic on
Source's input/output system. The map's characters are now in the world: skinned and animated
from your models' own skeletons and sequences, they walk to their marks, act out
`scripted_sequence`s, follow you (`ai_goal_follow`, player squad), and perform the game's
choreographed scenes from your `scenes.image` - speaking their lines from your voice files, with
closed captions from your `closecaption_english.txt`.

v28 fixes (from playtesting v27):

- **Doors drawn a quarter turn out**: Source's door models are authored with their idle pose 90 degrees
  from the bind pose, and props were converted in the bind pose. A shut door stuck out of its frame
  like an open one and an open one lay in the frame like a shut one. Many-boned prop models are now
  converted in their first sequence's pose, as Source shows them (all 164 doors have the leaf along
  the frame). Door logic follows `CPropDoorRotating` / `CBaseDoor`: the "Starts Open" flag on prop
  doors (Barney's door in Point Insertion), forward/back angles and hinge side, opening away from
  whoever opens it, the Open input opening forward, sliding doors that start open being open, and
  rotating brush doors that start open swapping their angles.
- **Player weapons were invisible**: a converter bug left every viewmodel without its animations, so
  the engine never drew it. All 11 viewmodels now have their draw / idle / fire / reload sequences.
- **Sounds that stopped for good**: a looping sound (an ambient_generic's hum, machinery) was dropped
  if the player was out of earshot when it started, or when a nearer sound needed its voice, and
  never came back. Looping sounds are now parked and resume under the same handle (the packs mark
  them: `tools/soundflags.py`). A sound whose disc read failed once also stayed dead; it's re-read.
- **Stand-in effects replaced with the game's own**: muzzle flashes (`effects/muzzleflash1-4`),
  bullet holes per surface (`decals/concrete|metal|wood|glass/shot*`, at Source's decal sizes and
  blending), dust, flecks, sparks, blood and splashes (`particle_smokegrenade`, `fleck_*`,
  `effects/spark`, `blood_core`, `splash2`), the explosion (`sprites/zerogxplode`'s frames), the
  AR2 ball, and rockets, crossbow bolts and SMG grenades as their models.

v27 fixes (from playtesting v26):

- **Use** works the way Source's does: if nothing is exactly under the crosshair, the usable thing
  nearest the middle of the view within reach (80 units, ~37 degrees) is used, so doors and buttons
  don't need pixel-perfect stick aim.
- **Doors' open and closed states** follow Source: a brush door set to "Starts Open" treats its open
  spot as closed (the map's Open/Close inputs were reversed); prop doors spawn open forward/back or
  ajar as set, and an ajar door opens away from you.
- **Held suitcases** are bone-merged like Source (hand and the hand's attachment bone), and each held
  model keeps its own skeleton; they sat off the hand, turned sideways.
- **Turnstiles**: NPCs walk through the ones the map marks (logic_navigation "Ignore"), turning them.
- **Hints** show the GameCube button for the action, following your button layout ("[X] OPEN DOOR"),
  and the game's console wording is used where it has one (e.g. "Control stick" instead of keys).
- **The HALF-LIFE 2 logo** in the intro is drawn at the PC's size (it overflowed the screen).
- Maps only carry the combat animations for the weapons their NPCs actually use, and a map still
  too big after its textures shrink gets its animations at 7.5 fps: the biggest pack is 13.6 MB
  (v26: 14.6 MB), leaving more memory free on every map.
- **More of the PC HUD**, laid out by `hudlayout.res` and drawn with `halflife2.ttf` like the PC
  (`hl2gc/font_icons.bin`, new): the numbers in the HUD font; the weapon's icon in the weapon
  selection; the pickup history on the right (weapons, ammo with the gun beside it and the amount,
  batteries, health kits; fading over 5 s); the squad panel ("SQUAD FOLLOWING", an icon per member,
  medics marked); and the neurotoxin panel while poison heals. Black headcrab bites poison you down
  to 1 health like on PC, given back 10 a second; poison and radiation now bypass the suit's armour.

This build (v26) fixes, checked against the PC game's code and data:

- **Ragdolls**: killed NPCs collapse as a jointed body (bone constraints, world collision, friction)
  pushed the way the shot came, instead of standing in a death pose.
- **NPC weapons**: NPCs hold their weapons (and the train passengers their suitcases, given by the
  map's `giveweapon`), bone-merged to the right hand like Source; armed NPCs use their combat idle,
  aim, run-aim and fire animations, with muzzle flashes.
- **Bullet impacts**: holes on walls and floors (per surface type from `surfaceproperties`), dust
  and chips off concrete/wood/dirt, sparks off metal, splashes on water, blood on flesh; NPC
  misses hit the world around you the same way.
- **Breakables scatter**: pieces used to freeze where they spawned when they started overlapping
  what they broke off (a board nailed to a wall); they now fall and scatter.
- **NPCs and physics props**: thrown or punted objects hit NPCs instead of passing through them,
  and hurt them by Source's NPC impact damage table; NPCs still shove light props out of their way.
- **Doors** open away from you, and never through a wall when the other way is clear.
- **Missing props**: a static prop whose origin was under the floor (the chair in Barney's office)
  was never drawn. Props are now visible whenever any BSP cluster they touch is (vbsp's leaf list).
- **Characters sharing a skeleton** (citizen heads, hands) are skinned with their own bind pose:
  the hands were up to 3 units off, so the train passenger's hand missed the grab rail.
- **Barney's helmet** follows his head in the same tick as his animation (it trailed a tick behind).
- **Dialogue timing**: lines load from the disc a few seconds before they're due, instead of a
  whole scene's lines at once (which pushed the first ones out of the 1 MB sound cache before they
  played, so they started late); if a line is still loading when due, the scene waits for it (up to
  a second) so voice, mouth and gestures stay together. Speech takes priority over other sounds.
- **The HALF-LIFE 2 logo** shows before the G-Man's speech (`env_credits` ShowLogo, credits.txt timing).
- **Controls**: every button can be remapped (Options > Buttons), and the menus say "stick".
- **Performance**: NPCs look for you ten times a second instead of every tick (sight traces were 80%
  of the tick on big maps); props and characters are drawn as triangle strips; far LODs for more
  models; distant models use cheaper lighting; and the level of detail adapts when a scene gets
  heavy, so the frame rate holds instead of dropping.

Milestone 7 adds:

- **HEV suit**: aux power like `hl2_player.cpp` - sprint and the flashlight drain it, swimming
  under water uses it as air, and it recharges when idle. Run out of air and you drown (10 damage
  a second, armour doesn't help); the lost health comes back after you surface. AUX POWER panel on
  the HUD. Flashlight on D-pad up (a projected cookie, 45 degrees, 750 units).
- **Water**: swimming, water levels and water jumps from Source's gamemovement; physics props float
  by their mass and volume; water surfaces refract the scene behind them (an EFB copy distorted by
  the material's normal map through GX indirect texturing, the Xbox-era technique) and take the
  material's fog colour; under water the view gets the water's fog. Canal slime uses its own
  material colours.
- **Breakables**: crates, bottles, glass, barrels and `func_breakable`s break into the model's own
  gib pieces (from the .phy) or generic wood/glass/metal/concrete chunks, which bounce, settle and
  fade, with the material's break sound.
- **Vehicles**: the airboat (fast on water, struggles on land) and the buggy (jeep, with turbo).
  Press X at the vehicle to get in. The vehicle you drive is carried into the next map like on PC.
- **G-Man intro fix**: the intro's views were combined on the CPU from EFB copies, which Dolphin's
  default "Store EFB Copies to Texture Only" never writes to memory, so the intro was black. They are
  now combined on the GPU (TEV stages, with luminance from I8 EFB copies), which works both ways.
  The very wide, stretched train view during the G-Man's speech is the map's own 170-degree FOV
  zoom (`SetFOV 170`, blending back to 75), as on PC.
- **Polish pass, from the PC's code and data**:
  - footsteps on every surface (`scripts/surfaceproperties*.txt`: concrete, metal, dirt, grass,
    wood, ...), alternating feet every 300 ms running / 400 ms walking like `CBasePlayer`, splashing
    in shallow water, wading deeper, swimming strokes
  - the HEV suit's voice (`scripts/sentences.txt`): the logon when you put it on, fractures and
    blood loss, morphine and automedic, health critical / near death, hazards, and the battery
    power level, with Source's rules and no-repeat timers
  - HUD damage direction indicator, and a Geiger counter near radiation `trigger_hurt`s
  - sprites and glows (`env_sprite`, `env_lightglow`) and searchlight beams with halos
    (`point_spotlight`), switched by the map's inputs; glows fade when something hides them
- **Startup and loading like the PC**: the Valve startup movie (your `media/valve.bik`, with its
  sound; A, B or START skips it), then the startup "Loading..." box while the menu's background map
  loads. A new game keeps the menu's background on screen with the "Loading..." box and a progress
  bar; level changes keep the last frame up with the glowing "Loading..." in the corner (both images
  are your install's `materials/console/startup_loading` and `console/loading`). No boot text.
- **Errors in dialogs, with codes** (below) instead of a text screen: a map that won't load from the
  menu brings you back to the menu with the error; one that won't load at a level change keeps you
  in the level you were in.
- **Memory**: map packs are read into one block reserved at boot, sized for the biggest pack on the
  disc. Before, each map was allocated separately, and sounds and buffers allocated while the menu
  ran split the heap, so big maps like Point Insertion failed to load from New Game. Maps are now
  converted to at most 13 MB.
- **PC-accurate fixes**: the G-Man intro (script_intro blending of the camera and normal views,
  fades), monitors no longer striped, sound is positional with Source's falloff, occlusion and
  long sounds streamed instead of cut off, eyes look at you, multi-bone props (the walkway's window
  bars, pods) animate, doors follow Source's use/blocking rules, the G-Man faces the player.

Milestone 6 added:

- **Weapons**: crowbar, gravity gun, pistol, .357, SMG (with grenades), pulse rifle (with energy
  balls), shotgun (double blast), crossbow, grenades, RPG (laser-guided) and bugbait, using your
  install's viewmodels, animations and sounds (`hl2gc/weapons.hgm`), with HL2's damage, fire rates
  and clip sizes. Bullets hit NPCs (they die, slumping into the pose their ragdoll would settle in),
  break crates, glass and other breakable props (their `prop_data` health; explosive barrels
  explode), and knock physics objects around.
- **The gravity gun**: secondary pulls and holds objects up to 250 kg, primary punts them.
  Thrown objects and punts follow the bodies' real masses. In the Citadel it gets supercharged.
- **Items and the suit**: health kits, vials, batteries, ammo, weapons lying in the levels, wall
  chargers, ammo crates and supply crates that drop their items. The maps hand them out the way they
  do on PC (`item_suit`, `point_template` "new game" spawners, `trigger_weapon_strip/dissolve`).
  The HUD (health, suit, ammo, crosshair, weapon selection) appears once you have the suit.
  Falls and `trigger_hurt` hurt; dying restarts the level with what you had when you arrived.
- **Cutscenes**: `point_viewcontrol` cameras (G-Man intro included), `env_fade`, `env_shake`,
  `env_zoom`, `func_tracktrain` trains that carry you; monitors (`func_monitor`) show their camera.
- **Character animation in cutscenes, like Source's**: sequences are layered the way
  `bone_setup.cpp` does it (weight lists, delta and post layers, autolayers with their
  spline/crossfade ramps, realtime loops), so postures and layered sequences play properly instead
  of falling back to the rest pose. Scene **gestures** (the most common choreography event, 5,553 of
  them in `scenes.image`) play as overlay layers timed by each event's apex/loop/end tags, with
  fades. **Look-at** events turn spine, neck and head toward the target (within HL2's head limits,
  eased), eyes follow the target, and idle characters glance at you when you walk up to them.
- **Trains, doors and getting off**: things parented to trains are attached where the map places
  them (trains have already jumped to their first path_track by then), parented sliding doors keep
  sliding while they ride (the arrival train's doors open), func_door's "Toggle" doors stay open, the
  player and passengers turn with a train at curves, and a player pushed into a car's seats is moved
  back into free space instead of getting stuck.
- **NPC pathfinding**: NPCs route through each map's compiled AI node graph (`maps/graphs/*.ain`,
  A* over the links a human-sized hull can walk), skip ahead when the next node is in plain view,
  re-plan when blocked, and only teleport as a last resort. The arrival passengers walk out of the
  door to the luggage drop instead of through the train walls.
- **Act-busy goals and queues**: `ai_goal_actbusy` sends NPCs to their `info_node_hint` spots;
  `ai_goal_actbusy_queue` (Trainstation 2's food line) moves up when the front citizen leaves for
  the exit, never through a spot the player blocks. Friendly NPCs step aside when you walk into
  them and go back afterwards.
- **Turnstiles**: a `func_physbox` on an upright `phys_hinge` turns when you walk into it.
- **Attachments and animation events**: `SetParentAttachment` puts props on an NPC's attachment
  (Barney's helmet on his head, the faceplate in his hand) and keeps them there as he animates;
  sequences' script events fire the `scripted_sequence`'s `OnScriptEvent01-08` (the helmet swap).
- **Monitors**: screens use their own texture coordinates on a 256x256 camera texture that tiles,
  like Source's `_rt_Camera` (the curved jumbotrons were squeezed into stripes before), cameras
  render square as point_camera does, and `SetOnAndTurnOthersOff` switches cameras (Kleiner on
  Barney's monitor).
- **Combat**: Combine (pistol, SMG, AR2, shotgun, stunstick) and zombies, headcrabs and antlions
  attack once the `gordon_precriminal` global is off (`env_global` states carry across maps like
  Source's). They face you, keep their weapon's range using the node graph, fire in bursts with
  Source's NPC damage, and miss more at range and when you run. This is much simpler than Source's
  squad AI: no cover, grenades, flanking or squad callouts yet.
- **Ladders**: `func_useableladder` and `func_ladderendpoint` ladders: walk into one to climb on,
  forward climbs (down when looking down), jump lets go, the ends step you off at the nearest free
  `info_ladder_dismount`.
- **Vehicle guns**: the buggy's tau cannon (15 per shot) and the airboat's gun (held, heats up) on
  R, enabled by `EnableGun`, with a vehicle crosshair and the airboat gun's heat on the HUD.
- **HUD from HudLayout.res**: secondary ammo at r76 with the primary moved to r222 as HL2's HUD
  animation does, weapon selection with 32-pixel slot boxes and a 112x80 selected box, and the quick
  info brackets beside the crosshair (health left, clip right).
- **Faces**: flex animation from the models' own flex controllers and rules, lip sync from each
  voice line's phonemes, blinking, and the scenes' flex tracks.
- **Speech you hear from where it's said**: lines fade with distance (PC's speech radius) and play
  through `env_microphone`/`env_speaker` relays. Captions are off by default (Options), smaller,
  and only show lines you can hear. Citizens answer when you use them (the response system's lines).
- **Cheats** in the pause menu: no clipping, god mode, infinite ammo, all weapons and ammo.

Milestone 5 added:

- **The original Xbox release's menu** (ported from `hl2OGXboxMenu.swf`), at the bottom left of the
  screen: the grey panel slides in with the SWF's whoosh, lands with its flicker and settle sound; the focused item
  gets the orange gradient bar and the glowing ">"; Options moves the panel left into GAME
  (captions, volume), CONTROLLER (reverse look, sensitivity) and BUTTONS (the GameCube layout);
  the legend bar shows the GameCube's A (select) and B (back). Its rollover, press, submenu and
  back sounds play, and its music loops on the title screen. The SWF's static background is not
  used: the chapter's live background map (background01-07) stays behind the menu. New Game opens
  chapter select with the 14 chapter thumbnails; Start opens the menu as the pause menu (Resume,
  Save, Cheats added; B resumes). Text uses the game's own menu font.
  Load/Save Game are shown greyed out: saving isn't implemented yet.
- **On-screen text with the game's fonts**: `game_text`, `env_message` (titles.txt, e.g. chapter
  titles) and `env_hudhint`, plus captions. The fonts are built from your install's TTFs; pass
  `--windows-fonts C:\Windows\Fonts` to `convert_all.py` to use the Tahoma/Verdana/Trebuchet MS the
  PC version draws with.
- **Live screens**: monitors (`func_monitor`, `_rt_Camera` materials) show their `point_camera`'s
  view - NPCs included - rendered into a 256x192 texture every other frame.
- **Physics props**: `prop_physics` objects are rigid bodies with their real masses (from the .phy
  files): gravity, tumbling, bounce and friction. Walk into them to push them; X picks up light
  ones (35 kg or less, like PC) and drops them, Y throws.
- **Doors**: open/close/locked sounds, func_doors that open on touch, NPCs open doors in their way.

Not there yet: combat AI (hostile NPCs stand idle), facial animation and lip sync, gesture layers,
NPC pathfinding (NPCs walk straight at their goal; if something blocks them for a few seconds they
are moved there so scripts keep going).

## How it works

```
Steam "Half-Life 2/hl2"  ──►  tools/convert.py (your PC)  ──►  map.hgm  ──►  SD card  ──►  hl2gc.dol (GameCube)
  .bsp / .vpk / .vmt / .vtf      parse, convert, fit memory         big-endian,
                                                                    GX-ready
```

The converter does the work the GameCube can't afford at load time:

| Source (PC) | GameCube pack |
|---|---|
| VTF textures, mostly 512², DXT1/DXT5 | GX CMPR (4 bpp), 256² cap. DXT1 is repacked losslessly from the VTF's own mips. Alpha-tested textures use CMPR punch-through, translucent ones RGB5A3. A per-map budget (default 6 MB) shrinks the biggest textures first. |
| RGBE lightmaps, 4 bytes/luxel | Packed into a 1024-wide CMPR atlas, block-aligned so no compression block mixes two faces. Gamma-converted at half intensity; the TEV doubles it back (Source's LDR overbright). |
| Brush faces, displacements | 32-byte vertices (pos, uv, lightmap uv, blend alpha). Displacements use the SDK's own lightmap mapping (`CCoreDispSurface::CalcLuxelCoords`) and are linked into BSP leaves the way the engine does at load. |
| VMT shaders | One of 8 fixed-function kinds: opaque, alpha-test, translucent, additive, water, sky, nodraw, two-texture blend. |
| BSP tree, PVS, entities | Kept as-is, byte-swapped. Plus brush-entity models, spawn point, and skybox. |
| NPC models (MDL v44-49, $includemodel, .ani blocks) | Skeleton + baked sequences (idle, walk, run and every sequence the map's scenes and scripts name) at 10 fps, 12 bytes per bone per frame. Meshes are rigidly skinned and split into batches of at most 10 bones, so GX's matrix palette skins them in hardware. |
| scenes.image (compiled .vcd) | Each scene the map uses: speak, sequence, move-to, face, fire-trigger, pause sections, loops. |
| Voice lines (hl2_sound_vo_english, hl2_sound_misc) | One shared bank on the disc, `hl2gc/vo.bin`: IMA ADPCM, 22 kHz mono (8x smaller than the PC's 44 kHz PCM), streamed from disc when a line starts. `$gender01` lines pick male/female from the speaker's model. |
| closecaption_english.txt | Caption strings for every line, drawn with a font atlas built from your install's `dejavusans.ttf` (`hl2gc/font.bin`). |

### Measured on your install (all 68 campaign maps)

| | |
|---|---|
| Pack size | 3.0–9.2 MB, average 6.0 MB (largest: d1_canals_12) |
| GameCube RAM | 24 MB main memory. Pack + 2 framebuffers (1.2 MB) + GX FIFO (256 KB) + engine ≈ 11 MB worst case |
| Conversion time | about 1.5 s per map, 97 s for the whole campaign |

## 1. Convert your maps (PC)

Needs Python 3.9+ with `numpy`, `scipy` and `Pillow`, plus `ffmpeg` on the PATH for the startup
movie (`media/valve.bik`) and for any voice files that aren't plain PCM WAV:

```bat
pip install numpy scipy pillow
python tools\convert_all.py "C:\Program Files (x86)\Steam\steamapps\common\Half-Life 2\hl2" disc\files\hl2gc\maps --xbox-menu "%USERPROFILE%\Downloads\hl2XboxMenu\hl2OGXboxMenu.swf"
```

The disc's banner (the picture and title Dolphin's game list, Swiss and the GameCube menu show) is
`disc/files/opening.bnr`, made from `banner.jpg`:

```bat
python tools\banner.py banner.jpg disc\files\opening.bnr
```

`--xbox-menu` takes the Xbox menu's SWF; its sounds and music go into `vo.bin` and the menu's
sound table into `xmenu.bin`. Without it the menu looks the same but is silent.

Or just one map:

```bat
python tools\convert.py "C:\...\Half-Life 2\hl2" d1_trainstation_01 d1_trainstation_01.hgm
```

Check a pack on your PC without building anything. This renders the spawn view in software,
reading the pack exactly as the engine does:

```bat
python tools\preview.py d1_trainstation_01.hgm view.png
python tools\preview.py d1_trainstation_01.hgm view.png --pos -4096 -1984 40 --ang 0 65
```

## 2. Build

Install devkitPro from https://github.com/devkitPro/installer/releases and tick "GameCube
development". Then double-click `build.bat`. It builds `engine/hl2gc.dol` and hl2gc's own disc
loader, and assembles a disc folder:

```
disc/sys/main.dol        the engine
disc/sys/apploader.img   hl2gc's disc loader (open source, no Nintendo code)
disc/sys/boot.bin, bi2.bin
disc/files/hl2gc/maps/   converted maps
disc/files/hl2gc/vo.bin  voice bank; font*.bin fonts; menu.bin menu; xmenu.bin Xbox menu sounds
                         (written by the converter)
```

## 3. Run it

- **Dolphin:** File → Open → `hl2gc.iso` (build.bat assembles it from the disc folder).
- **Real hardware with an SD adapter:** copy `disc/files/hl2gc` to the SD card root and launch
  `engine/hl2gc.dol` with Swiss. The engine looks for a disc first, then SD2SP2, slot A, slot B.

### Controls

| | |
|---|---|
| Control stick | move |
| C-stick | look |
| A | jump |
| B (hold) | sprint |
| X | use (doors, buttons, NPCs, chargers), pick up / drop objects |
| R | fire (throws an object you're carrying) |
| L | secondary fire (gravity gun: pull / drop) |
| Y | reload (also throws a carried object) |
| Z (hold) | crouch |
| D-pad left / right | previous / next weapon |
| D-pad down | last weapon |
| D-pad up | flashlight |
| Start | menu (pause); no clipping and other cheats are under Cheats |

Driving: control stick steers and throttles, C-stick looks, B turbo (buggy), A handbrake, X gets out.

### Error codes

| Code | Meaning | What to do |
|---|---|---|
| HL2GC-E01 | A map pack isn't on the disc / SD card | Convert it (`convert_all.py`) and run `build.bat` |
| HL2GC-E02 | A map pack is bigger than the memory reserved for maps | Convert the maps again with the current tools |
| HL2GC-E03 | Reading a map from the disc or SD card failed | Rebuild the disc image; check the card |
| HL2GC-E04 | The pack is from another converter version, or isn't a pack | Convert the maps again |
| HL2GC-E05 | Out of memory setting up a map (visibility, rendering, collision) | Report it with the map name |
| HL2GC-E06 | Out of memory creating a map's characters and objects | Report it with the map name |
| HL2GC-E07 | Not enough memory at boot for the map block | Convert the maps again (keeps them under 13 MB) |
| HL2GC-E08 | No hl2gc disc or SD card | Boot `hl2gc.iso`, or put `hl2gc/` on the SD card |

## Layout

```
engine/            GameCube engine (C, libogc)
  source/main.c    video/GX init, input, 66 Hz simulation loop, map loading, level transitions
  source/player.c  HL2 player movement (follows the SDK's gamemovement.cpp, written in C)
  source/collide.c swept-box collision: BSP brushes, brush entities, displacement triangles,
                   prop collision hulls (.phy convex pieces), moving and rotating doors
  source/ent.c     entities and Source's input/output system (outputs, delays, fire limits),
                   scripted_sequence, ai_goal_follow, npc_template_maker, ambient_generic
  source/npc.c     NPCs: pose evaluation, locomotion with collision, following, facing
  source/scene.c   choreographed scene player (logic_choreographed_scene)
  source/audio.c   voice/sound playback: IMA ADPCM decoded into ASND double buffers
  source/caption.c closed captions
  source/text.c    fonts, 2D drawing, HUD messages and hints
  source/menu.c    main/pause menu, chapter select, options
  source/physics.c rigid-body physics props, pushing, pick up and throw
  source/weapons.c weapons, items, HEV suit (aux power, drowning, flashlight), HUD
  source/intro.c   script_intro view blending (G-Man intro)
  source/gibs.c    breakable gibs
  source/loadscreen.c startup movie, loading screens
  source/vehicle.c airboat and buggy
  source/disc.c    GameCube disc file-table reader
  apploader/       hl2gc's own apploader (loads main.dol from a disc)
  source/pack.c    .hgm loader (one aligned read, pointers into it)
  source/world.c   BSP leaf lookup, PVS decompression, frustum culling, material bucketing
  source/render.c  GX vertex arrays, TEV setups per material kind, skybox, skinned NPCs,
                   model lighting (ambient cube in GX lights), light style layers, far LODs
  source/light.c   model lighting from the map's worldlights (traced), light styles, light_dynamic
  source/fx.c      material effects: the proxies' programs, run once a frame
  source/soundscape.c soundscapes (env_soundscape and its triggers), DSP rooms
tools/             PC-side converter (Python)
  vpk.py           VPK v1/v2 reader + engine-style search path (loose → VPK → map pakfile)
  bsp.py           BSP v19/v20 parser
  material.py      VMT/KeyValues parser, patch materials, shader → material kind
  vtf.py           VTF 7.0–7.5 reader, DXT1/3/5 + uncompressed decoders
  gxtex.py         GX texture encoders (CMPR, RGB565, RGB5A3) with GX tiling
  convert.py       map → .hgm
  model.py         MDL/VVD/VTX meshes (with LODs) and PHY collision hulls
  props.py         static props, prop entities, prop lighting (ambient cubes, worldlights), far
                   LODs, entity key/values
  lightstyles.py   switchable / flickering lights: style layers and patterns
  matfx.py         material effects: proxies compiled to programs, unlit / two-texture / self-illum
  dolphin_mcp/     Dolphin debugger MCP server (JackPriceBurns/dolphin-debugger-mcp) set up for hl2gc
  assets/          the GameCube A / B button icons
  anim.py          skeletons and animation decoding (RLE anim values, Quaternion48/64, .ani)
  npcs.py          NPC models, rigid-skin batching, baked sequences, scenes and their sounds
  scenes.py        scenes.image / compiled .vcd reader
  sounds.py        soundscripts (game_sounds_manifest) and close captions
  soundbank.py     voice bank: decode, resample, IMA ADPCM encode
  shared_data.py   per-run shared data
  frontend.py      fonts, localization, titles.txt, menu data (menu.bin)
  banner.py        the disc banner (opening.bnr) from an image
  swfmenu.py       the Xbox menu's sounds and music from hl2OGXboxMenu.swf (xmenu.bin)
  media.py         startup movie (intro.bin) and loading screen images (loading.bin)
  convert_all.py   every campaign map
  make_disc_headers.py  boot.bin / bi2.bin for the disc folder
  preview.py       software renderer for .hgm packs (verification tool)
```

## What's been verified, and what hasn't

- **Verified here:** the converter on all 68 maps from your install. Rendered previews of
  several maps via `preview.py`, which reads the packs the way the engine does: big-endian
  structs, GX tile decoding, BSP lookup, PVS, and the same TEV maths. The skybox face layout
  comes from the SDK (`utils/vbsp/cubemap.cpp`), and its in-face orientation was fitted by
  matching pixels across face seams.
- **Not yet verified:** the `.dol` hasn't been built or run. The workspace couldn't run the
  devkitPPC toolchain, so the engine was checked with GCC (syntax only) against libogc's and
  libfat's real headers. Expect a few rounds of fixes on first build and first run in Dolphin.

## Tested

The collision and movement code is also compiled natively and driven against the real converted
packs (`test/sim.c`):

- Speeds match HL2: run 190, sprint 320, crouch-walk 63, jump height 20 (HL2: 21).
- 16,000-tick random walks (running, sprinting, jumping, crouching) on trainstation and coast
  terrain: never inside solid, never out of the world.
- Every map's script logic (`test/enttest.c`) under AddressSanitizer/UBSan: the intro plays out
  (Trainstation's G-Man intro runs to the player's teleport onto the train), then the player is
  walked into every trigger. No memory errors in any of the 68 maps.
- Door collision follows the door: a closed prop door blocks the doorway, an open one doesn't.
- Every touchable level exit in the campaign: 109 of 110 deliver the player standing on solid
  ground in the next map. The exception is d3_citadel_01 → 02, where the player rides a pod
  (an entity, which needs vehicle support).
- NPCs (`test/npctest.c`): every compiled scene in your scenes.image parses byte-exactly
  (1,837 of 1,837), and 751 of the 752 scenes the maps reference are in it. Skinned poses from the
  packs match a reference decode of the original MDL/.ani data to within 0.3 units, feet on the
  ground. On trainstation_01, walking the player through the map's triggers plays 27 scenes,
  86 voice lines and 52 captions, and all 18 scripted sequences complete.
- Weapons (`test/weaptest.c`, real `weapons.hgm` and maps, AddressSanitizer/UBSan): all 11
  viewmodels and their animations load; on d1_canals_01, 12 of the 13 damageable NPCs are shot dead,
  43 breakable props and a supply crate (dropping its items) broken with the crowbar, light props
  pulled, held and punted with the gravity gun, grenade/rocket/SMG grenade all explode, every gun
  reloads, a long fall kills, and walking onto items picks them up.
- Renderer (`test/rendertest.c` with `test/gxv`): the real `render.c`, `world.c`, `gibs.c`,
  `intro.c` and `text.c` run against a host GX validator that parses every vertex the way the
  GPU's command processor does and checks it against the vertex descriptor and formats, the
  arrays and the loaded matrices. Point Insertion, 1658 frames from the player, NPCs, trains and
  the intro: 0 stream errors, 0 vertices out of range. (`test/gxv/make_inc.py <libogc>/gc` makes
  the hooked headers it compiles against.)
- The validator can't see inside libogc's assembly, which is where the stretched-props bug was:
  `guMtxConcat` (paired-single asm) loads its {0, 1} constant with a 12-bit `psq_l` that the linker
  fills with a 16-bit small-data offset. In the built `main.dol` that instruction reads a random
  `.sbss` variable instead of 0, so every model matrix got that value times the camera position
  added to one axis. The engine now does all matrix maths in plain C and calls none of libogc's
  paired-single helpers (checked by decoding the shipped DOL, not just the source).
- Cutscene animation (`test/gesturetest.c`, `test/gesture_ref.py`): on Trainstation 1, 2 and 5 every
  gesture a scene names resolves on a skeleton and moves it, with no NaNs; every humanoid skeleton
  turns its head for look-at without moving its root; over the first minute of each map's scripted
  scenes 15/40/2 gestures play. The engine's layered poses were checked against an independent
  Python implementation of Source's AccumulatePose on the raw MDL data (full frame rate): Kleiner's
  gesture, posture and typing sequences within 0.2 units, the vortigaunt's sweep within 0.6 (the
  packs store 10 fps).
- Trains, queues, turnstiles, combat (`test/traintest.c`, `test/queuetest.c`, `test/combattest.c`):
  on Trainstation 1 the player can walk around the moving car, the train stops with its door open,
  both passengers end up on the platform never inside solid, and the player walks out of the door;
  the turnstile turns about 150 degrees and lets the player through from its entry side; on
  Trainstation 2 the queue's front citizen leaves and the rest move up; on Canals 1 all three armed
  Metro Police in view of the player attack, while on Trainstation 1 nobody does (precriminal).
- Menu (`test/menutest.c`, `test/menugxtest.c`): with the real `menu.bin` and `xmenu.bin`, the
  title screen's music starts, the slide-in whoosh plays on frame 4 and the settle on frame 18,
  input waits for the items; Options -> Game / Controller / Buttons and back work, values change,
  chapter select starts chapter 2 on d1_trainstation_05 and stops the music; the pause menu has no
  music and B resumes; Quit and the error box work. Everything drawn once settled is on screen.
  The real `menu.c` + `text.c` against the GX validator: 0 stream errors over 660 frames.
- NPC fixes (`test/linkcheck.c`, `test/sitchk.c`, `test/runtilt.c`, `test/seqtilt.c`):
  - Doorways: prop collision used every node of the .phy files' ledge trees, including the inner
    nodes, which are only bounding hulls for culling; those were the invisible walls. Only the
    leaves are real pieces now. With the NPC collision mask (player clips don't stop NPCs, as in
    Source), the node-graph links blocked by world or props drop to 1/6/3/4/0/4 on Trainstation
    1-5 and Canals 1 (from 10/32/9/21/1/8 counting player clips, 13/42/-/29/-/20 before); what's
    left is mostly closed doors and physics props, which open or get pushed in play.
  - Sitting: act-busy hints by activity (ACT_BUSY_SIT_CHAIR, ACT_BUSY_SIT_GROUND...) now play the
    model's sequence for it, and NPCs that npc_template_makers hand to an act-busy goal pick a hint.
    On Trainstation 2/3, citizens sit on the plaza benches, the floor and the couch.
  - Staring at the sky: scene look-ats aimed 64 units above every target, so an NPC told to look
    at a mark or info_target looked over it (up to the 40-degree limit); and "!self" in a scene
    resolved to the scene entity. Point entities are now looked at where they are, NPCs and the
    player at their eyes, and idle glances need the player in sight on the same level. Over a
    minute of every map's scenes, no humanoid NPC has its face more than 25 degrees up for more
    than a tenth of the time (2 antlion guards show up; their rig isn't humanoid).
  - Barney's helmet and faceplate: single-bone props are baked in their idle pose (rotated 90
    degrees from the reference mesh), so the mask sits on his head the right way.
- Sound (`test/soundtest.c`): soundscapes from `scripts/soundscapes*.txt` (looping beds, random
  distant sounds, positioned sounds, their DSP room) switch as the player moves, like Source's
  env_soundscape; every sound placed in the world gets room reverb (the soundscape's dsp preset, or
  an automatic room measured from the space around the listener), walls muffle as well as quieten,
  under water muffles everything. New effects: NPC footsteps and other animation sound events,
  train/elevator/fan/button start-loop-stop sounds, physics impacts by surface, bullet impacts by
  surface (the player's and NPCs' misses).
- Props (`test/proppush.c`): NPCs shove physics props up to 250 kg out of their way instead of
  stopping; on Trainstation 1/2 and Canals 1 they got past 25 of 25 light props.
- Performance (`test/rendertest.c`): props and NPCs are culled against the full view frustum, by
  their map fade distance and when under ~1.5 pixels; vertices per frame on Trainstation 1 dropped
  from about 101,000 to 43,000 (Trainstation 2: 58,000 to 35,000; Canals 1: 72,000 to 29,000).
- Fixes: the tram's sliding doors keep the train's rotation (were left at their spawn angle when
  the car turned); the G-Man intro's background train camera follows its path with pitch and
  blended turns (orientation types 2/3), as on PC; chapter titles with quotes ("A RED LETTER DAY"
  etc.) and chapters 10-13 (were shifted by the missing 9a) are right, and previews are 256x144
  from the full-size art; NPCs ignore the player on the title screen's background map.
- The cop gate on Trainstation 1 (`test/takeaway.c`): the gate between the two cops is a player clip
  that the map disables when the citizen being led away walks through the NPC trigger in Barney's
  hallway. Triggers only ever reacted to the player, so it never opened (the "invisible wall").
  Triggers now go by their flags like Source's (clients, NPCs, ally NPCs, everything) and by their
  filter (filter_activator_class / _name / _multi, negated or not), with the toucher as the
  activator; the gate opens and the player walks through.
- Sitting (`test/sitchk.c`): act-busy spots play scripts/actbusy.txt's entry sequence before the busy
  one and its exit sequence when let go, and sequences played by scripts, scenes and act-busy move the
  NPC by their own root motion (Source's AutoMovement). Sitting down on a bench moves the NPC back
  onto the seat (19 units on Trainstation 2's plaza benches); before they sat where they stood, in the
  air in front of it.
- Pushers (`test/pushtest.c`, `test/pushtest2.c`, `test/traintest.c`): whatever stands on a moving
  door, lift or train moves (and turns) with it by what it stands on, not by being somewhere inside its
  box, and what a moving brush runs into is shoved out of the way by the distance that part of it
  moved, with its "dmg" when pinned, like Source's pushers. NPCs ride and get pushed the same way.
- The G-Man's void: tools/toolsblack faces are drawn (black) again; they were dropped with the other
  tool textures, so the intro saw through the walls of his room. His light comes from the map's
  light_dynamic lights riding around his head (`test/gmanlight.c`), now supported for models.
- point_spotlight beams are as wide at both ends (the cone is in the texture) at Source's brightness.
- Screen effects (`test/fxtest.c`): materials' proxies run each frame from programs the converter
  compiles (tools/matfx.py): TextureScroll, AnimatedTexture (every frame of the VTF), Sine, LinearRamp,
  GaussianNoise / UniformNoise, the arithmetic ones and TextureTransform, which is everything HL2's
  monitors use (scrolling scanlines, flicker, TV static, animated screens). UnlitGeneric and
  UnlitTwoTexture draw unlit, the second texture with its own transform, and $selfillum adds the base
  texture masked by its alpha.
- The menu's legend bar shows the GameCube A and B buttons (tools/assets, from the SVGs supplied).
- The G-Man intro's backgrounds (`test/introcam.c`, `test/rendertest.c`): the script_intro's
  camera entities ride inside small brush boxes (the HL1 room's `hl1room_brush`, the train's
  `background_train`). Source draws brush faces only from the front, so its cameras see out of them;
  ours drew every face both ways and the cameras saw the inside of the box. Brush entities' faces
  are now drawn only from their front, like Source. The blend passes take luminance the shader's
  way, (r+g+b)/3, from the colour copies through TEV swap tables, instead of an I8 copy's video Y
  (16-235 and green-heavy), which skewed modes 1 and 3 and let a black scene through mode 5.
- The HL1 test chamber (`test/hl1light.c`): its lights are switchable ones the map turns on at the
  start (styles 33/34); brush entities (the rotating rig and the beams' rails) now get their light
  style layers like the world's, so they're lit too.
- Chapter titles: the localisation reader lost step after the first escaped quote, so chapters
  3-5 and 7-10 had no names; every line is read directly now. titles.txt (in hl2_misc.vpk, not
  loose) is read, so env_message titles resolve ("CHAPTER1_TITLE" was shown as is), and its effect 2
  writes the title out letter by letter in the highlight colour like the PC. Each chapter's map shows
  its title a second in (worldspawn "chaptertitle", once), and the New Game list starts chapters 4,
  8, 9, 9a and 10 on the PC's maps (d1_canals_06, d2_coast_09, d2_prison_02, d2_prison_06, d3_c17_02).
- Pacing: without the HEV suit the player only walks (150, CHL2_Player), as Trainstation's scripts
  are timed for; at 190 he got everywhere a fifth sooner. Scenes with "busyactor" 1 (331 of HL2's
  752) wait until their actors stop talking before they start, like CSceneEntity::CheckActors.
- The train's passengers (`test/traincit.c`): a citizen parented to a func_tracktrain doesn't move
  (CNPC_Citizen): citizen_train_2 sits (d1_t01_TrainRide_Sit_Idle), the others stand, until the
  train arrives and unparents them, when they get up with the _Exit sequences. Friendly NPCs give way
  to the player only when Source's would: not with "ignore player push", not while talking, in a
  script, scene or act-busy, and not on a train or lift (they walked in place there).
- Riding (`test/ridejitter.c`): the view interpolates between ticks; it now does so relative to the
  train it rides, so it no longer lags the car by a varying part of a tick (0.5 units a frame on
  average, up to 10, now 0). The mover a player rides no longer shoves him a second time, and trains
  that orient "at path tracks" face along their wheelbase (CFuncTrackTrain) instead of snapping at
  each path_track.
- Monitors (`test/fxtest.c`): `_rt_Camera` screens run their material effects too: the scanlines /
  static of $texture2 scroll over the camera picture with the proxies' flicker (3 on Trainstation 1).
- Pause (`test/audiotest.c`): the pause menu pauses every playing sound and line where it is and
  resumes them after, instead of letting dialogue run on or cutting sounds off; channels held by
  the pause aren't taken for new sounds.
- Barney's door on Trainstation 1 (`test/cageprobe.c`): NPC clips named for one NPC
  (`func_brush` with `excludednpc`, e.g. `barney_hallway_clip`) now let that NPC through, so
  Barney goes to his mark and back instead of standing stuck in the doorway (that was the
  invisible wall); the player walks 163 units through the door. The player clip between the cops is
  disabled by the map once the citizen ahead of you is led through (see the cop gate above).
- Model lighting (`test/lighttest.c`, `test/rendertest.c`): models are lit the way Source lights
  them: per vertex from their normals with the ambient cube of where they stand (six GX hardware
  lights in specular-attenuation mode reproduce Source's squared-normal weighting), plus the direct
  light of every worldlight that can see them (point, spot, light-emitting surfaces and the sun,
  traced against the map, folded into the cube like Source does). Before, props, characters and
  the viewmodel were flat-shaded with one colour, characters kept the colour of where they spawned,
  and the ambient cubes were read 255 times too dark and then normalised per map. Compared with the
  floor lightmap under each static prop (linear, cube top / floor), the median went from 0.70 /
  0.19 / 0.16 / 0.07 (ambient only) to 1.61 / 1.10 / 0.99 / 1.06 on Trainstation 2, Canals 1, Coast
  1 and Town 1. Moving things are re-lit when they move 24 units or every 90 frames, within a
  per-frame trace budget, and ease between cubes. The flashlight now lights models too (a hardware
  spotlight in a second colour channel).
- Switchable and flickering lights (`test/styletest.c`): faces lit by named lights or light-style
  patterns get their extra lightmaps as additive layers (stored as the difference they make in the
  lightmaps' gamma, so a light that's on looks exactly like one baked lightmap). Lights start on or
  off like the map says, `light`/`light_spot` TurnOn/TurnOff/Toggle/SetPattern switch them, and
  Source's 13 patterns (flicker, candle, fluorescent...) animate at 10 letters a second. Before,
  every light that could be switched was missing from the world (e.g. 7 of Trainstation 1's 12
  named lights start on). Models get the same lights through the worldlights' styles.
- Shots into the sky make no impact effect or sound (sky faces carry their own surface id).
- Performance: traces test static props through a 256-unit grid instead of every prop (identical
  results over 200,000 random sweeps on Trainstation 2 and Canals 1, ~40% faster on the host);
  static props' rotations are worked out once per map; big models get a far LOD that takes over
  beyond 12 bounding radii (props' vertices per frame on Trainstation 2: 23,200 to 20,750). Far LODs
  are dropped before textures are shrunk when a map is too big for memory. Model vertices are 14
  bytes (16-bit positions with as many fraction bits as each model's size allows, 1/256 unit for
  most props) instead of 16 bytes without normals / 20 with: a map with 90,000 model vertices saves
  about 540 KB, more than the normals and light data cost, so textures don't have to shrink for them.
- Milestone 7 (`test/worldtest.c`): the intro blends camera and normal views and fades; the G-Man
  faces the camera in every sample; the player swims to level 3; 12 s under water leaves 20 aux,
  15 s more drowns to 50 health (Source's numbers); props float; breakables shatter into gibs; the
  airboat drives on canal water and is carried into the next map; the buggy drives and you get out.
- The intro (`test/introtest.c`): the G-Man's cameras, his speech with a moving face, the train
  ride, and the player getting control back at 61 s.

## Roadmap

1. ~~World viewer~~ (milestone 1)
2. ~~Walking, collision, level transitions~~ (milestone 2)
3. ~~Props, entities, map scripts~~ (milestone 3)
4. ~~NPCs, scenes, dialogue and captions~~ (milestone 4)
5. ~~Menu, on-screen text, live screens, physics props, doors~~ (milestone 5)
6. ~~Weapons, items, suit and HUD, cutscenes, faces, positional speech, cheats~~ (milestone 6)
7. ~~HEV suit and aux power, water, breakables, vehicles, PC-accuracy fixes~~ (milestone 7)
8. ~~Pathfinding, basic combat, ladders, vehicle guns, HUD layout~~ (milestone 8)
9. **Saving and loading**, Source-like squad combat (cover, grenades, flanking), NPC weapon models
   and muzzle flashes, the HUD's pickup history and icon fonts.

## Licence notes

The engine and tools are original code. The Source SDK is under Valve's Source 1 SDK Licence
(free, non-commercial distribution only), so anything ported from it later inherits those
terms: hl2gc must stay free. Never distribute `.hgm` packs, `vo.bin`, `font*.bin`, `menu.bin` or `xmenu.bin`; they
contain converted game assets. Every player generates their own from their own copy.
