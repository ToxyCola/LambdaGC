#include "menu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <gccore.h>

#include "audio.h"
#include "caption.h"
#include "text.h"
#include "weapons.h"

/* string ids, in tools/frontend.py MENU_STRINGS order */
enum {
	S_RESUME, S_NEWGAME, S_LOADGAME, S_SAVEGAME, S_OPTIONS, S_QUIT, S_NEWGAME_TITLE, S_START, S_BACK,
	S_OPTIONS_TITLE, S_CAPTIONS, S_REVERSE, S_SENSITIVITY, S_VOLUME, S_QUIT_TITLE, S_OK, S_CANCEL, S_GAME_TITLE,
	S_COUNT
};

enum { SCR_MAIN, SCR_NEWGAME, SCR_OPTIONS, SCR_QUIT, SCR_CHEATS, SCR_ERROR, SCR_OPT_GAME, SCR_OPT_CTRL, SCR_OPT_BUTTONS };
#define S_CHEATS 100 /* not a gameui string: the PC game uses the console for these */

#define MAX_CHAPTERS 16
#define THUMB_W 256 /* tools/frontend.py THUMB_W/H */
#define THUMB_H 144

static const u32 bindDefault[NUM_BINDS] = {PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y, PAD_TRIGGER_Z,
                                           PAD_TRIGGER_R, PAD_TRIGGER_L, PAD_BUTTON_UP};
static const char *const bindName[NUM_BINDS] = {"Jump", "Sprint", "Use", "Reload", "Crouch", "Fire", "Alt fire",
                                                "Flashlight"};
Options gOptions = {0, 0, 1.0f, 10,
                    {PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y, PAD_TRIGGER_Z, PAD_TRIGGER_R, PAD_TRIGGER_L,
                     PAD_BUTTON_UP}};

static const char *button_name(u32 b);
/* the hint text shows the buttons as they're set now */
static void sync_bind_names(void)
{
	for (int i = 0; i < NUM_BINDS; i++)
		Text_SetBindName(i, button_name(gOptions.bind[i]));
}

void Options_DefaultBinds(void)
{
	memcpy(gOptions.bind, bindDefault, sizeof(bindDefault));
	sync_bind_names();
}

u32 Options_Remap(u32 b)
{
	u32 all = 0, out;
	for (int i = 0; i < NUM_BINDS; i++)
		all |= bindDefault[i];
	out = b & ~all;
	for (int i = 0; i < NUM_BINDS; i++)
		if (b & gOptions.bind[i])
			out |= bindDefault[i];
	return out;
}

static const char *button_name(u32 b)
{
	switch (b) {
	case PAD_BUTTON_A: return "A";
	case PAD_BUTTON_B: return "B";
	case PAD_BUTTON_X: return "X";
	case PAD_BUTTON_Y: return "Y";
	case PAD_TRIGGER_Z: return "Z";
	case PAD_TRIGGER_R: return "R";
	case PAD_TRIGGER_L: return "L";
	case PAD_BUTTON_UP: return "D-pad up";
	case PAD_BUTTON_DOWN: return "D-pad down";
	case PAD_BUTTON_LEFT: return "D-pad left";
	case PAD_BUTTON_RIGHT: return "D-pad right";
	default: return "-";
	}
}

static int capturing = -1; /* the action waiting for a button on the Buttons screen */

static const u8 *base;
static const char *strs;
static u16 numStrings, numChapters;
static u32 strOff[S_COUNT];
static u32 chTitle[MAX_CHAPTERS], chMap[MAX_CHAPTERS], chBg[MAX_CHAPTERS];
static GXTexObj thumbs[MAX_CHAPTERS];
static int loaded;

static int open, inGame, screen, sel, chapterSel, repeat;
static f32 fade, stickHold;
static char errTitle[48], errText[320];
static int errWasOpen; /* the menu was open before the error: go back to it, else back to the game */

static const char *S(int id)
{
	static const char *fallback[S_COUNT] = {"RESUME GAME", "NEW GAME", "LOAD GAME", "SAVE GAME", "OPTIONS", "QUIT",
	                                        "NEW GAME", "Start New Game", "Back", "Options", "Closed captions",
	                                        "Reverse look", "Look sensitivity", "Volume", "Quit", "OK", "Cancel",
	                                        "HALF-LIFE 2"};
	if (id == S_CHEATS)
		return "CHEATS";
	if (loaded && id < numStrings)
		return strs + strOff[id];
	return fallback[id];
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

int Menu_Load(u8 *data, u32 size)
{
	sync_bind_names();
	if (!data || size < 48)
		return -1;
	u32 stringsOff = be32(data), thumbsOff = be32(data + 4);
	base = data + 32;
	if (memcmp(base, "HMNU", 4) != 0)
		return -1;
	numStrings = (u16)(base[4] << 8 | base[5]);
	numChapters = (u16)(base[6] << 8 | base[7]);
	if (numStrings > S_COUNT)
		numStrings = S_COUNT;
	if (numChapters > MAX_CHAPTERS)
		numChapters = MAX_CHAPTERS;
	u16 fileStrings = (u16)(base[4] << 8 | base[5]);
	const u8 *t = base + 8;
	for (u32 i = 0; i < numStrings; i++)
		strOff[i] = be32(t + i * 4);
	t += fileStrings * 4;
	for (u32 i = 0; i < numChapters; i++) {
		chTitle[i] = be32(t + i * 12);
		chMap[i] = be32(t + i * 12 + 4);
		chBg[i] = be32(t + i * 12 + 8);
	}
	strs = (const char *)base + stringsOff;
	DCFlushRange(data, size);
	for (u32 i = 0; i < numChapters; i++)
		GX_InitTexObj(&thumbs[i], (void *)(base + thumbsOff + i * THUMB_W * THUMB_H * 2), THUMB_W, THUMB_H,
		              GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	loaded = 1;
	return 0;
}

const char *Menu_Background(int chapter)
{
	if (!loaded || !numChapters)
		return "background01";
	if (chapter < 0)
		chapter = 0;
	if (chapter >= numChapters)
		chapter = numChapters - 1;
	return strs + chBg[chapter];
}

/* ---- the original Xbox menu's sounds (xmenu.bin, from hl2OGXboxMenu.swf) ---- */

enum { XS_MUSIC, XS_OPEN, XS_SETTLE, XS_ROLLOVER, XS_PRESS, XS_SUBMENU, XS_BACK, XS_COUNT };
static PackSound xsnd[XS_COUNT];
static int xloaded;
/* the GameCube's A (top half) and B (bottom half) buttons for the legend bar, from xmenu.bin */
static GXTexObj btnTex;
static int btnLoaded;
static u32 musicHandle;

int Menu_LoadXbox(u8 *data, u32 size)
{
	if (!data || size < 12 || memcmp(data, "HXMN", 4) != 0)
		return -1;
	u32 n = (u32)(data[4] << 8 | data[5]);
	if (n > XS_COUNT)
		n = XS_COUNT;
	if (12 + n * 12 > size)
		return -1;
	for (u32 i = 0; i < n; i++) {
		xsnd[i].offset = be32(data + 12 + i * 12);
		xsnd[i].sizeVol = be32(data + 16 + i * 12);
		xsnd[i].samplesLevel = be32(data + 20 + i * 12);
	}
	xloaded = 1;
	u32 iw = (u32)(data[6] << 8 | data[7]), ih = (u32)(data[8] << 8 | data[9]);
	u32 off = (12 + n * 12 + 31) & ~31u;
	if (iw && ih == iw * 2 && off + iw * ih * 4 <= size && !((uintptr_t)(data + off) & 31)) {
		DCFlushRange(data + off, iw * ih * 4);
		GX_InitTexObj(&btnTex, data + off, (u16)iw, (u16)ih, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjLOD(&btnTex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
		btnLoaded = 1;
	}
	return 0;
}

static void sfx(int id)
{
	if (xloaded && Sound_Size(&xsnd[id]))
		Audio_Play(&xsnd[id], NULL, 1.0f, 0);
}

/* the menu music loops on the title screen (not on the pause menu), like on the Xbox */
static void music(int on)
{
	if (on) {
		if (xloaded && Sound_Size(&xsnd[XS_MUSIC]) && (!musicHandle || !Audio_IsPlaying(musicHandle)))
			musicHandle = Audio_Play(&xsnd[XS_MUSIC], NULL, 0.8f, 0);
	} else if (musicHandle) {
		Audio_Stop(musicHandle);
		musicHandle = 0;
	}
}

/* ---- layout: the SWF's 800x600 stage, scaled to the screen ---- */

#define FPS 60.0f
#define ITEM_STEP 44.6f
#define ITEM_TOP 4.75f
#define BAR_H 40.0f
#define MOVE_FRAMES 13.0f  /* panel move/resize between screens */
#define IN_FRAMES 20.0f    /* slide-in when the menu opens */
#define FLICKER_FRAMES 9.0f
#define ITEMS_AT 7.0f      /* items appear on this flicker frame */

typedef struct { f32 x, y, w, h; } Rect;

/* item kinds */
enum {
	I_RESUME, I_NEWGAME, I_LOAD, I_SAVE, I_OPTIONS, I_CHEATS, I_QUIT, I_BACK,
	I_GAME, I_CONTROLLER, I_BUTTONS, I_CAPTIONS, I_VOLUME, I_REVERSE, I_SENS,
	I_NOCLIP, I_GOD, I_AMMO, I_GIVEALL, I_OK, I_CANCEL, I_START, I_INFO, I_RESETBINDS,
	I_BIND0 /* .. I_BIND0 + NUM_BINDS - 1 */
};

typedef struct {
	int id;
	char label[40];
	char value[24];
	int disabled; /* greyed (no saves yet) */
	int info;     /* a line of text, not selectable */
} Item;

static f32 tAnim, tOpen;       /* frames since the last screen change / since opening */
static Rect rectFrom;          /* panel before the current move */
static int opening;            /* slide in from the right, then flicker */
static int settlePending, openPending;
static int lastSel = -1;
static int optSel;             /* where to return to in Options */

static void upper(char *dst, int n, const char *s)
{
	int i = 0;
	for (; s[i] && i < n - 1; i++)
		dst[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
	dst[i] = 0;
}

static void add(Item *it, int *n, int id, const char *label, const char *value)
{
	Item *p = &it[(*n)++];
	memset(p, 0, sizeof(*p));
	p->id = id;
	upper(p->label, sizeof(p->label), label);
	if (value)
		upper(p->value, sizeof(p->value), value);
	p->disabled = id == I_LOAD || id == I_SAVE;
	p->info = id == I_INFO;
}

static const char *onoff(int v) { return v ? "On" : "Off"; }

static int list(int scr, Item *it)
{
	int n = 0;
	char v[24];
	switch (scr) {
	case SCR_MAIN:
		if (inGame)
			add(it, &n, I_RESUME, S(S_RESUME), NULL);
		add(it, &n, I_NEWGAME, S(S_NEWGAME), NULL);
		add(it, &n, I_LOAD, S(S_LOADGAME), NULL);
		if (inGame)
			add(it, &n, I_SAVE, S(S_SAVEGAME), NULL);
		add(it, &n, I_OPTIONS, S(S_OPTIONS), NULL);
		if (inGame)
			add(it, &n, I_CHEATS, S(S_CHEATS), NULL);
		add(it, &n, I_QUIT, S(S_QUIT), NULL);
		break;
	case SCR_OPTIONS:
		add(it, &n, I_GAME, "Game", NULL);
		add(it, &n, I_CONTROLLER, "Controller", NULL);
		add(it, &n, I_BUTTONS, "Buttons", NULL);
		add(it, &n, I_BACK, S(S_BACK), NULL);
		break;
	case SCR_OPT_GAME:
		add(it, &n, I_CAPTIONS, S(S_CAPTIONS), onoff(gOptions.captions));
		snprintf(v, sizeof(v), "%d", gOptions.volume);
		add(it, &n, I_VOLUME, S(S_VOLUME), v);
		add(it, &n, I_BACK, S(S_BACK), NULL);
		break;
	case SCR_OPT_CTRL:
		add(it, &n, I_REVERSE, S(S_REVERSE), onoff(gOptions.reverseLook));
		snprintf(v, sizeof(v), "%.1f", gOptions.sensitivity);
		add(it, &n, I_SENS, S(S_SENSITIVITY), v);
		add(it, &n, I_BACK, S(S_BACK), NULL);
		break;
	case SCR_OPT_BUTTONS:
		/* each action takes a button: A on it, then the button (the one that had it gets this one's) */
		for (int i = 0; i < NUM_BINDS; i++)
			add(it, &n, I_BIND0 + i, bindName[i], capturing == i ? "Press a button" : button_name(gOptions.bind[i]));
		add(it, &n, I_RESETBINDS, "Default buttons", NULL);
		add(it, &n, I_BACK, S(S_BACK), NULL);
		break;
	case SCR_CHEATS:
		add(it, &n, I_NOCLIP, "No clipping", onoff(gCheats.noclip));
		add(it, &n, I_GOD, "God mode", onoff(gCheats.god));
		add(it, &n, I_AMMO, "Infinite ammo", onoff(gCheats.infiniteAmmo));
		add(it, &n, I_GIVEALL, "Give all weapons", NULL);
		add(it, &n, I_BACK, S(S_BACK), NULL);
		break;
	case SCR_QUIT:
		add(it, &n, I_OK, S(S_OK), NULL);
		add(it, &n, I_CANCEL, S(S_CANCEL), NULL);
		break;
	case SCR_NEWGAME:
		add(it, &n, I_START, S(S_START), NULL);
		add(it, &n, I_CANCEL, S(S_CANCEL), NULL);
		break;
	case SCR_ERROR:
		add(it, &n, I_OK, S(S_OK), NULL);
		break;
	}
	return n;
}

static const char *title_of(int scr)
{
	switch (scr) {
	case SCR_MAIN: return inGame ? "PAUSED" : "MAIN MENU";
	case SCR_OPTIONS: return "OPTIONS";
	case SCR_OPT_GAME: return "GAME";
	case SCR_OPT_CTRL: return "CONTROLLER";
	case SCR_OPT_BUTTONS: return "BUTTONS";
	case SCR_CHEATS: return "CHEATS";
	case SCR_QUIT: return "QUIT";
	case SCR_NEWGAME: return "NEW GAME";
	default: return errTitle;
	}
}

static int selectable(const Item *p) { return !p->disabled && !p->info; }

/* extra space above the items: chapter thumbnails, error text */
static f32 header_h(int scr)
{
	if (scr == SCR_NEWGAME)
		return 236;
	if (scr == SCR_ERROR)
		return 120;
	return 0;
}

/* the panel for a screen, in stage units. It sits at the bottom left (above the legend bar), like the
 * console menus and the PC's: every screen keeps its bottom edge there and grows upward; the screens
 * with values are wider, chapter select and the error box take most of the width */
#define PANEL_X 70.0f
#define PANEL_BOTTOM 492.0f
static Rect rect_of(int scr)
{
	Item it[16];
	int n = list(scr, it);
	f32 step = scr == SCR_OPT_BUTTONS ? 30.0f : ITEM_STEP;
	Rect r = {PANEL_X, 0, 288, 0};
	switch (scr) {
	case SCR_MAIN:
	case SCR_OPTIONS:
	case SCR_QUIT:
		break;
	case SCR_OPT_BUTTONS:
		r.w = 440;
		break;
	case SCR_NEWGAME:
		r.x = 40, r.w = 720;
		break;
	case SCR_ERROR:
		r.w = 520;
		break;
	default:
		r.w = 420;
		break;
	}
	r.h = header_h(scr) + ITEM_TOP * 2 + n * step;
	if (scr == SCR_OPT_BUTTONS)
		r.h += ITEM_STEP - step; /* BACK gets a full-size bar */
	r.y = PANEL_BOTTOM - r.h;
	return r;
}

static f32 ease(f32 t) { return t >= 1 ? 1 : t <= 0 ? 0 : 1 - (1 - t) * (1 - t) * (1 - t); }

static Rect cur_rect(void)
{
	Rect to = rect_of(screen);
	f32 dur = opening ? IN_FRAMES : MOVE_FRAMES;
	f32 k = ease(tAnim / dur);
	Rect r;
	r.x = rectFrom.x + (to.x - rectFrom.x) * k;
	r.y = rectFrom.y + (to.y - rectFrom.y) * k;
	r.w = rectFrom.w + (to.w - rectFrom.w) * k;
	r.h = rectFrom.h + (to.h - rectFrom.h) * k;
	return r;
}

static void go(int scr, int newSel)
{
	rectFrom = cur_rect();
	opening = 0;
	screen = scr;
	sel = newSel;
	tAnim = 0;
	settlePending = 1;
	Item it[16];
	int n = list(scr, it);
	while (sel < n - 1 && !selectable(&it[sel]))
		sel++;
	lastSel = sel;
}

void Menu_Open(int game)
{
	open = 1;
	inGame = game;
	screen = SCR_MAIN;
	sel = 0;
	fade = 0;
	tAnim = tOpen = 0;
	opening = 1;
	rectFrom = rect_of(SCR_MAIN);
	rectFrom.x -= 367; /* slides in from the left */
	openPending = settlePending = 1;
	lastSel = 0;
	music(!game);
}

void Menu_Close(void)
{
	open = 0;
	music(0);
}

void Menu_ShowError(int game, const char *title, const char *text)
{
	errWasOpen = open || !game;
	if (!open) {
		fade = 0;
		tOpen = 0;
		rectFrom = rect_of(SCR_ERROR);
	} else {
		rectFrom = cur_rect();
	}
	open = 1;
	inGame = game;
	snprintf(errTitle, sizeof(errTitle), "%s", title);
	upper(errTitle, sizeof(errTitle), errTitle);
	snprintf(errText, sizeof(errText), "%s", text);
	screen = SCR_ERROR;
	sel = 0;
	tAnim = 0;
	opening = 0;
	settlePending = 1;
}
int Menu_IsOpen(void) { return open; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* left/right on a value */
static int adjust(int id, int lr)
{
	switch (id) {
	case I_CAPTIONS:
		gOptions.captions = !gOptions.captions;
		Caption_SetEnabled(gOptions.captions);
		return 1;
	case I_VOLUME:
		gOptions.volume = clampi(gOptions.volume + lr, 0, 10);
		Audio_SetVolume(gOptions.volume / 10.0f);
		return 1;
	case I_REVERSE:
		gOptions.reverseLook = !gOptions.reverseLook;
		return 1;
	case I_SENS:
		gOptions.sensitivity += lr * 0.1f;
		if (gOptions.sensitivity < 0.5f)
			gOptions.sensitivity = 0.5f;
		if (gOptions.sensitivity > 2.0f)
			gOptions.sensitivity = 2.0f;
		return 1;
	case I_NOCLIP:
		gCheats.noclip = !gCheats.noclip;
		return 1;
	case I_GOD:
		gCheats.god = !gCheats.god;
		return 1;
	case I_AMMO:
		gCheats.infiniteAmmo = !gCheats.infiniteAmmo;
		return 1;
	}
	return 0;
}

static int back_from(int scr)
{
	switch (scr) {
	case SCR_OPT_GAME:
	case SCR_OPT_CTRL:
	case SCR_OPT_BUTTONS:
		return SCR_OPTIONS;
	default:
		return SCR_MAIN;
	}
}

static int resume(void)
{
	open = 0;
	music(0);
	return MA_RESUME;
}

static int go_back(void)
{
	if (screen == SCR_ERROR) {
		if (!errWasOpen)
			return resume();
		sfx(XS_BACK);
		go(SCR_MAIN, 0);
		return MA_NONE;
	}
	if (screen == SCR_MAIN) {
		if (inGame) {
			sfx(XS_BACK);
			return resume();
		}
		return MA_NONE;
	}
	int to = back_from(screen);
	int s = 0;
	if (to == SCR_OPTIONS) {
		s = optSel;
	} else {
		/* back onto the item that led here */
		Item it[16];
		int n = list(SCR_MAIN, it);
		int want = screen == SCR_OPTIONS ? I_OPTIONS : screen == SCR_CHEATS ? I_CHEATS
		           : screen == SCR_QUIT ? I_QUIT : I_NEWGAME;
		for (int i = 0; i < n; i++)
			if (it[i].id == want)
				s = i;
	}
	sfx(XS_BACK);
	go(to, s);
	return MA_NONE;
}

int Menu_Update(u32 down, s8 stickY, f32 dt, const char **map)
{
	if (!open)
		return MA_NONE;
	fade += dt * 3;
	if (fade > 1)
		fade = 1;
	tAnim += dt * FPS;
	tOpen += dt * FPS;
	if (!inGame)
		music(1); /* (re)start it: a level load stops every sound */
	/* the SWF's cues: the whoosh 4 frames into the slide, the settle as it lands */
	if (openPending && tAnim >= 4) {
		sfx(XS_OPEN);
		openPending = 0;
	}
	if (settlePending && tAnim >= (opening ? 18 : MOVE_FRAMES)) {
		sfx(XS_SETTLE);
		settlePending = 0;
	}
	/* input waits until the items are up */
	f32 ready = (opening ? IN_FRAMES : MOVE_FRAMES) + ITEMS_AT;
	if (tAnim < ready) {
		if ((down & PAD_BUTTON_START) && inGame && screen == SCR_MAIN)
			return resume();
		return MA_NONE;
	}

	if (capturing >= 0) {
		static const u32 pick[] = {PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y, PAD_TRIGGER_Z,
		                           PAD_TRIGGER_R, PAD_TRIGGER_L, PAD_BUTTON_UP};
		if (down & PAD_BUTTON_START) { /* (Start: leave it as it was) */
			capturing = -1;
			sfx(XS_BACK);
			return MA_NONE;
		}
		for (u32 k = 0; k < sizeof(pick) / sizeof(pick[0]); k++)
			if (down & pick[k]) {
				for (int j = 0; j < NUM_BINDS; j++)
					if (j != capturing && gOptions.bind[j] == pick[k])
						gOptions.bind[j] = gOptions.bind[capturing];
				gOptions.bind[capturing] = pick[k];
				sync_bind_names();
				capturing = -1;
				sfx(XS_PRESS);
				break;
			}
		return MA_NONE;
	}

	int dir = 0;
	if (down & PAD_BUTTON_UP)
		dir = -1;
	if (down & PAD_BUTTON_DOWN)
		dir = 1;
	if (stickY > 50 || stickY < -50) {
		stickHold -= dt;
		if (!repeat || stickHold <= 0) {
			dir = stickY > 0 ? -1 : 1;
			stickHold = repeat ? 0.12f : 0.4f;
			repeat = 1;
		}
	} else {
		repeat = 0;
	}
	int lr = (down & PAD_BUTTON_LEFT) ? -1 : (down & PAD_BUTTON_RIGHT) ? 1 : 0;

	Item it[16];
	int n = list(screen, it);
	if (dir && n) {
		int s = sel;
		do
			s = (s + dir + n) % n;
		while (!selectable(&it[s]) && s != sel);
		sel = s;
	}
	if (sel != lastSel) {
		sfx(XS_ROLLOVER);
		lastSel = sel;
	}
	if ((down & PAD_BUTTON_START) && inGame && screen != SCR_ERROR)
		return resume();
	if (down & PAD_BUTTON_B)
		return go_back();

	if (screen == SCR_NEWGAME && lr && numChapters) {
		chapterSel = (chapterSel + lr + numChapters) % numChapters;
		sfx(XS_ROLLOVER);
	} else if (lr && adjust(it[sel].id, lr)) {
		sfx(XS_ROLLOVER);
	}
	if (!(down & PAD_BUTTON_A) || !n || !selectable(&it[sel]))
		return MA_NONE;

	switch (it[sel].id) {
	case I_RESUME:
		sfx(XS_PRESS);
		return resume();
	case I_NEWGAME:
		sfx(XS_PRESS);
		go(SCR_NEWGAME, 0);
		break;
	case I_OPTIONS:
		sfx(XS_SUBMENU);
		go(SCR_OPTIONS, 0);
		break;
	case I_CHEATS:
		sfx(XS_SUBMENU);
		go(SCR_CHEATS, 0);
		break;
	case I_QUIT:
		sfx(XS_PRESS);
		go(SCR_QUIT, 1);
		break;
	case I_GAME:
	case I_CONTROLLER:
	case I_BUTTONS:
		sfx(XS_SUBMENU);
		optSel = sel;
		go(it[sel].id == I_GAME ? SCR_OPT_GAME : it[sel].id == I_CONTROLLER ? SCR_OPT_CTRL : SCR_OPT_BUTTONS, 0);
		break;
	case I_BACK:
	case I_CANCEL:
		return go_back();
	case I_GIVEALL:
		sfx(XS_PRESS);
		return MA_GIVEALL;
	case I_START:
		if (!numChapters)
			break;
		sfx(XS_PRESS);
		*map = strs + chMap[chapterSel];
		open = 0;
		music(0);
		screen = SCR_MAIN;
		return MA_NEWGAME;
	case I_OK:
		if (screen == SCR_QUIT) {
			sfx(XS_PRESS);
			return MA_QUIT;
		}
		return go_back(); /* the error box */
	case I_RESETBINDS:
		Options_DefaultBinds();
		sfx(XS_PRESS);
		break;
	default:
		if (it[sel].id >= I_BIND0 && it[sel].id < I_BIND0 + NUM_BINDS) {
			capturing = it[sel].id - I_BIND0;
			sfx(XS_PRESS);
			break;
		}
		if (adjust(it[sel].id, 1))
			sfx(XS_PRESS);
		break;
	}
	return MA_NONE;
}

/* ---- drawing ---- */

static u32 rgba(u8 r, u8 g, u8 b, f32 a)
{
	if (a < 0)
		a = 0;
	if (a > 1)
		a = 1;
	return (u32)r << 24 | (u32)g << 16 | (u32)b << 8 | (u32)(a * 255);
}

/* the SWF's flicker as the panel lands: colour transforms (multiply, add) frame by frame */
static const f32 flickMul[9] = {0, 1, 1, 0.51f, 0, 0, 0, 0.93f, 1};
static const f32 flickAdd[9] = {255, 0, 0, 125, 255, 255, 255, 18, 0};
static f32 flMul = 1, flAdd = 0;

static u32 fx(u8 r, u8 g, u8 b, f32 a)
{
	f32 c[3] = {r, g, b};
	for (int i = 0; i < 3; i++) {
		c[i] = c[i] * flMul + flAdd;
		if (c[i] > 255)
			c[i] = 255;
	}
	return rgba((u8)c[0], (u8)c[1], (u8)c[2], a);
}

static f32 sx, sy; /* stage -> screen */

static void rect(f32 x0, f32 y0, f32 x1, f32 y1, u32 c) { Text_Rect(x0 * sx, y0 * sy, x1 * sx, y1 * sy, c); }

/* the game's menu font at a pixel height on the stage */
static f32 font_scale(f32 px) { return px * sy / Text_LineHeight(FONT_MENU, 1.0f); }

/* white bold text with the SWF's black glow */
static void glow_text(f32 x, f32 y, const char *s, f32 px, u32 c, f32 a)
{
	f32 k = font_scale(px), X = x * sx, Y = y * sy;
	u32 shadow = rgba(0, 0, 0, 0.55f * a);
	static const s8 o[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}, {-1, -1}, {1, 1}, {-1, 1}, {1, -1}};
	for (int i = 0; i < 8; i++)
		Text_Draw(FONT_MENU, X + o[i][0], Y + o[i][1], s, -1, shadow, k);
	Text_Draw(FONT_MENU, X, Y, s, -1, c, k);
	Text_Draw(FONT_MENU, X + 1, Y, s, -1, c, k); /* bold */
}

static f32 text_w(const char *s, f32 px) { return Text_Width(FONT_MENU, s, -1, font_scale(px)) / sx; }

/* a filled circle (the GameCube's face buttons on the legend bar) */
static void disc(f32 cx, f32 cy, f32 r, u32 c)
{
	const int strips = 12;
	for (int i = 0; i < strips; i++) {
		f32 y0 = -r + 2 * r * i / strips, y1 = -r + 2 * r * (i + 1) / strips, ym = (y0 + y1) * 0.5f;
		f32 hw = sqrtf(r * r - ym * ym);
		rect(cx - hw, cy + y0, cx + hw, cy + y1, c);
	}
}

/* the focus bar: transparent -> orange at half alpha -> opaque -> half -> transparent */
static void highlight(f32 x, f32 y, f32 w, f32 a)
{
	f32 q = w * 0.25f;
	u32 c0 = rgba(0xde, 0x6e, 0x00, 0), c1 = rgba(0xde, 0x6e, 0x00, 0.5f * a), c2 = rgba(0xdc, 0x6e, 0x01, a);
	for (int i = 0; i < 4; i++) {
		u32 l = i == 0 ? c0 : i == 3 ? c1 : i == 1 ? c1 : c2;
		u32 r = i == 0 ? c1 : i == 1 ? c2 : i == 2 ? c1 : c0;
		Text_GradientH((x + q * i) * sx, y * sy, (x + q * (i + 1)) * sx, (y + BAR_H) * sy, l, r);
	}
}

static void legend(f32 a, int canBack)
{
	/* the SWF's bar rises from +69 to +2 at y 533; it ends 16 higher here so "SELECT" stays inside a
	 * TV's title-safe area */
	f32 rise = 69 + (-14 - 69) * ease(tOpen / 15.0f);
	f32 x = 61, y = 533 + rise, w = 680, h = 70;
	Text_GradientH(x * sx, y * sy, (x + w * 0.5f) * sx, (y + h) * sy, rgba(0x45, 0x45, 0x45, 0), rgba(0x45, 0x45, 0x45, 0.5f * a));
	Text_GradientH((x + w * 0.5f) * sx, y * sy, (x + w) * sx, (y + h) * sy, rgba(0x45, 0x45, 0x45, 0.5f * a),
	               rgba(0x66, 0x66, 0x66, 0.5f * a));
	/* GameCube A (big, green) = select; B (small, red) = back */
	f32 bx = x + w - 250, by = y + 22;
	if (btnLoaded) {
		/* the button icons (both drawn on the same 256-unit canvas, so B comes out smaller like the pad) */
		f32 s = 40;
		Text_ImageUV(&btnTex, (bx - s * 0.5f) * sx, (by + 5 - s * 0.5f) * sy, (bx + s * 0.5f) * sx, (by + 5 + s * 0.5f) * sy,
		             0, 0, 1, 0.5f, rgba(255, 255, 255, a));
	} else {
		disc(bx, by + 6, 14, rgba(0, 0, 0, 0.5f * a));
		disc(bx, by + 5, 13, rgba(30, 170, 120, a));
		glow_text(bx - text_w("A", 18) * 0.5f, by - 4, "A", 18, rgba(255, 255, 255, a), a);
	}
	glow_text(bx + 20, by - 6, "SELECT", 22, rgba(255, 255, 255, a), a);
	if (canBack) {
		f32 cx = bx + 125;
		if (btnLoaded) {
			f32 s = 40;
			Text_ImageUV(&btnTex, (cx - s * 0.5f) * sx, (by + 5 - s * 0.5f) * sy, (cx + s * 0.5f) * sx,
			             (by + 5 + s * 0.5f) * sy, 0, 0.5f, 1, 1, rgba(255, 255, 255, a));
		} else {
			disc(cx, by + 6, 10, rgba(0, 0, 0, 0.5f * a));
			disc(cx, by + 5, 9.5f, rgba(210, 40, 40, a));
			glow_text(cx - text_w("B", 14) * 0.5f, by - 1, "B", 14, rgba(255, 255, 255, a), a);
		}
		glow_text(cx + 16, by - 6, "BACK", 22, rgba(255, 255, 255, a), a);
	}
}

/* word wrap for the error box */
static int wrap(const char *p, f32 width, f32 px, char lines[][96], int max)
{
	int nl = 0;
	f32 k = font_scale(px);
	while (*p && nl < max) {
		int n = 0, lastSpace = -1;
		while (p[n] && p[n] != '\n' && n < 95) {
			if (p[n] == ' ')
				lastSpace = n;
			if (Text_Width(FONT_MENU, p, n + 1, k) / sx > width && lastSpace > 0) {
				n = lastSpace;
				break;
			}
			n++;
		}
		snprintf(lines[nl++], 96, "%.*s", n, p);
		p += n;
		while (*p == ' ' || *p == '\n')
			p++;
	}
	return nl;
}

void Menu_Draw(u16 w, u16 h)
{
	if (!open)
		return;
	sx = w / 800.0f;
	sy = h / 600.0f;
	/* the live background map stays visible (the SWF's static picture isn't used); dim it a little
	 * in game so the pause menu reads */
	if (inGame)
		Text_Rect(0, 0, w, h, rgba(0, 0, 0, 0.4f * fade));

	/* the game's logo above the menu, as on the title screen (not over the big panels) */
	if (!inGame && cur_rect().y > 230) {
		f32 tx = PANEL_X * sx, ty = h * 0.16f;
		if (Text_Covers(FONT_TITLE, S(S_GAME_TITLE)))
			Text_Draw(FONT_TITLE, tx, ty, S(S_GAME_TITLE), -1, rgba(255, 255, 255, fade), 0.75f);
		else
			Text_Draw(FONT_MENU, tx, ty, S(S_GAME_TITLE), -1, rgba(255, 255, 255, fade), 1.9f);
	}

	f32 move = opening ? IN_FRAMES : MOVE_FRAMES;
	f32 fl = tAnim - move;
	if (fl >= 0 && fl < FLICKER_FRAMES) {
		flMul = flickMul[(int)fl];
		flAdd = flickAdd[(int)fl];
	} else {
		flMul = 1;
		flAdd = 0;
	}
	Rect r = cur_rect();

	/* the panel: #666666 at 50% */
	rect(r.x, r.y, r.x + r.w, r.y + r.h, fx(0x66, 0x66, 0x66, 0.5f));

	/* the title above the panel's top left; on opening it slides in from the far left */
	const char *title = title_of(screen);
	f32 tx = r.x - 40 < 12 ? 12 : r.x - 40, ty = r.y - 35.75f;
	if (opening)
		tx = -text_w(title, 31) + (tx + text_w(title, 31)) * ease(tAnim / IN_FRAMES);
	glow_text(tx, ty, title, 31, fx(255, 255, 255, 1), 1);

	Item it[16];
	int n = list(screen, it);
	int itemsUp = tAnim >= move + ITEMS_AT;
	if (itemsUp) {
		f32 top = r.y + ITEM_TOP + header_h(screen);
		f32 hdrY = r.y + 12;
		if (screen == SCR_NEWGAME) {
			for (int k = -1; k <= 1; k++) {
				int c = chapterSel + k;
				if (c < 0 || c >= numChapters)
					continue;
				/* the selected chapter's preview big in the middle, its neighbours smaller either side */
				f32 tw = k == 0 ? 280 : 168, th = tw * THUMB_H / THUMB_W;
				f32 cx = r.x + r.w * 0.5f + k * 250 - tw * 0.5f, cy = hdrY + 34 + (k == 0 ? 0 : 158 - th);
				f32 a = k == 0 ? 1 : 0.45f;
				if (k == 0)
					rect(cx - 3, cy - 3, cx + tw + 3, cy + th + 3, rgba(0xdc, 0x6e, 0x01, 1));
				char lab[24];
				snprintf(lab, sizeof(lab), "CHAPTER %d", c + 1);
				glow_text(cx, cy - 26, lab, k == 0 ? 20 : 16, rgba(255, 255, 255, a), a);
				Text_Image(&thumbs[c], cx * sx, cy * sy, (cx + tw) * sx, (cy + th) * sy, rgba(255, 255, 255, a));
				if (k == 0) {
					char t[48];
					upper(t, sizeof(t), strs + chTitle[c]);
					f32 px = 20;
					while (px > 12 && text_w(t, px) > tw + 160)
						px -= 1;
					glow_text(cx + (tw - text_w(t, px)) * 0.5f, cy + th + 8, t, px, rgba(240, 240, 240, 1), 1);
				}
			}
		} else if (screen == SCR_ERROR) {
			char lines[6][96];
			int nl = wrap(errText, r.w - 40, 20, lines, 6);
			for (int i = 0; i < nl; i++)
				glow_text(r.x + 20, hdrY + i * 20, lines[i], 20, rgba(235, 235, 235, 1), 1);
		}
		f32 step = screen == SCR_OPT_BUTTONS ? 30.0f : ITEM_STEP;
		for (int i = 0; i < n; i++) {
			const Item *p = &it[i];
			f32 y = top + i * step;
			int on = i == sel && selectable(p);
			f32 barX = r.x + 3, barW = r.w - 6;
			if (on) {
				highlight(barX, y, barW, 1);
				glow_text(barX - 16, y + 4.5f, ">", 30, rgba(255, 255, 255, 1), 1);
			}
			u32 c = p->disabled ? rgba(150, 150, 150, 1) : rgba(255, 255, 255, 1);
			if (p->info) {
				glow_text(r.x + 20, y + 3, p->label, 20, rgba(220, 220, 220, 1), 1);
				glow_text(r.x + r.w - 20 - text_w(p->value, 20), y + 3, p->value, 20, rgba(255, 255, 255, 1), 1);
			} else if (p->value[0]) {
				glow_text(r.x + 20, y + 4, p->label, 26, c, 1);
				char v[32];
				snprintf(v, sizeof(v), on ? "< %s >" : "%s", p->value);
				glow_text(r.x + r.w - 20 - text_w(v, 26), y + 4, v, 26, c, 1);
			} else {
				f32 pxs = 30;
				while (pxs > 18 && text_w(p->label, pxs) > barW - 24)
					pxs -= 2;
				glow_text(barX + (barW - text_w(p->label, pxs)) * 0.5f, y + 4 + (30 - pxs) * 0.5f, p->label, pxs, c, 1);
			}
		}
	}
	legend(1, screen != SCR_MAIN || inGame);
}

#ifdef HOST_TEST
PackSound *Menu_XboxSounds(void) { return xsnd; }
#endif
