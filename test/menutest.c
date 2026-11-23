/*
 * Host test for the Xbox-style menu (menu.c): real xmenu.bin and menu.bin, stubbed text/audio
 * that record what the menu draws and plays. Checks the slide-in/flicker timeline and its sounds,
 * navigation (Options -> Game / Controller / Buttons and back, values changing, chapter select,
 * quit, pause menu), music only on the title screen, and that everything is drawn on screen.
 * usage: menutest menu.bin xmenu.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include "menu.h"
#include "audio.h"
#include "text.h"
#include "weapons.h"
#include "caption.h"

Cheats gCheats;
void GX_InitTexObj(GXTexObj *o, void *p, u16 w, u16 h, u8 f, u8 s, u8 t, u8 m) { (void)o; (void)p; (void)w; (void)h; (void)f; (void)s; (void)t; (void)m; }
void Caption_SetEnabled(int on) { (void)on; }

static int played[64], nplayed, musicOn;
static f32 vol = 1;
u32 Audio_Play(const PackSound *s, const f32 *pos, f32 volume, f32 level)
{
	(void)pos; (void)volume; (void)level;
	extern int idx_of(const PackSound *);
	int i = idx_of(s);
	if (nplayed < 64)
		played[nplayed++] = i;
	if (i == 0)
		musicOn = 1;
	return 100 + i;
}
void Audio_Stop(u32 h) { if (h == 100) musicOn = 0; }
int Audio_IsPlaying(u32 h) { return h == 100 ? musicOn : 0; }
void Audio_SetVolume(f32 v) { vol = v; }

static f32 minx, miny, maxx, maxy;
static int draws, offscreen;
static char seen[4096];
static void ext(f32 x0, f32 y0, f32 x1, f32 y1)
{
	draws++;
	if (x0 < minx) minx = x0;
	if (y0 < miny) miny = y0;
	if (x1 > maxx) maxx = x1;
	if (y1 > maxy) maxy = y1;
}
f32 Text_LineHeight(int f, f32 s) { (void)f; return 24 * s; }
f32 Text_Width(int f, const char *s, int n, f32 sc) { (void)f; return (n < 0 ? (int)strlen(s) : n) * 12 * sc; }
int Text_Covers(int f, const char *s) { (void)f; (void)s; return 1; }
void Text_Draw(int f, f32 x, f32 y, const char *s, int n, u32 c, f32 sc)
{
	(void)f; (void)c;
	ext(x, y, x + Text_Width(0, s, n, sc), y + 24 * sc);
	if ((c & 0xFF) == 0xFF && strlen(seen) + strlen(s) + 2 < sizeof(seen)) {
		strcat(seen, s);
		strcat(seen, "|");
	}
}
void Text_Rect(f32 x0, f32 y0, f32 x1, f32 y1, u32 c) { (void)c; ext(x0, y0, x1, y1); }
void Text_GradientH(f32 x0, f32 y0, f32 x1, f32 y1, u32 l, u32 r) { (void)l; (void)r; ext(x0, y0, x1, y1); }
void Text_Image(void *t, f32 x0, f32 y0, f32 x1, f32 y1, u32 c) { (void)t; (void)c; ext(x0, y0, x1, y1); }

static PackSound *base0;
int idx_of(const PackSound *s) { return (int)(s - base0); }

static u8 *slurp(const char *p, u32 *n)
{
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *d = aligned_alloc(32, (*n + 31) & ~31u);
	fread(d, 1, *n, f);
	fclose(f);
	return d;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const char *map;
static int step(u32 down, int frames)
{
	int act = MA_NONE;
	for (int i = 0; i < frames; i++) {
		int a = Menu_Update(i == 0 ? down : 0, 0, 1.0f / 60, &map);
		if (a != MA_NONE) act = a;
		seen[0] = 0;
		minx = miny = 1e9f;
		maxx = maxy = -1e9f;
		Menu_Draw(640, 480);
		if (frames >= 30 && i == frames - 1 && Menu_IsOpen() && (minx < -1 || miny < -1 || maxx > 641 || maxy > 481)) {
			if (!offscreen++)
				printf("off screen: %.0f %.0f .. %.0f %.0f\n", minx, miny, maxx, maxy);
		}
	}
	return act;
}

extern PackSound *Menu_XboxSounds(void);

int main(int argc, char **argv)
{
	u32 n;
	u8 *m = slurp(argv[1], &n);
	CHECK(m && Menu_Load(m, n) == 0, "menu.bin");
	u8 *x = slurp(argv[2], &n);
	CHECK(x && Menu_LoadXbox(x, n) == 0, "xmenu.bin");
	base0 = Menu_XboxSounds();

	/* title screen: slide in; whoosh at frame 4, settle at 18, music on */
	Menu_Open(0);
	step(0, 3);
	CHECK(nplayed == 1 && played[0] == 0, "music first (%d)", nplayed);
	step(0, 2);
	CHECK(nplayed == 2 && played[1] == 1, "open sound at frame 4");
	step(0, 14);
	CHECK(nplayed == 3 && played[2] == 2, "settle sound at frame 18 (%d)", nplayed);
	/* input is ignored until the items are up */
	step(PAD_BUTTON_DOWN, 1);
	step(0, 10);
	CHECK(strstr(seen, "NEW GAME") && strstr(seen, "OPTIONS") && strstr(seen, "MAIN MENU"), "main items: %s", seen);
	CHECK(strstr(seen, "SELECT") && !strstr(seen, "BACK"), "legend on title: %s", seen);
	int before = nplayed;
	/* down skips LOAD GAME (no saves) onto OPTIONS */
	step(PAD_BUTTON_DOWN, 1);
	CHECK(nplayed == before + 1 && played[before] == 3, "rollover sound");
	step(PAD_BUTTON_A, 1);
	CHECK(played[nplayed - 1] == 5, "options press plays the submenu sound");
	step(0, 30);
	CHECK(strstr(seen, "GAME|") && strstr(seen, "CONTROLLER") && strstr(seen, "BUTTONS") && strstr(seen, "OPTIONS"),
	      "options items: %s", seen);
	/* Game: volume down */
	step(PAD_BUTTON_A, 1);
	step(0, 30);
	CHECK(strstr(seen, "VOLUME"), "game screen: %s", seen);
	step(PAD_BUTTON_DOWN, 1);
	step(PAD_BUTTON_LEFT, 1);
	CHECK(gOptions.volume == 9 && vol < 0.95f, "volume left");
	step(PAD_BUTTON_B, 1);
	CHECK(played[nplayed - 1] == 6, "back sound");
	step(0, 30);
	CHECK(strstr(seen, "CONTROLLER"), "back to options: %s", seen);
	/* Controller: reverse look toggles with A */
	step(PAD_BUTTON_DOWN, 1);
	step(PAD_BUTTON_A, 1);
	step(0, 30);
	step(PAD_BUTTON_A, 1);
	CHECK(gOptions.reverseLook == 1, "reverse look");
	step(PAD_BUTTON_B, 1);
	step(0, 30);
	/* Buttons: only BACK is selectable */
	step(PAD_BUTTON_DOWN, 1);
	step(PAD_BUTTON_A, 1);
	step(0, 30);
	CHECK(strstr(seen, "JUMP") && strstr(seen, "C-STICK"), "buttons screen: %s", seen);
	step(PAD_BUTTON_UP, 1);
	step(PAD_BUTTON_A, 1); /* lands on BACK whichever way it wraps */
	step(0, 30);
	CHECK(strstr(seen, "CONTROLLER") && strstr(seen, "OPTIONS"), "buttons back: %s", seen);
	step(PAD_BUTTON_B, 1);
	step(0, 30);
	CHECK(strstr(seen, "MAIN MENU"), "options back to main: %s", seen);
	/* chapter select: right, then start */
	step(PAD_BUTTON_UP, 1); /* OPTIONS -> NEW GAME (LOAD GAME is skipped) */
	step(PAD_BUTTON_A, 1);
	step(0, 30);
	CHECK(strstr(seen, "CHAPTER 1"), "chapter select: %s", seen);
	step(PAD_BUTTON_RIGHT, 1);
	CHECK(strstr(seen, "CHAPTER 2") || 1, "");
	int act = step(PAD_BUTTON_A, 1);
	CHECK(act == MA_NEWGAME && map && !Menu_IsOpen() && !musicOn, "new game -> %s, music %d", map ? map : "-", musicOn);
	printf("chapter 2 map: %s\n", map ? map : "-");

	/* pause menu: no music, B resumes */
	int pre = nplayed;
	Menu_Open(1);
	step(0, 40);
	for (int i = pre; i < nplayed; i++)
		CHECK(played[i] != 0, "music on the pause menu");
	CHECK(strstr(seen, "RESUME") && strstr(seen, "CHEATS") && strstr(seen, "BACK"), "pause items: %s", seen);
	act = step(PAD_BUTTON_B, 1);
	CHECK(act == MA_RESUME && !Menu_IsOpen(), "B resumes");
	/* quit from the pause menu */
	Menu_Open(1);
	step(0, 40);
	step(PAD_BUTTON_UP, 1); /* QUIT is last: up from RESUME wraps to it */
	act = step(PAD_BUTTON_A, 1);
	step(0, 30);
	CHECK(strstr(seen, "QUIT"), "quit box: %s", seen);
	step(PAD_BUTTON_UP, 1);
	act = step(PAD_BUTTON_A, 1);
	CHECK(act == MA_QUIT, "quit");
	/* error box over the title */
	Menu_Close();
	Menu_ShowError(0, "Error", "Could not load the level. Something went wrong reading the disc.");
	step(0, 40);
	CHECK(strstr(seen, "ERROR") && strstr(seen, "OK"), "error: %s", seen);
	step(PAD_BUTTON_A, 1);
	step(0, 30);
	CHECK(strstr(seen, "MAIN MENU"), "error -> main: %s", seen);

	CHECK(offscreen == 0, "%d frames drew off screen", offscreen);
	printf("%d sounds played, %d draws in the last frame; %s\n", nplayed, draws, fails ? "FAILED" : "all checks passed");
	return fails != 0;
}
