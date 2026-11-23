/*
 * hl2gc - a GameCube engine for Half-Life 2 maps converted from the player's own copy.
 * Milestone 2: walk the campaign. HL2-style player movement and collision, level transitions.
 *
 * Milestone 3: props, entities and map scripts (Source's input/output system).
 * Milestone 4: NPCs - animated characters, scripted sequences, choreographed scenes with voices
 *              and captions.
 *
 * Milestone 6: weapons, items, health and suit, cheats.
 * Milestone 7: HEV suit, water, breakables, vehicles; the PC's startup movie, loading screens and
 *              error dialogs (codes HL2GC-Exx, listed in README).
 *
 * Controls: stick = move, C-stick = look, A = jump, B (hold) = sprint, X = use / pick up,
 *           R = fire (throws what you carry), L = secondary fire, Y = reload (throws too), Z = crouch,
 *           D-pad left/right = previous/next weapon, D-pad down = last weapon, D-pad up = flashlight,
 *           Start = menu
 *           (no clipping and the other cheats are in the pause menu).
 * Maps load from the disc (files/hl2gc/maps/, also Dolphin's extracted-disc folder) or SD.
 */
#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>
#include <sdcard/gcsd.h>

#include "audio.h"
#include "crash.h"
#include "caption.h"
#include "collide.h"
#include "disc.h"
#include "ent.h"
#include "pack.h"
#include "physics.h"
#include "menu.h"
#include "player.h"
#include "text.h"
#include "face.h"
#include "render.h"
#include "light.h"
#include "fx.h"
#include "weapons.h"
#include "intro.h"
#include "loadscreen.h"
#include "gibs.h"
#include "vehicle.h"
#include "tanks.h"
#include "world.h"
#include "soundscape.h"

#define FIFO_SIZE (256 * 1024)
#define DEFAULT_MAP "d1_trainstation_01"

static void *xfb[2];
static GXRModeObj *rmode;
static const char *sdDev;
static int haveDisc;

static Pack pack;
static World world;
static int mapLoaded;
static char mapName[32];
static u8 *triggerArmed;
static Player *thePlayer;
static int newGame = 1;

/* the 3D skybox (sky_camera): its own visibility, found once the map's entities are up */
static World skyWorld;
static int skyReady, skyOn;
static f32 skyOrg[3], skyScale, skyFogS, skyFogE;
static int skyFogOn;
static u8 skyFogCol[3];

static void sky_find(void)
{
	skyReady = 1;
	skyOn = 0;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		if (strcmp(ents[i].classname, "sky_camera"))
			continue;
		const char *sc = Ent_KV((s32)i, "scale");
		skyScale = sc && atof(sc) > 0 ? (f32)atof(sc) : 16.0f;
		memcpy(skyOrg, ents[i].origin, 12);
		const char *fe = Ent_KV((s32)i, "fogenable");
		skyFogOn = fe && atoi(fe) != 0;
		const char *fs_ = Ent_KV((s32)i, "fogstart"), *fe_ = Ent_KV((s32)i, "fogend"), *fc = Ent_KV((s32)i, "fogcolor");
		skyFogS = (fs_ ? (f32)atof(fs_) : 0) / skyScale;
		skyFogE = (fe_ ? (f32)atof(fe_) : 4000) / skyScale;
		int r = 128, g = 128, b = 128;
		if (fc)
			sscanf(fc, "%d %d %d", &r, &g, &b);
		skyFogCol[0] = (u8)r, skyFogCol[1] = (u8)g, skyFogCol[2] = (u8)b;
		skyOn = World_PointCluster(&pack, skyOrg) >= 0;
		break;
	}
}

/* this frame's sky view: the player's view, from sky_camera + eye / scale (CSkyboxView::Setup) */
static void sky_frame(const Camera *cam)
{
	if (!skyReady)
		sky_find();
	if (!skyOn) {
		Render_SetSky(NULL, NULL, NULL, 0, 0, 0, NULL);
		return;
	}
	Camera sc = *cam;
	sc.origin = (guVector){skyOrg[0] + cam->origin.x / skyScale, skyOrg[1] + cam->origin.y / skyScale,
	                       skyOrg[2] + cam->origin.z / skyScale};
	f32 so[3] = {sc.origin.x, sc.origin.y, sc.origin.z};
	if (World_PointCluster(&pack, so) < 0) /* (outside the skybox's room: look from the sky camera itself) */
		sc.origin = (guVector){skyOrg[0], skyOrg[1], skyOrg[2]};
	Camera_Update(&sc);
	World_Collect(&skyWorld, &sc, 0);
	Mtx sv;
	guVector t = {sc.origin.x + sc.forward.x, sc.origin.y + sc.forward.y, sc.origin.z + sc.forward.z};
	guLookAt(sv, &sc.origin, &sc.up, &t);
	Render_SetSky(&skyWorld, &sc, sv, skyFogOn, skyFogS, skyFogE, skyFogCol);
}
static char pendingMap[32], pendingLandmark[32];
static FILE *voFile;
static u8 *fontData;

/* Every map pack is read into one block reserved at boot, sized for the biggest pack on the disc.
 * Allocating each map separately split the heap up (sounds, buffers allocated while a map ran sat
 * after it), so a big map could fail to fit even with enough memory free in total. */
static u8 *mapArena;
static int gxReady, fontsReady;
static FILE *introFile;
static u32 mapArenaSize;
#define FALLBACK_ARENA (13 * 1024 * 1024) /* tools/convert_all.py MAX_PACK_BYTES */

/* the last error, PC-style: a code and a message for the dialog */
static char errCode[16], errText[320];

static u32 free_kb(void)
{
	struct mallinfo mi = mallinfo();
	u32 top = (u32)SYS_GetArena1Hi() - (u32)SYS_GetArena1Lo();
	return (u32)(mi.fordblks + top) / 1024;
}

static void set_error(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_error(int code, const char *fmt, ...)
{
	snprintf(errCode, sizeof(errCode), "HL2GC-E%02d", code);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errText, sizeof(errText), fmt, ap);
	va_end(ap);
	printf("%s: %s\n", errCode, errText);
}

/* voice bank reader: disc first, SD otherwise */
static int read_vo(u32 offset, u32 len, void *buf)
{
	if (haveDisc)
		return Disc_ReadRange("hl2gc/vo.bin", offset, len, buf);
	if (!voFile)
		return -1;
	if (fseek(voFile, (long)offset, SEEK_SET) != 0)
		return -1;
	return fread(buf, 1, len, voFile) == len ? 0 : -1;
}

/* a whole file from hl2gc/ on the disc or SD card, 32-byte aligned */
static u8 *read_hl2gc_file(const char *name, u32 *size)
{
	char path[96];
	u8 *data = NULL;
	*size = 0;
	if (haveDisc) {
		snprintf(path, sizeof(path), "hl2gc/%s", name);
		if (Disc_ReadFile(path, &data, size) == 0)
			return data;
		return NULL;
	}
	if (!sdDev)
		return NULL;
	snprintf(path, sizeof(path), "%s:/hl2gc/%s", sdDev, name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = (u32)ftell(f);
	fseek(f, 0, SEEK_SET);
	data = memalign(32, (*size + 31) & ~31u);
	if (data && fread(data, 1, *size, f) != *size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

static void init_frontend(void)
{
	static const char *files[] = {"font_hud.bin", "font_menu.bin", "font_title.bin", "font_icons.bin", "font_cross.bin"};
	static const int ids[] = {FONT_HUD, FONT_MENU, FONT_TITLE, FONT_ICONS, FONT_CROSS};
	for (int i = 0; i < 5; i++) {
		u32 sz;
		u8 *d = read_hl2gc_file(files[i], &sz);
		if (!d || Text_LoadFont(ids[i], d, sz) != 0)
			printf("text: %s missing\n", files[i]);
	}
	u32 sz;
	u8 *d = read_hl2gc_file("menu.bin", &sz);
	if (!d || Menu_Load(d, sz) != 0)
		printf("menu: menu.bin missing\n");
	d = read_hl2gc_file("xmenu.bin", &sz);
	if (!d || Menu_LoadXbox(d, sz) != 0)
		printf("menu: xmenu.bin missing (no Xbox menu sounds)\n");
	d = read_hl2gc_file("weapons.hgm", &sz);
	if (!d || Weap_Load(d, sz) != 0)
		printf("weapons: weapons.hgm missing\n");
	d = read_hl2gc_file("loading.bin", &sz);
	if (!d || Load_Init(d, sz) != 0)
		printf("loading.bin missing\n");
	fontsReady = Text_HasFont(FONT_MENU);
}

/* the startup movie (hl2gc/intro.bin) */
static int read_intro(u32 offset, u32 len, void *buf)
{
	if (haveDisc)
		return Disc_ReadRange("hl2gc/intro.bin", offset, len, buf);
	if (!introFile)
		return -1;
	if (fseek(introFile, (long)offset, SEEK_SET) != 0)
		return -1;
	return fread(buf, 1, len, introFile) == len ? 0 : -1;
}

/* the map arena: as big as the biggest map pack, reserved before anything else is allocated */
static int reserve_map_arena(void)
{
	u32 biggest = 0;
	if (haveDisc)
		biggest = Disc_LargestFile("hl2gc/maps");
	if (!biggest)
		biggest = FALLBACK_ARENA;
	mapArenaSize = (biggest + 31) & ~31u;
	mapArena = memalign(32, mapArenaSize);
	if (!mapArena) {
		set_error(7, "Not enough memory for the map data: the biggest map needs %u KB, %u KB free. Convert the maps "
		             "again with the current tools.", mapArenaSize / 1024, free_kb());
		return -1;
	}
	return 0;
}

/* sounds behind walls are muffled: the world (and closed doors) between the listener and the sound */
static int sound_occluded(const f32 a[3], const f32 b[3])
{
	if (!mapLoaded)
		return 0;
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_SetSkipBoxes(1);
	CM_BoxTrace(&tr, a, b, z, z, CONTENTS_SOLID);
	CM_SetSkipBoxes(0);
	return tr.fraction < 0.97f && !tr.startsolid;
}

static void init_sound_and_captions(void)
{
	char path[64];
	u32 size = 0;
	if (haveDisc) {
		if (Disc_ReadFile("hl2gc/font.bin", &fontData, &size) != 0)
			fontData = NULL;
	} else if (sdDev) {
		snprintf(path, sizeof(path), "%s:/hl2gc/vo.bin", sdDev);
		voFile = fopen(path, "rb");
		snprintf(path, sizeof(path), "%s:/hl2gc/font.bin", sdDev);
		FILE *f = fopen(path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			size = (u32)ftell(f);
			fseek(f, 0, SEEK_SET);
			fontData = memalign(32, (size + 31) & ~31u);
			if (fontData && fread(fontData, 1, size, f) != size) {
				free(fontData);
				fontData = NULL;
			}
			fclose(f);
		}
	}
	if (!fontData || Caption_Init(fontData, size) != 0)
		printf("captions: no font (hl2gc/font.bin)\n");
	Render_SetScreen(rmode->fbWidth, rmode->efbHeight);
	Audio_Init(read_vo);
	Audio_SetOcclusion(sound_occluded);
}

/* A fatal error (the game can't go on): the PC shows an "Engine Error" box and quits. Drawn with
 * the game's fonts when they're loaded, on the text console otherwise. */
static void fatal(void)
{
	if (gxReady && fontsReady) {
		static f32 proj2d[4][4];
		guOrtho(proj2d, 0, rmode->efbHeight, 0, rmode->fbWidth, 0, 1);
		for (int fb = 0;; fb ^= 1) {
			Text_Begin(rmode->fbWidth, rmode->efbHeight);
			Text_Rect(0, 0, rmode->fbWidth, rmode->efbHeight, 0x000000FF);
			f32 w = rmode->fbWidth, h = rmode->efbHeight, x0 = w * 0.5f - 240, y0 = h * 0.5f - 90;
			Text_Rect(x0, y0, x0 + 480, y0 + 180, 0x2E2E2EF0);
			Text_Rect(x0, y0, x0 + 480, y0 + 1, 0x787878FF);
			Text_Draw(FONT_MENU, x0 + 12, y0 + 6, "Engine Error", -1, 0xEBEBEBFF, 0.72f);
			Text_Draw(FONT_MENU, x0 + 20, y0 + 40, errCode, -1, 0xE6A01EFF, 0.6f);
			/* wrap the message */
			const char *p = errText;
			f32 y = y0 + 64;
			while (*p && y < y0 + 150) {
				int n = 0, sp = -1;
				while (p[n] && n < 120) {
					if (p[n] == ' ')
						sp = n;
					if (Text_Width(FONT_MENU, p, n + 1, 0.55f) > 440 && sp > 0) {
						n = sp;
						break;
					}
					n++;
				}
				Text_Draw(FONT_MENU, x0 + 20, y, p, n, 0xDCDCDCFF, 0.55f);
				p += n;
				while (*p == ' ')
					p++;
				y += Text_LineHeight(FONT_MENU, 0.55f) + 2;
			}
			Text_Draw(FONT_MENU, x0 + 20, y0 + 152, "Press START to exit.", -1, 0xA0A0A0FF, 0.5f);
			Text_End(proj2d);
			GX_CopyDisp(xfb[fb], GX_TRUE);
			GX_DrawDone();
			VIDEO_SetNextFramebuffer(xfb[fb]);
			VIDEO_SetBlack(FALSE);
			VIDEO_Flush();
			VIDEO_WaitVSync();
			PAD_ScanPads();
			if (PAD_ButtonsDown(0) & PAD_BUTTON_START)
				exit(0);
		}
	}
	Crash_Disarm();
	console_init(xfb[0], 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	printf("\n\n  Engine Error\n\n  %s\n  %s\n\n  Press START to exit.\n", errCode, errText);
	for (;;) {
		PAD_ScanPads();
		if (PAD_ButtonsDown(0) & PAD_BUTTON_START)
			exit(0);
		VIDEO_WaitVSync();
	}
}

/* SD2SP2 first (serial port 2 adapter), then SD Gecko style adapters in memory card slots */
static const char *mount_sd(void)
{
	static const struct {
		const char *name;
		const DISC_INTERFACE *io;
	} devs[] = {
		{"sd2", &__io_gcsd2},
		{"carda", &__io_gcsda},
		{"cardb", &__io_gcsdb},
	};
	for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
		if (fatMountSimple(devs[i].name, devs[i].io)) {
			printf("mounted %s:\n", devs[i].name);
			return devs[i].name;
		}
	}
	return NULL;
}

static void init_video(void)
{
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	Crash_Init(xfb[0], xfb[1], rmode->fbWidth, rmode->xfbHeight);
	/* no boot text: the screen stays black until the startup movie, like the PC */
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

static void init_gx(void)
{
	void *fifo = memalign(32, FIFO_SIZE);
	memset(fifo, 0, FIFO_SIZE);
	GX_Init(fifo, FIFO_SIZE);
	GX_SetCopyClear((GXColor){0, 0, 0, 0xFF}, GX_MAX_Z24); /* Source clears to black */
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	u32 xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering, (rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetDispCopyGamma(GX_GM_1_0);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_FALSE);
}

/* ------------------------------------------------------------------ maps */

static void unload_map(void)
{
	if (!mapLoaded)
		return;
	Weap_LevelEnd();
	Ent_Free();
	Light_Shutdown();
	Fx_Shutdown();
	CM_Shutdown();
	Render_Free();
	World_Free(&world);
	World_Free(&skyWorld);
	skyReady = skyOn = 0;
	free(triggerArmed);
	triggerArmed = NULL;
	memset(&pack, 0, sizeof(pack)); /* its data lives in mapArena, reused by the next map */
	mapLoaded = 0;
}

static void (*loadProgress)(f32 frac);

/* reads a map pack into the arena, reporting progress; 0 or an error code (errCode/errText set) */
static int read_map(const char *name, u32 *outSize)
{
	char path[96];
	u32 size = 0;
	FILE *f = NULL;
	if (haveDisc) {
		snprintf(path, sizeof(path), "hl2gc/maps/%s.hgm", name);
		if (Disc_FileSize(path, &size) != 0)
			size = 0;
	}
	if (!size && sdDev) {
		snprintf(path, sizeof(path), "%s:/hl2gc/maps/%s.hgm", sdDev, name);
		f = fopen(path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			size = (u32)ftell(f);
			fseek(f, 0, SEEK_SET);
		}
	}
	if (!size) {
		set_error(1, "Couldn't find map %s (hl2gc/maps/%s.hgm). Convert it with tools/convert_all.py and "
		             "run build.bat again.", name, name);
		return 1;
	}
	if (size > mapArenaSize) {
		if (f)
			fclose(f);
		set_error(2, "Map %s is too big for the GameCube's memory: %u KB, room for %u KB. Convert it again "
		             "with the current tools (they keep maps under the limit).", name, size / 1024, mapArenaSize / 1024);
		return 2;
	}
	const u32 step = 256 * 1024;
	for (u32 pos = 0; pos < size; pos += step) {
		u32 n = size - pos < step ? size - pos : step;
		int bad = f ? fread(mapArena + pos, 1, n, f) != n : Disc_ReadRange(path, pos, n, mapArena + pos) != 0;
		if (bad) {
			if (f)
				fclose(f);
			set_error(3, "Couldn't read map %s from the %s (at %u of %u KB). The disc image or card may be "
			             "damaged; build the disc again.", name, f ? "SD card" : "disc", pos / 1024, size / 1024);
			return 3;
		}
		if (loadProgress)
			loadProgress(0.75f * (pos + n) / size);
	}
	if (f)
		fclose(f);
	*outSize = size;
	return 0;
}

static int load_map_inner(const char *name);
static int load_map(const char *name)
{
	Crash_Map(name);
	Crash_Loading(1);
	CRASH_STAGE("loading the map");
	int r = load_map_inner(name);
	Crash_Loading(0);
	return r;
}

static int load_map_inner(const char *name)
{
	Scape_Stop();
	unload_map();
	Audio_FlushCache(); /* cached sounds would otherwise sit in the middle of the next map's memory */
	Gibs_Clear();
	u32 size;
	int e = read_map(name, &size);
	if (e)
		return e;
	int r = Pack_LoadMemory(&pack, mapArena, size);
	if (r == -2) {
		set_error(4, "Map %s was converted by a different version of hl2gc (pack version %u, this build reads "
		             "version %u). Convert the maps again and run build.bat.", name, *(u32 *)(mapArena + 4),
		          (unsigned)PACK_VERSION);
		return 4;
	}
	if (r != 0) {
		set_error(4, "Map %s isn't a valid hl2gc map pack. Convert it again and run build.bat.", name);
		return 4;
	}
	if (loadProgress)
		loadProgress(0.8f);
	const char *what = NULL;
	skyReady = 0;
	if (World_Init(&world, &pack) != 0 || World_Init(&skyWorld, &pack) != 0)
		what = "visibility";
	else if (Render_Init(&pack) != 0)
		what = "rendering";
	else if (CM_Init(&pack) != 0)
		what = "collision";
	if (!what) {
		Light_Init(&pack); /* (static props get their direct light: traces against the map) */
		Fx_Init(&pack);
	}
	if (what) {
		set_error(5, "Out of memory setting up map %s (%s). %u KB free.", name, what, free_kb());
		return 5;
	}
	/* the map's data went where the last map's was: drop anything the GPU cached from there */
	GX_InvVtxCache();
	GX_InvalidateTexAll();
	triggerArmed = calloc(pack.numTriggers ? pack.numTriggers : 1, 1);
	snprintf(mapName, sizeof(mapName), "%s", name);
	mapLoaded = 1;
	if (loadProgress)
		loadProgress(0.9f);
	return 0;
}

/* scripts ask for level changes from inside the entity tick: do it once the tick is over */
static void request_change_level(const char *map, const char *landmark)
{
	snprintf(pendingMap, sizeof(pendingMap), "%s", map);
	snprintf(pendingLandmark, sizeof(pendingLandmark), "%s", landmark);
}

static const EntCallbacks entCallbacks = {request_change_level, Weap_Strip, Weap_Hurt, Weap_HurtFrom, Weap_GeigerRange};

/* where this level began, to come back to after dying (the PC game reloads its autosave) */
static Player levelStartPlayer;
static int levelNewGame;

/* Source's automatic room DSP: how big the space around the ears is and how open to the sky, from a
 * fan of traces a few times a second */
static void room_probe(const f32 ear[3], f32 dt)
{
	static f32 timer;
	timer -= dt;
	if (timer > 0)
		return;
	timer = 0.3f;
	static const f32 dirs[][3] = {{1, 0, 0},     {-1, 0, 0},    {0, 1, 0},     {0, -1, 0},     {0.7f, 0.7f, 0},
	                              {-0.7f, 0.7f, 0}, {0.7f, -0.7f, 0}, {-0.7f, -0.7f, 0}, {0, 0, 1}, {0.6f, 0, 0.8f},
	                              {-0.6f, 0, 0.8f}, {0, 0.6f, 0.8f}, {0, -0.6f, 0.8f}};
	const int n = sizeof(dirs) / sizeof(dirs[0]);
	f32 total = 0;
	int open = 0, up = 0;
	f32 z[3] = {0, 0, 0};
	CM_SetSkipBoxes(1);
	for (int i = 0; i < n; i++) {
		f32 end[3] = {ear[0] + dirs[i][0] * 2048, ear[1] + dirs[i][1] * 2048, ear[2] + dirs[i][2] * 2048};
		Trace tr;
		CM_BoxTrace(&tr, ear, end, z, z, CONTENTS_SOLID);
		f32 d = tr.startsolid ? 0 : tr.fraction * 2048;
		total += d;
		if (dirs[i][2] > 0.5f) {
			up++;
			if (tr.fraction >= 1 || d > 900) /* (the sky brushes are high above in the open) */
				open++;
		}
	}
	CM_SetSkipBoxes(0);
	Audio_SetAutoRoom(total / n, up ? (f32)open / up : 0);
}

static int start_entities(void)
{
	levelNewGame = newGame;
	levelStartPlayer = *thePlayer;
	int r = Ent_Spawn(&pack, thePlayer, newGame, &entCallbacks);
	newGame = 0;
	if (r == 0) {
		Weap_LevelStart(&pack, thePlayer);
		Veh_LevelStart(&pack, thePlayer);
		Scape_Init(&pack);
	} else {
		set_error(6, "Out of memory creating the characters and objects of map %s. %u KB free.", mapName, free_kb());
	}
	if (loadProgress)
		loadProgress(1.0f);
	return r;
}

static int touching(const Player *pl, const PackTrigger *t)
{
	f32 mn[3], mx[3];
	Player_Bounds(pl, mn, mx);
	for (int i = 0; i < 3; i++)
		if (pl->origin[i] + mx[i] < t->mins[i] || pl->origin[i] + mn[i] > t->maxs[i])
			return 0;
	return 1;
}

/* A trigger only fires once the player has been outside it, so arriving inside the
 * new map's return trigger doesn't bounce straight back. */
static void arm_triggers(const Player *pl)
{
	for (u32 i = 0; i < pack.numTriggers; i++)
		triggerArmed[i] = !touching(pl, &pack.triggers[i]);
}

static int in_world(const Player *pl)
{
	f32 c[3] = {pl->origin[0], pl->origin[1], pl->origin[2] + 36.0f};
	return World_PointCluster(&pack, c) >= 0;
}

static const PackLandmark *find_landmark(const char *name)
{
	for (u32 i = 0; i < pack.numLandmarks; i++)
		if (strcmp(pack.landmarks[i].name, name) == 0)
			return &pack.landmarks[i];
	return NULL;
}

/* 0 = arrived; otherwise the error is in errCode/errText and the player is back where they were
 * (or, if even that failed, nothing is loaded: fatal) */
static int change_level(Player *pl, const char *mapIn, const char *markIn)
{
	char map[32], mark[32];
	snprintf(map, sizeof(map), "%s", mapIn);
	snprintf(mark, sizeof(mark), "%s", markIn);
	f32 rel[3] = {0, 0, 0};
	const PackLandmark *from = find_landmark(mark);
	if (from) {
		for (int i = 0; i < 3; i++)
			rel[i] = pl->origin[i] - from->origin[i];
		Veh_PrepareCarry(from->origin); /* the airboat / buggy comes along */
		Ent_PrepareCarry(mark, from->origin); /* and the NPCs in its trigger_transition */
	}
	char prev[32];
	snprintf(prev, sizeof(prev), "%s", mapName);
	Caption_Clear();

	if (load_map(map) != 0) {
		/* the next map didn't load: go back to where we were (the PC returns to the menu with an
		 * error; here the level you were in comes back, so you can keep playing) */
		char code[16], text[320];
		snprintf(code, sizeof(code), "%s", errCode);
		snprintf(text, sizeof(text), "%s", errText);
		if (load_map(prev) != 0)
			fatal();
		pl->origin[0] -= 64 * cosf(pl->yaw * (f32)M_PI / 180);
		pl->origin[1] -= 64 * sinf(pl->yaw * (f32)M_PI / 180);
		if (start_entities() != 0)
			fatal();
		arm_triggers(pl);
		snprintf(errCode, sizeof(errCode), "%s", code);
		snprintf(errText, sizeof(errText), "%s", text);
		return -1;
	}
	/* arrive at the same spot relative to the landmark; if that's inside a wall or outside the
	 * playable space, fall back to the landmark itself, then to the map's spawn point */
	Player keep = *pl;
	const PackLandmark *to = find_landmark(mark);
	int placed = 0;
	if (to) {
		f32 dest[3];
		for (int i = 0; i < 3; i++)
			dest[i] = to->origin[i] + rel[i];
		placed = Player_Spawn(pl, dest, keep.pitch, keep.yaw) && in_world(pl);
		if (!placed)
			placed = Player_Spawn(pl, to->origin, keep.pitch, keep.yaw) && in_world(pl);
	}
	if (!placed)
		Player_Spawn(pl, pack.spawnOrigin, keep.pitch, pack.spawnAngles[1]);
	else
		memcpy(pl->velocity, keep.velocity, sizeof(pl->velocity));
	if (start_entities() != 0)
		fatal();
	if (to) {
		Veh_FinishCarry(to->origin);
		Ent_FinishCarry(to->origin);
	}
	arm_triggers(pl);
	return 0;
}

/* ------------------------------------------------------------------ input */

static f32 stick(s8 v)
{
	if (v > -16 && v < 16)
		return 0.0f;
	f32 f = (f32)v / 90.0f;
	return f > 1 ? 1 : (f < -1 ? -1 : f);
}

/* one frame of black with the startup "Loading..." box, shown while the first map loads */
static void startup_screen(f32 proj[4][4])
{
	Text_Begin(rmode->fbWidth, rmode->efbHeight);
	Text_Rect(0, 0, rmode->fbWidth, rmode->efbHeight, 0x000000FF);
	Load_DrawOverlay(LOAD_STARTUP, rmode->fbWidth, rmode->efbHeight);
	Text_End(proj);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	GX_CopyDisp(xfb[1], GX_TRUE);
	GX_DrawDone();
	VIDEO_SetNextFramebuffer(xfb[1]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

int main(int argc, char **argv)
{
	init_video();
	PAD_Init();

	haveDisc = Disc_Init() == 0;
	if (!haveDisc)
		sdDev = mount_sd();
	if (!haveDisc && !sdDev) {
		set_error(8, "No hl2gc disc or SD card found. Boot hl2gc.iso, or put the hl2gc folder on an SD card in an "
		             "SD2SP2 or memory card slot adapter.");
		fatal();
	}
	init_gx();
	gxReady = 1;
	/* before anything else, so nothing allocated later can split it */
	if (reserve_map_arena() != 0)
		fatal();
	init_sound_and_captions();
	init_frontend();
	Intro_Init(rmode->fbWidth, rmode->efbHeight);

	Camera cam = {0};
	cam.fovy = 59.0f; /* HL2's 75 degree horizontal FOV at 4:3 */
	cam.fovx = 75.0f;
	Mtx44 proj;
	guPerspective(proj, cam.fovy, 4.0f / 3.0f, 4.0f, 16384.0f);
	GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);

	/* a map on the command line starts it straight away; otherwise the startup movie, then the
	 * main menu over the chapter's background map, like on PC */
	int direct = argc > 1 && argv[1] && argv[1][0];
	if (!direct) {
		if (!haveDisc && sdDev) {
			char path[64];
			snprintf(path, sizeof(path), "%s:/hl2gc/intro.bin", sdDev);
			introFile = fopen(path, "rb");
		}
		Load_PlayMovie(read_intro, xfb[0], xfb[1], rmode->fbWidth, rmode->efbHeight, proj);
		if (introFile)
			fclose(introFile);
		introFile = NULL;
	}
	startup_screen(proj);
	const char *first = direct ? argv[1] : Menu_Background(0);
	int err = load_map(first);
	if (err == 1 && !direct) {
		first = DEFAULT_MAP; /* background maps not converted: use the first map as the backdrop */
		err = load_map(first);
	}
	if (err)
		fatal();

	static Player pl;
	thePlayer = &pl;
	Player_Spawn(&pl, pack.spawnOrigin, pack.spawnAngles[0], pack.spawnAngles[1]);
	newGame = direct;
	if (start_entities() != 0)
		fatal();
	arm_triggers(&pl);
	int playing = direct; /* 0 = main menu over a background map */
	if (!direct)
		Menu_Open(0);
	f32 prevOrigin[3], prevEye;
	memcpy(prevOrigin, pl.origin, sizeof(prevOrigin));
	prevEye = Player_EyeHeight(&pl);

	/* a load waits for one frame to be shown with its loading screen (like the PC: the last frame
	 * stays up with "Loading..." over it) */
	enum { PEND_NONE, PEND_NEWGAME, PEND_LEVEL, PEND_RESPAWN };
	int pending = PEND_NONE;
	char pendMap[32] = "", pendMark[32] = "";

	UserCmd cmd = {0};
	cmd.pitch = pl.pitch;
	cmd.yaw = pl.yaw;
	u64 last = gettime();
	f32 accum = 0;
	int fb = 0;
	RenderStats st;

	for (;;) {
		Crash_Beat();
		CRASH_STAGE("input and menu");
		u64 now = gettime();
		f32 dt = (f32)diff_usec(last, now) / 1000000.0f;
		last = now;
		if (dt > 0.1f)
			dt = 0.1f; /* after a load: don't simulate the time spent loading */

		PAD_ScanPads();
		u32 down = PAD_ButtonsDown(0), held = PAD_ButtonsHeld(0);
		u32 menuDown = down; /* (the menu reads the buttons as they are) */
		{
			/* the analog triggers count as pressed past halfway, then the player's own button layout */
			static u32 prevAnalog;
			u32 analog = (PAD_TriggerR(0) > 140 ? PAD_TRIGGER_R : 0) | (PAD_TriggerL(0) > 140 ? PAD_TRIGGER_L : 0);
			down |= analog & ~prevAnalog & ~held;
			held |= analog;
			prevAnalog = analog;
			down = Options_Remap(down);
			held = Options_Remap(held);
		}
		if (Menu_IsOpen()) {
			const char *chapterMap = NULL;
			int act = Menu_Update(menuDown, PAD_StickY(0), dt, &chapterMap);
			if (act == MA_QUIT)
				break;
			if (act == MA_GIVEALL)
				Weap_GiveAll();
			if (act == MA_NEWGAME && chapterMap) {
				Caption_Clear();
				Hud_Clear();
				Audio_StopAll();
				snprintf(pendMap, sizeof(pendMap), "%s", chapterMap);
				pending = PEND_NEWGAME;
			}
			down = held = 0; /* the menu has the controller */
		} else if (down & PAD_BUTTON_START) {
			Menu_Open(1);
			down = held = 0;
		}
		int paused = Menu_IsOpen() && playing;
		Audio_Pause(paused); /* dialogue and every other game sound wait for the game */
		f32 vo[3], va[3];
		int cutscene = Ent_ViewOverride(vo, va);
		if (cutscene)
			down &= PAD_BUTTON_START; /* cutscene camera: the player is frozen */
		if (pl.noclip != gCheats.noclip) {
			pl.noclip = gCheats.noclip;
			memset(pl.velocity, 0, sizeof(pl.velocity));
		}
		/* player_loadsaved (a failure: Alyx or Barney died, the cop caught you): back to the level's start */
		if (Ent_ReloadRequested() && !pending) {
			Caption_Clear();
			Hud_Clear();
			Audio_StopAll();
			snprintf(pendMap, sizeof(pendMap), "%s", mapName);
			pending = PEND_RESPAWN;
		}
		int dead = Weap_DeadTime() > 0;
		if (dead) {
			down &= PAD_BUTTON_START;
			held = 0;
			if (Weap_DeadTime() > 3.0f) {
				/* back to the start of the level with what we had then */
				if (!pending) {
					Caption_Clear();
					Hud_Clear();
					Audio_StopAll();
					snprintf(pendMap, sizeof(pendMap), "%s", mapName);
					pending = PEND_RESPAWN;
				}
			}
		}
		if (playing && !paused && !dead) {
			cmd.yaw -= stick(PAD_SubStickX(0)) * 180.0f * gOptions.sensitivity * dt;
			cmd.pitch -= stick(PAD_SubStickY(0)) * 140.0f * gOptions.sensitivity * (gOptions.reverseLook ? -1 : 1) * dt;
		}
		if (cmd.pitch > 89.0f)
			cmd.pitch = 89.0f;
		if (cmd.pitch < -89.0f)
			cmd.pitch = -89.0f;
		cmd.forwardmove = playing && !paused && !cutscene ? stick(PAD_StickY(0)) : 0;
		cmd.sidemove = playing && !paused && !cutscene ? stick(PAD_StickX(0)) : 0;
		int driving = Veh_Driving() >= 0;
		if (driving) {
			/* in a vehicle: the stick drives, the C-stick looks around the cab, X gets out */
			static f32 lookYaw, lookPitch;
			if (playing && !paused && !cutscene) {
				lookYaw -= stick(PAD_SubStickX(0)) * 180.0f * dt;
				lookPitch -= stick(PAD_SubStickY(0)) * 120.0f * dt;
				if (!stick(PAD_SubStickX(0)))
					lookYaw -= lookYaw * (dt * 3 > 1 ? 1 : dt * 3);
			}
			lookYaw = lookYaw > 100 ? 100 : lookYaw < -100 ? -100 : lookYaw;
			lookPitch = lookPitch > 60 ? 60 : lookPitch < -50 ? -50 : lookPitch;
			u32 vb = 0;
			if (held & PAD_BUTTON_B)
				vb |= VB_TURBO;
			if (held & PAD_BUTTON_A)
				vb |= VB_HANDBRAKE;
			if (held & PAD_BUTTON_X)
				vb |= VB_EXIT;
			if (held & PAD_TRIGGER_R)
				vb |= VB_FIRE;
			Veh_Controls(cutscene || paused ? 0 : cmd.forwardmove, cutscene || paused ? 0 : cmd.sidemove, lookYaw,
			             lookPitch, cutscene || paused ? 0 : vb);
			cmd.forwardmove = cmd.sidemove = 0;
			held &= ~(PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_TRIGGER_R | PAD_TRIGGER_L | PAD_BUTTON_Y);
			down &= ~(PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_TRIGGER_R | PAD_TRIGGER_L | PAD_BUTTON_Y |
			          PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_DOWN);
		}
		if (cutscene)
			held = 0;
		cmd.buttons = 0;
		if (held & PAD_BUTTON_A)
			cmd.buttons |= IN_JUMP;
		if (Weap_SprintAllowed((held & PAD_BUTTON_B) != 0, cmd.forwardmove != 0 || cmd.sidemove != 0))
			cmd.buttons |= IN_SPEED; /* the HEV suit's sprint, on aux power */
		if (!Weap_HasSuit())
			cmd.buttons |= IN_WALK; /* CHL2_Player: without the suit he only walks (150), as the
			                         * Trainstation scripts are timed for */
		if ((down & PAD_BUTTON_UP) && playing && !paused && !cutscene)
			Weap_ToggleFlashlight();
		if (held & PAD_TRIGGER_Z)
			cmd.buttons |= IN_DUCK;
		{
			f32 eye[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + Player_EyeHeight(&pl)};
			f32 sp = sinf(cmd.pitch * (f32)M_PI / 180), cp = cosf(cmd.pitch * (f32)M_PI / 180);
			f32 fwd[3] = {cp * cosf(cmd.yaw * (f32)M_PI / 180), cp * sinf(cmd.yaw * (f32)M_PI / 180), -sp};
			Phys_SetView(eye, fwd);
			{
				f32 sy = sinf(cmd.yaw * (f32)M_PI / 180), cy = cosf(cmd.yaw * (f32)M_PI / 180);
				f32 spd = (held & PAD_BUTTON_B) ? 320.0f : 190.0f;
				Phys_SetWish((cy * cmd.forwardmove + sy * cmd.sidemove) * spd, (sy * cmd.forwardmove - cy * cmd.sidemove) * spd);
			}
			Tanks_SetView(eye, fwd);
			if ((down & PAD_BUTTON_X) && !Tanks_PlayerUse(eye, fwd))
				Ent_Use(eye, fwd);
			/* on a mounted gun: the fire trigger is the gun's, his own weapon is put away */
			Tanks_PlayerFire(playing && !paused && (held & PAD_TRIGGER_R));
			if ((down & PAD_BUTTON_Y) && Phys_Holding() && Phys_HeldEnt() < 0)
				Phys_Drop(1);
			/* weapons (the analog triggers count as pressed past halfway too) */
			u32 wh = 0, wd = 0;
			if (playing && !paused) {
				if (held & PAD_TRIGGER_R)
					wh |= WB_FIRE;
				if (held & PAD_TRIGGER_L)
					wh |= WB_ALT;
				if (held & PAD_BUTTON_X)
					wh |= WB_USE;
				static u32 prevWh;
				wd = wh & ~prevWh;
				prevWh = wh;
				if (down & PAD_BUTTON_Y)
					wd |= WB_RELOAD;
				if (down & PAD_BUTTON_RIGHT)
					wd |= WB_NEXT;
				if (down & PAD_BUTTON_LEFT)
					wd |= WB_PREV;
				if (down & PAD_BUTTON_DOWN)
					wd |= WB_LAST;
				if (down & PAD_BUTTON_X)
					wd |= WB_USE;
			}
			if (Tanks_PlayerManned())
				wh = wd = 0;
			Weap_Input(wh, wd, eye, cmd.pitch, cmd.yaw, cutscene || !playing || paused);
		}

		accum += paused || pending ? 0 : dt;
		while (accum >= TICK_INTERVAL) {
			memcpy(prevOrigin, pl.origin, sizeof(prevOrigin));
			prevEye = Player_EyeHeight(&pl);
			if (playing)
				CRASH_STAGE("player");
			Player_Tick(&pl, &cmd);
			Ent_SetNoTarget(!playing);
			CRASH_STAGE("entities, NPCs, scenes, physics");
			Ent_Tick(TICK_INTERVAL);
			{
				/* riding a train or lift: the view interpolates only his own movement on it, so it doesn't
				 * lag the (uninterpolated) train by a varying part of a tick, which made it jitter */
				f32 carried[3];
				Ent_PlayerCarry(carried);
				for (int k = 0; k < 3; k++)
					prevOrigin[k] += carried[k];
			}
			CRASH_STAGE("weapons");
			Weap_Tick(TICK_INTERVAL);
			/* teleports set the player's view: keep the controller's angles in sync */
			cmd.pitch = pl.pitch;
			cmd.yaw = pl.yaw;
			accum -= TICK_INTERVAL;
			if (pending)
				break;
			if (pendingMap[0]) {
				char m[32], l[32];
				snprintf(m, sizeof(m), "%s", pendingMap);
				snprintf(l, sizeof(l), "%s", pendingLandmark);
				pendingMap[0] = 0;
				snprintf(pendMap, sizeof(pendMap), "%s", m);
				snprintf(pendMark, sizeof(pendMark), "%s", l);
				pending = PEND_LEVEL;
				accum = 0;
				break;
			}

			for (u32 i = 0; i < pack.numTriggers && !pl.noclip && !pending; i++) {
				int in = touching(&pl, &pack.triggers[i]);
				if (in && triggerArmed[i]) {
					snprintf(pendMap, sizeof(pendMap), "%s", pack.triggers[i].map);
					snprintf(pendMark, sizeof(pendMark), "%s", pack.triggers[i].landmark);
					pending = PEND_LEVEL;
					accum = 0;
					break;
				}
				if (!in)
					triggerArmed[i] = 1;
			}
		}

		/* render between the last two ticks so motion is smooth at 60 Hz */
		f32 a = accum / TICK_INTERVAL;
		f32 eye = prevEye + (Player_EyeHeight(&pl) - prevEye) * a;
		cam.origin.x = prevOrigin[0] + (pl.origin[0] - prevOrigin[0]) * a;
		cam.origin.y = prevOrigin[1] + (pl.origin[1] - prevOrigin[1]) * a;
		cam.origin.z = prevOrigin[2] + (pl.origin[2] - prevOrigin[2]) * a + eye;
		cam.pitch = cmd.pitch;
		cam.yaw = cmd.yaw;
		{
			f32 ve[3], vp, vy, vr;
			if (Veh_View(ve, &vp, &vy, &vr)) {
				cam.origin = (guVector){ve[0], ve[1], ve[2]};
				cam.pitch = vp;
				cam.yaw = vy;
				cmd.pitch = vp;
				cmd.yaw = vy;
			}
		}
		if (Weap_DeadTime() > 0) {
			/* dead: the view sinks to the floor and tilts */
			f32 k = Weap_DeadTime() > 0.6f ? 1 : Weap_DeadTime() / 0.6f;
			cam.origin.z -= (eye - 10) * k;
		}
		if (Ent_ViewOverride(vo, va)) {
			cam.origin = (guVector){vo[0], vo[1], vo[2]};
			cam.pitch = va[0];
			cam.yaw = va[1];
		}
		f32 shake = Ent_ShakeAmount();
		if (shake > 0) {
			cam.origin.x += ((rand() & 255) / 127.5f - 1) * shake * 0.5f;
			cam.origin.y += ((rand() & 255) / 127.5f - 1) * shake * 0.5f;
			cam.origin.z += ((rand() & 255) / 127.5f - 1) * shake * 0.5f;
		}
		{
			/* env_zoom changes the field of view */
			f32 zf = Ent_ZoomFov();
			f32 fx = zf > 0 ? zf : 75.0f;
			if (fx != cam.fovx) {
				cam.fovx = fx;
				cam.fovy = 2.0f * atanf(tanf(fx * (f32)M_PI / 360.0f) * 0.75f) * 180.0f / (f32)M_PI;
				guPerspective(proj, cam.fovy, 4.0f / 3.0f, 4.0f, 16384.0f);
				GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
			}
		}
		Camera_Update(&cam);
		Face_BeginFrame(Ent_Now());
		GX_InvVtxCache(); /* faces are re-morphed into the same buffers every frame */

		Mtx view;
		guVector target = {cam.origin.x + cam.forward.x, cam.origin.y + cam.forward.y,
		                   cam.origin.z + cam.forward.z};
		guLookAt(view, &cam.origin, &cam.up, &target);

		/* monitor screens: the active point_camera's view, refreshed every other frame */
		f32 co[3], ca[3], cfov;
		if (Ent_ActiveCamera(co, ca, &cfov)) {
			if (fb == 0) {
				Camera cc = {0};
				cc.origin = (guVector){co[0], co[1], co[2]};
				cc.pitch = ca[0];
				cc.yaw = ca[1];
				cc.fovx = cfov;
				cc.fovy = cfov; /* point_camera without UseScreenAspectRatio renders square (aspect 1) */
				Camera_Update(&cc);
				Mtx cv;
				Mtx44 cp;
				guVector ct = {cc.origin.x + cc.forward.x, cc.origin.y + cc.forward.y, cc.origin.z + cc.forward.z};
				guLookAt(cv, &cc.origin, &cc.up, &ct);
				guPerspective(cp, cc.fovy, 1.0f, 4.0f, 8192.0f);
				GX_LoadProjectionMtx(cp, GX_PERSPECTIVE);
				Render_CameraBegin();
				World_Collect(&world, &cc, 0);
				Render_Frame(&pack, &world, &cc, cv, &st);
				Render_CameraEnd(rmode->fbWidth, rmode->efbHeight);
				GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
			}
		} else {
			Render_CameraOff();
		}

		IntroView iv;
		int introOn = Ent_IntroView(&iv) &&
		              (CRASH_STAGE("intro view"), 1) && Intro_Render(&iv, &pack, &world, &cam, view, proj, rmode->fbWidth, rmode->efbHeight, &st);
		if (!introOn) {
			World_Collect(&world, &cam, 0);
			int flOn = Weap_FlashlightOn() && !cutscene;
			Render_ModelFlashlight(flOn, &cam);
			CRASH_STAGE("rendering the world");
			sky_frame(&cam);
			Render_Frame(&pack, &world, &cam, view, &st);
			Render_SetSky(NULL, NULL, NULL, 0, 0, 0, NULL);
			Render_AdaptDetail(&st);
			Gibs_Draw(view);
			Render_ModelFlashlight(0, 0);
			if (flOn)
				Render_Flashlight(&pack, &world, &cam, view);
			CRASH_STAGE("drawing weapons and effects");
			Weap_DrawWorld(view, &cam);
			f32 eyeP[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
			Render_Sprites(view, eyeP, dt, sound_occluded);
			if (playing && !cutscene && Veh_Driving() < 0)
				Weap_DrawViewmodel(view, &cam, proj, rmode->fbWidth, rmode->efbHeight);
		}
		f32 ear[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
		CRASH_STAGE("sound");
		Audio_Update(ear, cam.yaw);
		if (!paused) {
			Scape_Tick(dt, ear, pl.origin);
			room_probe(ear, dt);
			Light_StyleTick(dt);
		}
		Fx_Tick(Ent_Now());
		Audio_SetUnderwater(pl.waterLevel >= 3 && playing);
		if (!paused) {
			Caption_Tick(dt);
			Hud_Tick(dt);
		}
		Text_Begin(rmode->fbWidth, rmode->efbHeight);
		u32 fade = Ent_ScreenFade();
		if (introOn && iv.fade[3] > 0) /* the intro's own fade overrides env_fade, like Source */
			fade = (u32)iv.fade[0] << 24 | (u32)iv.fade[1] << 16 | (u32)iv.fade[2] << 8 |
			       (u32)(iv.fade[3] > 255 ? 255 : iv.fade[3]);
		if (fade & 0xFF)
			Text_Rect(0, 0, rmode->fbWidth, rmode->efbHeight, fade);
		u32 flash = Weap_DamageFlash();
		if (flash & 0xFF)
			Text_Rect(0, 0, rmode->fbWidth, rmode->efbHeight, flash);
		if (playing && !cutscene) {
			Weap_DrawHud(rmode->fbWidth, rmode->efbHeight);
			Hud_Draw(rmode->fbWidth, rmode->efbHeight);
		}
		if (!pending)
			Caption_Draw(rmode->fbWidth, rmode->efbHeight, proj);
		CRASH_STAGE("menu");
		Menu_Draw(rmode->fbWidth, rmode->efbHeight);
		if (pending)
			Load_DrawOverlay(pending == PEND_NEWGAME ? LOAD_NEWGAME : LOAD_LEVEL, rmode->fbWidth, rmode->efbHeight);
		Text_End(proj);

		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		GX_SetColorUpdate(GX_TRUE);
		GX_CopyDisp(xfb[fb], pending ? GX_FALSE : GX_TRUE); /* a loading frame stays in the EFB for the bar */
		GX_DrawDone();
		VIDEO_SetNextFramebuffer(xfb[fb]);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		fb ^= 1;

		if (pending) {
			int kind = pending;
			pending = PEND_NONE;
			Load_Begin(kind == PEND_NEWGAME ? LOAD_NEWGAME : LOAD_LEVEL, xfb[0], xfb[1], fb ^ 1, rmode->fbWidth,
			           rmode->efbHeight, proj);
			loadProgress = Load_Progress;
			if (kind == PEND_NEWGAME) {
				Weap_NewGame();
				Ent_ResetGlobals();
				if (load_map(pendMap) != 0) {
					/* back to the main menu over its background, with the error in a dialog */
					char code[16], text[320];
					snprintf(code, sizeof(code), "%s", errCode);
					snprintf(text, sizeof(text), "%s", errText);
					loadProgress = NULL;
					if (load_map(Menu_Background(0)) != 0 && load_map(DEFAULT_MAP) != 0)
						fatal();
					Player_Spawn(&pl, pack.spawnOrigin, pack.spawnAngles[0], pack.spawnAngles[1]);
					newGame = 0;
					if (start_entities() != 0)
						fatal();
					arm_triggers(&pl);
					playing = 0;
					Menu_ShowError(0, code, text);
				} else {
					Player_Spawn(&pl, pack.spawnOrigin, pack.spawnAngles[0], pack.spawnAngles[1]);
					memset(pl.velocity, 0, sizeof(pl.velocity));
					newGame = 1;
					if (start_entities() != 0)
						fatal();
					arm_triggers(&pl);
					playing = 1;
				}
			} else if (kind == PEND_RESPAWN) {
				Player keep = levelStartPlayer;
				Weap_Respawn();
				if (load_map(pendMap) != 0)
					fatal();
				pl = keep;
				memset(pl.velocity, 0, sizeof(pl.velocity));
				newGame = levelNewGame;
				if (start_entities() != 0)
					fatal();
				arm_triggers(&pl);
			} else if (change_level(&pl, pendMap, pendMark) != 0) {
				Menu_ShowError(1, errCode, errText);
			}
			loadProgress = NULL;
			cmd.pitch = pl.pitch;
			cmd.yaw = pl.yaw;
			memcpy(prevOrigin, pl.origin, sizeof(prevOrigin));
			prevEye = Player_EyeHeight(&pl);
			accum = 0;
			last = gettime();
		}
	}
	unload_map();
	Audio_StopAll();
	if (voFile)
		fclose(voFile);
	if (sdDev)
		fatUnmount(sdDev);
	return 0;
}
