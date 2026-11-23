#include "weapons.h"
#include "envents.h"
#include "sentries.h"
#include "flyers.h"
#include "objects.h"
#include "vehicle.h"

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "collide.h"
#include "ent.h"
#include "npc.h"
#include "physics.h"
#ifndef HOST_TEST
#include "render.h"
#include "text.h"
#endif

/* ---- weapons.hgm records (tools/weapons.py) */
typedef struct {
	u32 name;
	s16 vm, wm;
	u8 slot, flags;
	s16 clip;
	u8 ammo, pellets;
	u8 pad0[2];
	f32 damage, rate, spread;
	s16 seq[8];
	s16 snd[4];
} WeapRec;

typedef struct {
	u32 name;
	s16 model;
	u8 kind, arg;
	s16 amount, snd;
} ItemRec;

_Static_assert(sizeof(WeapRec) == 52, "WeapRec");
_Static_assert(sizeof(ItemRec) == 12, "ItemRec");

enum { F_MELEE = 1, F_AUTO = 2, F_PHYS = 4, F_THROW = 8, F_ROCKET = 16, F_SHOTGUN = 32 };
enum { SQ_IDLE, SQ_DRAW, SQ_HOLSTER, SQ_PRIMARY, SQ_SECONDARY, SQ_RELOAD, SQ_DRY, SQ_MISS };
enum { SN_SHOT, SN_RELOAD, SN_EMPTY, SN_HIT };
enum { K_HEALTH, K_SUIT, K_AMMO, K_WEAPON, K_HEV, K_CRATE };
enum { A_NONE, A_PISTOL, A_357, A_SMG1, A_AR2, A_BUCKSHOT, A_BOLT, A_GRENADE, A_RPG, A_SMG1_GRENADE, A_AR2_ALT,
       NUM_AMMO };


#define MAX_WEAPONS 16
#define MAX_ITEMS 512
#define MAX_PROJ 32
#define MAX_FX 48
#define MAX_CHARGERS 32
#define PI_F 3.14159265f

Cheats gCheats;

static Pack wp;
static int loaded;
static const WeapRec *weaps;
static u32 numWeaps;
static const ItemRec *items;
static u32 numItems;
static s16 xsnd[NUM_X];
static s16 ammoMax[NUM_AMMO];

/* inventory: carried across levels */
typedef struct {
	u8 have[MAX_WEAPONS];
	s16 clip[MAX_WEAPONS];
	s16 ammo[NUM_AMMO];
	s32 cur, last;
	f32 health, armor;
	u8 suit, supercharged;
	f32 aux;          /* HEV auxiliary power 0..100 */
} Inventory;

/* HEV suit devices (hl2_player.cpp): power drained per second while in use, recharged otherwise */
#define AUX_CHARGE_RATE 12.5f
#define AUX_SPRINT 12.5f
#define AUX_FLASHLIGHT 2.222f
#define AUX_BREATHER 6.7f
#define AIRTIME 7.0f          /* HL2: lungs last 7 s once the suit can't help */
#define DROWN_DAMAGE 10.0f
static u8 sprintActive, flashlightOn, breatherActive;
static f32 airFinished, painFinished, drownDmg, drownRestored, recoverAt;
static Inventory inv, levelStartInv;

/* this level */
static const Pack *map;
static Player *player;
static f32 wclock;

typedef struct {
	s32 ent;           /* map entity, -1 = dropped by a crate */
	s16 item;          /* ITEM record */
	u8 alive, placed, falling;
	f32 pos[3];
	f32 vel[3];        /* falling / thrown (an NPC dropped it, a physexplosion threw it) */
	f32 R[3][3];
	u8 tint[4];
} WorldItem;
static u8 *itemOfEnt;  /* per entity: has a WorldItem */
static u32 itemOfEntN;
static f32 itemsClock, itemsScan;
static WorldItem *witems;
static u32 numWItems;

enum { P_FRAG, P_SMGNADE, P_ROCKET, P_BOLT, P_BALL, P_BUGBAIT };
typedef struct {
	u8 type, alive;
	f32 pos[3], vel[3];
	f32 t, fuse;
	f32 spin;
} Proj;
static Proj projs[MAX_PROJ];

typedef struct {
	u8 type, alive, flags; /* flags: EXPL_* */
	f32 pos[3];
	f32 t, dur, scale;
} Fx;
static Fx fxs[MAX_FX];
/* the game's own effect textures in the weapons pack (FXTX; tools/weapons.py FX_TEXTURES, same order) */
enum {
	FX_MUZZLE = 0,        /* 4 */
	FX_CMUZZLE = 4,       /* 2 */
	FX_SHOT_CONCRETE = 6, /* 3 each */
	FX_SHOT_METAL = 9,
	FX_SHOT_WOOD = 12,
	FX_SHOT_GLASS = 15,
	FX_BLOOD_DECAL = 18,  /* 2, then alien */
	FX_EXPLODE = 21,      /* 15 frames */
	FX_SMOKE = 36,
	FX_FLECK_CEMENT = 37, /* 2 */
	FX_FLECK_WOOD = 39,   /* 2 */
	FX_SPARK = 41,
	FX_BLOOD = 42,
	FX_SPLASH = 43,
	FX_BALL = 44,
	FX_GLOW = 45,
	FX_FLAME = 46,     /* 5: sprites/flamelet1-5 (env_fire) */
	FX_LASERBEAM = 51, /* sprites/laserbeam (env_beam's default) */
	FX_PHYSBEAM = 52,  /* sprites/physbeam (point_tesla's default) */
	FX_LIGHTNING = 53, /* sprites/lgtning */
	FX_CAMGLOW = 54,   /* sprites/light_glow03 */
	FX_COUNT = 55
};
static s16 fxTex[FX_COUNT];
static u8 wIcon[32][4];          /* per weapon: HUD icon glyphs: its own, its ammo's, its secondary ammo's (WICN) */
static f32 poisonLeft, poisonNext; /* poison (black headcrab bites): health given back over time */

typedef struct {
	s32 ent;
	f32 juice;
} Charger;
static Charger chargers[MAX_CHARGERS];
static u32 numChargers;

/* input */
static u32 inHeld, inDown;
static f32 eye[3], fwd[3], viewPitch, viewYaw;
static int inFrozen;

/* viewmodel animation */
static s16 vseq = -1, vprev = -1;
static f32 vt, vprevT, vblend = 1;
static int vhold;
static f32 seqEnd;           /* when a one-shot sequence finishes (then idle) */
static f32 nextAttack, nextAlt;
static f32 reloadDone;       /* 0 = not reloading */
static s32 switchTo = -1;    /* weapon being drawn after the holster */
static f32 switchAt;
static f32 selShowUntil;
static f32 muzzleUntil;
static f32 bobPhase, bobAmt;
static u8 vmTint[4] = {128, 128, 128, 255};
static f32 tintAt;

/* the player */
static f32 deadAt;
static f32 flashUntil, flashAmt;
static u8 prevOnGround = 1;
static f32 prevVz;
static f32 chargeAcc;
static int pullWarned;

#ifndef ATTRIBUTE_ALIGN
#define ATTRIBUTE_ALIGN(v) __attribute__((aligned(v)))
#endif
static f32 pose[NPC_MAX_BONES][3][4] ATTRIBUTE_ALIGN(32);

static const char *wstr(u32 off) { return wp.entStrings ? wp.entStrings + off : ""; }

static void play(s16 snd, const f32 *pos, f32 vol, f32 radius)
{
	if (snd >= 0 && (u32)snd < wp.numSounds)
		Audio_Play(&wp.sounds[snd], pos, vol, radius > 0 ? SNDLVL_DEFAULT : 0);
}

static f32 frand(void) { return (f32)(rand() & 0xFFFF) / 65535.0f; }

static s32 weapon_named(const char *cls)
{
	for (u32 i = 0; i < numWeaps; i++)
		if (!strcmp(wstr(weaps[i].name), cls))
			return (s32)i;
	return -1;
}

static s32 item_named(const char *cls)
{
	for (u32 i = 0; i < numItems; i++)
		if (!strcmp(wstr(items[i].name), cls))
			return (s32)i;
	return -1;
}

/* ------------------------------------------------------------------ load */

/* ------------------------------------------------------------------ the suit's voice */

/* tools/weapons.py SUIT_SENTENCES order */
enum { SUIT_LOGON, SUIT_DMG0, SUIT_DMG1, SUIT_DMG2, SUIT_DMG3, SUIT_DMG4, SUIT_DMG5, SUIT_DMG6, SUIT_DMG7,
       SUIT_HLTH1, SUIT_HLTH2, SUIT_HLTH3, SUIT_MED1, SUIT_HEAL7, SUIT_DET0, SUIT_DET1, SUIT_DET2, SUIT_P0,
       NUM_SUIT = SUIT_P0 + 20 };
static const s16 *sentWords[NUM_SUIT];
static u8 sentLen[NUM_SUIT];
static f32 noRepeatUntil[NUM_SUIT];
static int suitQueue[4], suitQueued;
static int sayingSent = -1, sayingWord;
static u32 sayingHandle;
static f32 wordWait, suitNext;

void Weap_SetSentences(const u8 *c, u32 size)
{
	memset(sentLen, 0, sizeof(sentLen));
	if (!c || size < 2)
		return;
	u32 n = (u32)(c[0] << 8 | c[1]), off = 2;
	for (u32 i = 0; i < n && i < NUM_SUIT && off < size; i++) {
		u32 len = (u32)(c[off] << 8 | c[off + 1]);
		off += 2;
		if (len > 255 || off + len * 2u > size)
			break;
		sentWords[i] = (const s16 *)(c + off);
		sentLen[i] = (u8)len;
		off += len * 2u;
	}
}

/* CBasePlayer::SetSuitUpdate: queue a sentence unless it was said within noRepeat seconds */
static void suit_say(int id, f32 noRepeat)
{
	if (!inv.suit || id < 0 || id >= NUM_SUIT || !sentLen[id] || gCheats.god)
		return;
	if (wclock < noRepeatUntil[id])
		return;
	noRepeatUntil[id] = wclock + noRepeat;
	for (int i = 0; i < suitQueued; i++)
		if (suitQueue[i] == id)
			return;
	if (suitQueued < 4)
		suitQueue[suitQueued++] = id;
}

static void suit_voice_tick(f32 dt)
{
	if (sayingSent >= 0) {
		if (wordWait > 0) {
			wordWait -= dt;
			return;
		}
		if (sayingHandle && Audio_IsPlaying(sayingHandle))
			return;
		if (sayingWord >= sentLen[sayingSent]) {
			sayingSent = -1;
			return;
		}
		s16 w = sentWords[sayingSent][sayingWord++];
		sayingHandle = 0;
		if (w == -2)
			wordWait = 0.25f; /* a comma in sentences.txt */
		else if (w >= 0 && (u32)w < wp.numSounds)
			sayingHandle = Audio_Play(&wp.sounds[w], NULL, 1.0f, 0);
		return;
	}
	/* CheckSuitUpdate: one sentence every 3.5 s at most */
	if (suitQueued && wclock >= suitNext && deadAt == 0) {
		sayingSent = suitQueue[0];
		memmove(suitQueue, suitQueue + 1, sizeof(int) * (size_t)(--suitQueued));
		sayingWord = 0;
		sayingHandle = 0;
		wordWait = 0;
		suitNext = wclock + 3.5f;
	}
}

static void suit_voice_reset(void)
{
	suitQueued = 0;
	sayingSent = -1;
	sayingHandle = 0;
	memset(noRepeatUntil, 0, sizeof(noRepeatUntil));
}

/* ------------------------------------------------------------------ footsteps */

/* CBasePlayer::UpdateStepSound / PlayStepSound: the surface under the player picks the sound
 * (scripts/surfaceproperties*.txt), left and right feet alternate, every 300 ms running or 400 ms
 * walking (+100 crouched), quieter walking than running */
static const s16 *stepSnd; /* per surface: left, right */
static u32 numStepSurf;
static f32 stepTime;
static int stepLeft;
static u32 defaultSurf; /* floors that aren't the world (props, brush entities) */

void Weap_SetSteps(const u8 *c, u32 size)
{
	stepSnd = NULL;
	numStepSurf = 0;
	if (!c || size < 2)
		return;
	if (size < 4)
		return;
	u32 n = (u32)(c[0] << 8 | c[1]);
	if (4 + n * 4 > size)
		return;
	defaultSurf = (u32)(c[2] << 8 | c[3]);
	stepSnd = (const s16 *)(c + 4);
	numStepSurf = n;
}

/* impact sounds per surface (weapons.hgm SURF): props hitting things, bullets hitting the world */
static const s16 *surfSnd; /* per surface: hard, soft, bullet, scrape */
static const u8 *surfMat;   /* per surface id: gamematerial letter (SMAT) */
static u32 numSurfMat;
static u32 numSurfSnd, surfDefault, surfFlesh;

static void set_surfaces(const u8 *c, u32 size)
{
	surfSnd = NULL;
	numSurfSnd = 0;
	if (!c || size < 8)
		return;
	u32 n = (u32)(c[0] << 8 | c[1]);
	if (8 + n * 8 > size)
		return;
	surfDefault = (u32)(c[2] << 8 | c[3]);
	surfFlesh = (u32)(c[4] << 8 | c[5]);
	surfSnd = (const s16 *)(c + 8);
	numSurfSnd = n;
}

int Weap_EntSurface(s32 ent)
{
	u32 ne;
	Entity *E = Ent_List(&ne);
	if (ent <= 0 || (u32)ent >= ne)
		return (int)surfDefault;
	if (E[ent].npcModel >= 0)
		return (int)surfFlesh;
	if (map && map->modelSurf && E[ent].mdl >= 0 && (u32)E[ent].mdl < map->numModelSurf)
		return map->modelSurf[E[ent].mdl];
	return (int)surfDefault;
}

/* kind: 0 hard impact, 1 soft impact, 2 bullet impact, 3 scrape */
void Weap_SurfaceSound(int surf, int kind, const f32 at[3], f32 vol)
{
	if (!surfSnd || kind < 0 || kind > 3 || surf == SURF_SKY)
		return;
	if (surf < 0 || (u32)surf >= numSurfSnd)
		surf = (int)surfDefault;
	if ((u32)surf >= numSurfSnd)
		return;
	s16 snd = surfSnd[surf * 4 + kind];
	if (snd < 0 && (u32)surfDefault < numSurfSnd)
		snd = surfSnd[surfDefault * 4 + kind];
	if (snd >= 0 && (u32)snd < wp.numSounds)
		Audio_Play(&wp.sounds[snd], at, vol, SNDLVL_DEFAULT);
}

/* the surface under the feet: what a short trace down hits (brush side or displacement) */
static int step_surface(const f32 feet[3])
{
	Trace tr;
	f32 mn[3] = {-8, -8, 0}, mx[3] = {8, 8, 8};
	f32 a[3] = {feet[0], feet[1], feet[2] + 4}, b[3] = {feet[0], feet[1], feet[2] - 20};
	CM_BoxTrace(&tr, a, b, mn, mx, CONTENTS_SOLID);
	if (tr.fraction < 1 && !tr.startsolid && tr.surface >= 0)
		return tr.surface;
	return -1;
}

static void footsteps(f32 dt)
{
	if (!player || player->noclip || player->frozen || deadAt > 0)
		return;
	f32 speed = sqrtf(player->velocity[0] * player->velocity[0] + player->velocity[1] * player->velocity[1]);
	int ducked = player->ducked != 0;
	f32 velwalk = ducked ? 60 : 90, velrun = ducked ? 80 : 220;
	if (stepTime > 0)
		stepTime -= dt * 1000.0f;
	if (stepTime > 0)
		return;
	if (player->waterLevel >= 3) {
		/* swimming strokes */
		if (speed > 80) {
			f32 at[3] = {player->origin[0], player->origin[1], player->origin[2]};
			Weap_PlaySound(X_SWIM, at, 0.5f);
			stepTime = 1000;
		}
		return;
	}
	if (!player->onGround && player->waterLevel < 2)
		return;
	if (speed < velwalk)
		return;
	int walking = speed < velrun;
	f32 vol = walking ? 0.2f : 0.5f;
	int which;
	s16 snd = -1;
	if (player->waterLevel == 2) {
		which = stepLeft ? X_WADE_STEP_L : X_WADE_STEP_R;
		stepTime = 600; /* STEPSOUNDTIME_WATER_KNEE */
		vol = 0.65f;
	} else if (player->waterLevel == 1) {
		which = stepLeft ? X_WATER_STEP_L : X_WATER_STEP_R;
		stepTime = walking ? 400 : 300;
	} else {
		which = -1;
		stepTime = walking ? 400 : 300;
		int sid = step_surface(player->origin);
		u32 surf = sid >= 0 ? (u32)sid : defaultSurf;
		if (stepSnd && surf < numStepSurf)
			snd = stepSnd[surf * 2 + (stepLeft ? 0 : 1)];
	}
	if (ducked) {
		stepTime += 100;
		vol *= 0.65f;
	}
	stepLeft = !stepLeft;
	f32 at[3] = {player->origin[0], player->origin[1], player->origin[2]};
	if (which >= 0)
		Weap_PlaySound(which, at, vol);
	else if (snd >= 0 && (u32)snd < wp.numSounds)
		Audio_Play(&wp.sounds[snd], at, vol, SNDLVL_DEFAULT);
}

/* ------------------------------------------------------------------ damage indicator, Geiger counter */

static f32 dmgDir[4]; /* front, right, back, left: alpha 0..1, fading */
static f32 geigerRange = 1000, geigerReset;

void Weap_GeigerRange(f32 range)
{
	if (range < geigerRange)
		geigerRange = range;
}

static void geiger_tick(f32 dt)
{
	/* CBasePlayer::UpdateGeigerCounter sends the range 4 times a second, then resets it */
	static f32 range = 1000;
	geigerReset -= dt;
	if (geigerReset <= 0) {
		range = geigerRange;
		geigerRange = 1000;
		geigerReset = 0.25f;
	}
	if (range >= 800 || !inv.suit || deadAt > 0)
		return;
	/* hud_geiger.cpp: the closer, the more clicks (checked each frame, here scaled to the tick) */
	int pct;
	f32 vol;
	int high;
	if (range > 600) { pct = 2; vol = 0.4f; high = 0; }
	else if (range > 500) { pct = 4; vol = 0.5f; high = 0; }
	else if (range > 400) { pct = 8; vol = 0.6f; high = 0; }
	else if (range > 300) { pct = 8; vol = 0.6f; high = 1; }
	else if (range > 200) { pct = 28; vol = 0.6f; high = 1; }
	else if (range > 150) { pct = 40; vol = 0.8f; high = 1; }
	else if (range > 100) { pct = 60; vol = 0.85f; high = 1; }
	else if (range > 75) { pct = 80; vol = 0.9f; high = 1; }
	else if (range > 50) { pct = 90; vol = 0.95f; high = 1; }
	else { pct = 95; vol = 1.0f; high = 1; }
	if ((rand() & 127) < pct * dt * 60.0f * 0.5f) /* frames at 60 Hz, ~half chance as in the HUD */
		Weap_PlaySound(high && (rand() & 1) ? X_GEIGER_HIGH : X_GEIGER_LOW, NULL, vol);
}

static void damage_direction(const f32 *from)
{
	if (!from) {
		return;
	}
	f32 d[2] = {from[0] - eye[0], from[1] - eye[1]};
	f32 l = sqrtf(d[0] * d[0] + d[1] * d[1]);
	if (l < 16) {
		for (int i = 0; i < 4; i++)
			dmgDir[i] = 1;
		return;
	}
	f32 yr = viewYaw * (f32)M_PI / 180.0f;
	f32 fwd = (d[0] * cosf(yr) + d[1] * sinf(yr)) / l;
	f32 rgt = (d[0] * sinf(yr) - d[1] * cosf(yr)) / l;
	/* CHudDamageIndicator::CalcDamageDirection: the sides the hit came from */
	if (fwd > 0.3f)
		dmgDir[0] = 1;
	if (fwd < -0.3f)
		dmgDir[2] = 1;
	if (rgt > 0.3f)
		dmgDir[1] = 1;
	if (rgt < -0.3f)
		dmgDir[3] = 1;
}

static void draw_damage_indicator(u16 width, u16 height)
{
	/* red gradients along the edges the damage came from */
	const int steps = 8;
	for (int side = 0; side < 4; side++) {
		if (dmgDir[side] <= 0)
			continue;
		for (int k = 0; k < steps; k++) {
			f32 a = dmgDir[side] * 150.0f * (1.0f - (f32)k / steps) / steps * 2;
			u32 c = 0xC0000000u | (u32)(a > 255 ? 255 : a);
			f32 t0 = k * 10.0f, t1 = t0 + 10.0f;
			switch (side) {
			case 0: Text_Rect(width * 0.2f, t0, width * 0.8f, t1, c); break;
			case 2: Text_Rect(width * 0.2f, height - t1, width * 0.8f, height - t0, c); break;
			case 1: Text_Rect(width - t1, height * 0.2f, width - t0, height * 0.8f, c); break;
			case 3: Text_Rect(t0, height * 0.2f, t1, height * 0.8f, c); break;
			}
		}
	}
}

int Weap_Setup(const Pack *p, const void *weapRecs, u32 nw, const void *itemRecs, u32 ni, const s16 *ws, u32 nws,
               const s16 *ammx, u32 nam)
{
	if (p != &wp)
		wp = *p;
	weaps = (const WeapRec *)weapRecs;
	numWeaps = nw > MAX_WEAPONS ? MAX_WEAPONS : nw;
	items = (const ItemRec *)itemRecs;
	numItems = items ? ni : 0;
	for (int i = 0; i < NUM_X; i++)
		xsnd[i] = (u32)i < nws ? ws[i] : -1;
	memset(ammoMax, 0, sizeof(ammoMax));
	for (int i = 0; i < NUM_AMMO && (u32)i < nam; i++)
		ammoMax[i] = ammx[i];
	loaded = 1;
	Weap_NewGame();
	return 0;
}

#ifndef HOST_TEST
int Weap_Load(u8 *data, u32 size)
{
	if (Pack_LoadMemory(&wp, data, size) != 0)
		return -1;
	u32 sz, isz = 0, wsz = 0, asz = 0;
	u8 *c = Pack_Chunk(&wp, "WEAP", &sz);
	if (!c)
		return -1;
	u8 *ic = Pack_Chunk(&wp, "ITEM", &isz);
	u8 *wc = Pack_Chunk(&wp, "WSND", &wsz);
	u8 *ac = Pack_Chunk(&wp, "AMMX", &asz);
	{
		u32 isz2 = 0;
		u8 *icn = Pack_Chunk(&wp, "WICN", &isz2);
		memset(wIcon, 0, sizeof(wIcon));
		if (icn)
			memcpy(wIcon, icn, isz2 < sizeof(wIcon) ? isz2 : sizeof(wIcon));
	}
	{
		u32 fsz2 = 0;
		u8 *fx = Pack_Chunk(&wp, "FXTX", &fsz2);
		for (int i = 0; i < FX_COUNT; i++)
			fxTex[i] = -1;
		if (fx && fsz2 >= 2) {
			u32 n = (u32)(fx[0] << 8 | fx[1]);
			for (u32 i = 0; i < n && i < FX_COUNT && 2 + i * 2 + 1 < fsz2; i++)
				fxTex[i] = (s16)(fx[2 + i * 2] << 8 | fx[3 + i * 2]);
		}
	}
	u32 hsz = 0;
	u8 *hc = Pack_Chunk(&wp, "HSEN", &hsz);
	Weap_SetSentences(hc, hsz);
	u32 ssz = 0;
	u8 *sc2 = Pack_Chunk(&wp, "STEP", &ssz);
	Weap_SetSteps(sc2, ssz);
	u32 fsz = 0;
	u8 *fc = Pack_Chunk(&wp, "SURF", &fsz);
	set_surfaces(fc, fsz);
	{
		u32 msz = 0;
		u8 *mc = Pack_Chunk(&wp, "SMAT", &msz);
		surfMat = NULL;
		numSurfMat = 0;
		if (mc && msz >= 2) {
			u32 n = (u32)(mc[0] << 8 | mc[1]);
			if (2 + n <= msz) {
				surfMat = mc + 2;
				numSurfMat = n;
			}
		}
	}
	if (Render_InitSkinPack(&wp) != 0)
		return -1;
	Weap_Setup(&wp, c + 4, *(u32 *)c, ic ? ic + 4 : NULL, ic ? *(u32 *)ic : 0, (const s16 *)wc, wsz / 2,
	           (const s16 *)ac, asz / 2);
	printf("weapons: %u weapons, %u items\n", (unsigned)numWeaps, (unsigned)numItems);
	return 0;
}
#endif

int Weap_Loaded(void) { return loaded; }

void Weap_NewGame(void)
{
	memset(&inv, 0, sizeof(inv));
	inv.cur = inv.last = -1;
	inv.health = 100;
	inv.aux = 100;
	deadAt = 0;
	flashlightOn = 0;
	vseq = vprev = -1;
	suit_voice_reset();
	memset(dmgDir, 0, sizeof(dmgDir));
}

/* ---- the suit's auxiliary power */

int Weap_SprintAllowed(int wantSprint, int moving)
{
	/* sprinting needs the suit and some power (HL2 won't start a sprint under 10) */
	if (!wantSprint || !inv.suit || !moving || inv.aux <= 0 || (!sprintActive && inv.aux < 10)) {
		sprintActive = 0;
		return 0;
	}
	sprintActive = 1;
	return 1;
}

void Weap_ToggleFlashlight(void)
{
	if (!inv.suit)
		return;
	if (!flashlightOn && inv.aux <= 0)
		return;
	flashlightOn = !flashlightOn;
	play(xsnd[X_FLASHLIGHT], NULL, 1, 0);
}

int Weap_FlashlightOn(void) { return flashlightOn && deadAt == 0; }

static void suit_tick(f32 dt)
{
	if (!player)
		return;
	int under = player->waterLevel >= 3 && !player->noclip;
	breatherActive = under && inv.suit;
	f32 load = (sprintActive ? AUX_SPRINT : 0) + (flashlightOn ? AUX_FLASHLIGHT : 0) +
	           (breatherActive ? AUX_BREATHER : 0);
	if (gCheats.god)
		load = 0;
	if (load > 0) {
		inv.aux -= load * dt;
		if (inv.aux <= 0) {
			/* drained: every device switches off */
			inv.aux = 0;
			sprintActive = 0;
			flashlightOn = 0;
		}
	} else if (inv.suit && inv.aux < 100) {
		inv.aux += AUX_CHARGE_RATE * dt;
		if (inv.aux > 100)
			inv.aux = 100;
	}

	/* drowning (CBasePlayer::WaterMove): air lasts while the suit has power, then 7 s, then 10 a second */
	int canBreathe = !under || (breatherActive && inv.aux > 0);
	if (canBreathe || gCheats.god) {
		airFinished = wclock + AIRTIME;
		/* breathing again gives the drowning damage back, 10 every 2 s */
		if (drownDmg > drownRestored && wclock >= recoverAt && deadAt == 0) {
			f32 give = drownDmg - drownRestored > 10 ? 10 : drownDmg - drownRestored;
			inv.health = inv.health + give > 100 ? 100 : inv.health + give;
			drownRestored += give;
			recoverAt = wclock + 2.0f;
		}
	} else if (wclock > airFinished && wclock >= painFinished && deadAt == 0) {
		f32 before = inv.health;
		f32 armor = inv.armor;
		inv.armor = 0; /* the suit's armour doesn't help against drowning */
		Weap_HurtFrom(DROWN_DAMAGE, DMG_DROWN, NULL);
		inv.armor = armor;
		drownDmg += before - inv.health;
		painFinished = wclock + 1.0f;
		recoverAt = wclock + 2.0f;
	}
}

int Weap_HasSuit(void) { return inv.suit; }
f32 Weap_DebugHealth(void) { return inv.health; }
int Weap_DebugAux(void) { return (int)inv.aux; }

/* ------------------------------------------------------------------ viewmodel animation */

static const PackSkel *vm_skel(s32 w)
{
	if (w < 0 || !loaded)
		return NULL;
	s16 m = weaps[w].vm;
	if (m < 0 || (u32)m >= wp.numNpcModels)
		return NULL;
	u16 s = wp.npcModels[m].skel;
	return s < wp.numSkels ? &wp.skels[s] : NULL;
}

/* plays one of the weapon's sequences; returns its length */
static f32 vm_play(int which)
{
	const PackSkel *sk = vm_skel(inv.cur);
	if (!sk)
		return 0.3f;
	s16 s = weaps[inv.cur].seq[which];
	if (s < 0)
		s = weaps[inv.cur].seq[SQ_IDLE];
	if (s < 0)
		return 0.3f;
	vprev = vseq;
	vprevT = vt;
	vblend = (vprev >= 0 && which != SQ_PRIMARY && which != SQ_SECONDARY && which != SQ_MISS) ? 0 : 1;
	vseq = s;
	vt = 0;
	const PackSeq *ps = &wp.npcSeqs[sk->firstSeq + (u32)s];
	vhold = !(ps->flags & SEQF_LOOP) && which != SQ_IDLE;
	f32 d = Anim_SeqDuration(&wp, sk, s);
	seqEnd = which == SQ_IDLE ? 0 : wclock + d;
	return d > 0.05f ? d : 0.3f;
}

static void select_weapon(s32 w)
{
	if (w < 0 || !inv.have[w])
		return;
	if (switchTo >= 0) {
		switchTo = w; /* changed our mind while the last one goes away */
		selShowUntil = wclock + 1.5f;
		return;
	}
	if (w == inv.cur)
		return;
	if (Phys_HeldEnt() >= 0 && inv.cur >= 0 && (weaps[inv.cur].flags & F_PHYS))
		Phys_Drop(0);
	reloadDone = 0;
	selShowUntil = wclock + 1.5f;
	if (inv.cur >= 0) {
		f32 d = vm_play(SQ_HOLSTER);
		if (weaps[inv.cur].seq[SQ_HOLSTER] < 0)
			d = 0.1f;
		switchTo = w;
		switchAt = wclock + (d < 0.5f ? d : 0.5f);
		return;
	}
	inv.cur = w;
	nextAttack = nextAlt = wclock + vm_play(SQ_DRAW) * 0.8f;
}

static int weapon_usable(s32 w)
{
	if (w < 0 || !inv.have[w])
		return 0;
	const WeapRec *r = &weaps[w];
	if (r->ammo == A_NONE)
		return 1;
	return inv.clip[w] > 0 || inv.ammo[r->ammo] > 0;
}

/* Source's slot order: slot, then position in the slot (table order) */
static void cycle_weapon(int dir)
{
	if (!numWeaps)
		return;
	s32 start = inv.cur >= 0 ? inv.cur : 0;
	for (u32 k = 1; k <= numWeaps; k++) {
		s32 w = (s32)(((s32)start + dir * (s32)k + (s32)numWeaps * 4) % (s32)numWeaps);
		if (weapon_usable(w)) {
			if (inv.cur >= 0)
				inv.last = inv.cur;
			select_weapon(w);
			return;
		}
	}
}

/* ------------------------------------------------------------------ effects */

static Fx *new_fx(int fx, const f32 at[3])
{
	for (int i = 0; i < MAX_FX; i++)
		if (!fxs[i].alive) {
			fxs[i].alive = 1;
			fxs[i].type = (u8)fx;
			fxs[i].flags = 0;
			fxs[i].scale = 1;
			memcpy(fxs[i].pos, at, 12);
			fxs[i].t = 0;
			fxs[i].dur = fx == WFX_EXPLOSION ? 0.9f : fx == WFX_SPARK ? 0.15f : fx == WFX_MUZZLE ? 0.06f : 0.25f;
			return &fxs[i];
		}
	return NULL;
}

void Weap_Effect(int fx, const f32 at[3])
{
	if (new_fx(fx, at) && fx == WFX_EXPLOSION)
		play(xsnd[X_EXPLODE], at, 1.0f, 4000.0f);
}

/* ---- bullet impacts (Source's UTIL_ImpactTrace -> the surface's impact effect and its "Impact.*"
 * decal): a hole left on the wall, and dust / sparks / blood thrown off the hit point */
static void sample_tint(const f32 pos[3], u8 out[4]);

#define MAX_DECALS 96
typedef struct {
	f32 p[3], u[3], v[3];
	f32 r;
	u32 rgba;
	u8 alive, seed;
	s8 tex; /* FX_* slot of its decal texture (-1: none, a plain dark mark) */
} Decal;
static Decal decals[MAX_DECALS];
static int nextDecal;

#define MAX_PUFFS 96
typedef struct {
	f32 pos[3], vel[3];
	f32 t, dur, s0, s1, grav;
	u32 rgba;
	u8 alive, add;
	s8 tex; /* FX_* slot */
	u8 roll;
} Puff;
static Puff puffs[MAX_PUFFS];
static int nextPuff;

static f32 frand01(void) { return (f32)(rand() & 0xFFFF) / 65535.0f; }

static void puff(const f32 at[3], const f32 vel[3], f32 dur, f32 s0, f32 s1, f32 grav, u32 rgba, int add, int tex)
{
	Puff *q = &puffs[nextPuff];
	q->tex = (s8)tex;
	q->roll = (u8)rand();
	nextPuff = (nextPuff + 1) % MAX_PUFFS;
	memcpy(q->pos, at, 12);
	memcpy(q->vel, vel, 12);
	q->t = 0;
	q->dur = dur;
	q->s0 = s0;
	q->s1 = s1;
	q->grav = grav;
	q->rgba = rgba;
	q->alive = 1;
	q->add = (u8)add;
}

static int surface_material(int surf)
{
	if (surf == SURF_SKY)
		return 0;
	if (!surfMat || surf < 0 || (u32)surf >= numSurfMat)
		return 'C';
	return surfMat[surf];
}

static void add_decal(const f32 at[3], const f32 n[3], f32 r, u32 rgba, int tex)
{
	Decal *d = &decals[nextDecal];
	d->tex = (s8)tex;
	nextDecal = (nextDecal + 1) % MAX_DECALS;
	/* a frame on the surface: any axis not along the normal, crossed twice */
	f32 a[3] = {0, 0, 1};
	if (fabsf(n[2]) > 0.9f)
		a[0] = 1, a[2] = 0;
	f32 u[3] = {a[1] * n[2] - a[2] * n[1], a[2] * n[0] - a[0] * n[2], a[0] * n[1] - a[1] * n[0]};
	f32 l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	for (int k = 0; k < 3; k++)
		u[k] /= l;
	f32 v[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
	for (int k = 0; k < 3; k++) {
		d->p[k] = at[k] + n[k] * 0.4f; /* off the surface, so it doesn't fight it for depth */
		d->u[k] = u[k];
		d->v[k] = v[k];
	}
	d->r = r;
	d->rgba = rgba;
	d->seed = (u8)rand();
	d->alive = 1;
}

/* the world's light at the hit, so dust in a dark corridor isn't bright grey */
static u32 lit_colour(const f32 at[3], u32 rgb, u32 alpha)
{
	u8 t[4];
	sample_tint(at, t);
	u32 r = ((rgb >> 24) & 0xFF) * (t[0] + 40) / 168, g = ((rgb >> 16) & 0xFF) * (t[1] + 40) / 168,
	    b = ((rgb >> 8) & 0xFF) * (t[2] + 40) / 168;
	r = r > 255 ? 255 : r;
	g = g > 255 ? 255 : g;
	b = b > 255 ? 255 : b;
	return r << 24 | g << 16 | b << 8 | alpha;
}

void Weap_BulletImpact(const Trace *tr, const f32 dir[3])
{
	if (tr->fraction >= 1 || tr->startsolid)
		return;
	int flesh = tr->ent > 0 && Weap_EntSurface(tr->ent) == (int)surfFlesh;
	int surf = tr->ent > 0 ? Weap_EntSurface(tr->ent) : tr->surface;
	int mat = flesh ? 'F' : surface_material(surf);
	if (!mat)
		return; /* sky */
	f32 n[3] = {tr->normal[0], tr->normal[1], tr->normal[2]};
	if (flesh || n[0] * n[0] + n[1] * n[1] + n[2] * n[2] < 0.5f)
		for (int k = 0; k < 3; k++)
			n[k] = -dir[k];
	f32 at[3] = {tr->endpos[0] + n[0] * 1.5f, tr->endpos[1] + n[1] * 1.5f, tr->endpos[2] + n[2] * 1.5f};
	/* the bounce direction: out of the surface, a bit along the shot's reflection */
	f32 dn = dir[0] * n[0] + dir[1] * n[1] + dir[2] * n[2];
	f32 refl[3] = {dir[0] - 2 * dn * n[0], dir[1] - 2 * dn * n[1], dir[2] - 2 * dn * n[2]};
	int v3 = rand() % 3;
	switch (mat) {
	case 'F': case 'B': case 'H': case 'A': case 'E': case 'Z': { /* flesh: a spray of blood (blood_core) */
		u32 c = mat == 'A' || mat == 'E' || mat == 'Z' ? 0xC8C82800u : 0xB0000000u;
		for (int i = 0; i < 3; i++) {
			f32 v[3] = {n[0] * 40 + (frand01() - 0.5f) * 50, n[1] * 40 + (frand01() - 0.5f) * 50, n[2] * 40 + frand01() * 30};
			puff(at, v, 0.45f, 3, 10, 200, c | 0xE0, 0, FX_BLOOD);
		}
		return; /* (Source puts blood on the wall behind; not modelled) */
	}
	case 'M': case 'V': case 'G': case 'P': { /* metal: sparks (effects/spark), decals/metal/shot* */
		for (int i = 0; i < 4; i++) {
			f32 sp = 120 + frand01() * 160;
			f32 v[3] = {(refl[0] * 0.5f + n[0] * 0.5f + (frand01() - 0.5f) * 0.8f) * sp,
			            (refl[1] * 0.5f + n[1] * 0.5f + (frand01() - 0.5f) * 0.8f) * sp,
			            (refl[2] * 0.5f + n[2] * 0.5f + frand01() * 0.6f) * sp};
			puff(at, v, 0.25f + frand01() * 0.15f, 1.6f, 0.8f, 500, 0xFFFFFFFFu, 1, FX_SPARK);
		}
		f32 v0[3] = {n[0] * 15, n[1] * 15, n[2] * 15 + 5};
		puff(at, v0, 0.5f, 2, 7, 0, lit_colour(at, 0x9090A000u, 0) | 0x60, 0, FX_SMOKE);
		if (tr->ent <= 0)
			add_decal(tr->endpos, n, 3.2f, 0x141418E0u, FX_SHOT_METAL + v3);
		return;
	}
	case 'S': case 'Q': { /* water / slime: a splash (effects/splash2) */
		for (int i = 0; i < 3; i++) {
			f32 v[3] = {(frand01() - 0.5f) * 40, (frand01() - 0.5f) * 40, 90 + frand01() * 60};
			puff(at, v, 0.5f, 3, 9, 400, lit_colour(at, 0xC8D0D800u, 0) | 0xC0, 0, FX_SPLASH);
		}
		return;
	}
	case 'Y': { /* glass: bits, decals/glass/shot* */
		for (int i = 0; i < 3; i++) {
			f32 v[3] = {n[0] * 60 + (frand01() - 0.5f) * 80, n[1] * 60 + (frand01() - 0.5f) * 80, n[2] * 60 + frand01() * 40};
			puff(at, v, 0.4f, 1, 0.6f, 500, 0xD0E0FFC0u, 1, FX_SPARK);
		}
		if (tr->ent <= 0)
			add_decal(tr->endpos, n, 7.0f, 0xC8D0D870u, FX_SHOT_GLASS + v3);
		return;
	}
	default: { /* concrete, wood, dirt, tile...: dust (particle_smokegrenade), flecks, a hole */
		int wood = mat == 'W';
		u32 col = wood ? 0x8C6E4C00u : (mat == 'D' || mat == 'N' || mat == 'O') ? 0x7A6A5000u : 0x9C988E00u;
		u32 c = lit_colour(at, col, 0);
		for (int i = 0; i < 3; i++) {
			f32 sp = 20 + frand01() * 30;
			f32 v[3] = {(n[0] + refl[0] * 0.3f + (frand01() - 0.5f) * 0.6f) * sp, (n[1] + refl[1] * 0.3f + (frand01() - 0.5f) * 0.6f) * sp,
			            (n[2] + refl[2] * 0.3f + (frand01() - 0.5f) * 0.6f) * sp + 6};
			puff(at, v, 0.7f + frand01() * 0.5f, 3, 12 + frand01() * 6, -10, c | 0xA0, 0, FX_SMOKE);
		}
		u32 fc = lit_colour(at, 0xFFFFFF00u, 0);
		for (int i = 0; i < 3; i++) { /* flecks (effects/fleck_cement*, fleck_wood*) */
			f32 v[3] = {(n[0] + (frand01() - 0.5f)) * 110, (n[1] + (frand01() - 0.5f)) * 110, (n[2] + frand01() * 0.5f) * 110};
			puff(at, v, 0.6f, 1.0f, 1.0f, 600, fc | 0xFF, 0, (wood ? FX_FLECK_WOOD : FX_FLECK_CEMENT) + (i & 1));
		}
		if (tr->ent <= 0)
			add_decal(tr->endpos, n, 3.2f, wood ? 0x1E140CE8u : 0x161412E8u, (wood ? FX_SHOT_WOOD : FX_SHOT_CONCRETE) + v3);
		return;
	}
	}
}

void Weap_ClearImpacts(void)
{
	memset(decals, 0, sizeof(decals));
	memset(puffs, 0, sizeof(puffs));
}

int Weap_ImpactCounts(int *numDecals)
{
	int d = 0, p = 0;
	for (int i = 0; i < MAX_DECALS; i++)
		d += decals[i].alive;
	for (int i = 0; i < MAX_PUFFS; i++)
		p += puffs[i].alive;
	if (numDecals)
		*numDecals = d;
	return p;
}

static void tick_puffs(f32 dt)
{
	for (int i = 0; i < MAX_PUFFS; i++) {
		Puff *q = &puffs[i];
		if (!q->alive)
			continue;
		if ((q->t += dt) >= q->dur) {
			q->alive = 0;
			continue;
		}
		q->vel[2] -= q->grav * dt;
		f32 drag = 1.0f - dt * (q->add ? 0.5f : 2.5f);
		for (int k = 0; k < 3; k++) {
			q->vel[k] *= drag;
			q->pos[k] += q->vel[k] * dt;
		}
	}
}

static void explode(const f32 at[3], f32 damage, f32 radius)
{
	Weap_Effect(WFX_EXPLOSION, at);
	Ent_RadiusDamage(at, damage, radius);
}

/* ---- effects for the map's effect entities (envents.c), after Source's g_pEffects / te calls */

/* g_pEffects->Sparks: a burst of glowing streaks thrown out (along dir if given) that fall */
void Weap_Sparks(const f32 at[3], const f32 *dir, int magnitude, int trail)
{
	int n = 4 + magnitude * 6;
	n = n > 40 ? 40 : n;
	f32 speed = 120.0f + magnitude * 60.0f;
	for (int i = 0; i < n; i++) {
		f32 v[3] = {frand01() * 2 - 1, frand01() * 2 - 1, frand01() * 2 - 1};
		if (dir)
			for (int k = 0; k < 3; k++)
				v[k] = dir[k] * 1.5f + v[k] * 0.6f;
		f32 sp = speed * (0.5f + frand01());
		for (int k = 0; k < 3; k++)
			v[k] *= sp;
		puff(at, v, 0.2f + frand01() * 0.3f, 1.0f + trail * 0.4f, 0.5f, 600, 0xFFFFFFFFu, 1, FX_SPARK);
	}
	Weap_Effect(WFX_SPARK, at);
}

/* dust / smoke: count puffs of rgb (alpha added) drifting along dir */
void Weap_Dust(const f32 at[3], const f32 *dir, f32 scale, u32 rgb, f32 speed, int count)
{
	for (int i = 0; i < count; i++) {
		f32 v[3] = {frand01() * 2 - 1, frand01() * 2 - 1, frand01() * 2 - 1};
		for (int k = 0; k < 3; k++)
			v[k] = ((dir ? dir[k] : 0) + v[k] * 0.4f) * speed;
		puff(at, v, 1.2f + frand01(), 4 * scale, 12 * scale, -10, lit_colour(at, rgb, 0) | 0x90, 0, FX_SMOKE);
	}
}

/* env_splash / Physics.WaterSplash: water thrown up */
void Weap_Splash(const f32 at[3], f32 scale)
{
	for (int i = 0; i < 10; i++) {
		f32 v[3] = {(frand01() - 0.5f) * 120 * scale, (frand01() - 0.5f) * 120 * scale, (150 + frand01() * 150) * scale};
		puff(at, v, 0.6f, 4 * scale, 10 * scale, 500, lit_colour(at, 0xC8D0D800u, 0) | 0xC0, 0, FX_SPLASH);
	}
}

/* env_explosion: the fireball (and smoke, sparks, sound) unless flagged off */
void Weap_Explosion(const f32 at[3], f32 scale, int flags)
{
	Npc_HearCombat(at, 1024.0f * (scale > 1 ? scale : 1)); /* (an explosion is heard) */
	Fx *f = (flags & EXPL_NOFIREBALL) && (flags & EXPL_NOSMOKE) ? NULL : new_fx(WFX_EXPLOSION, at);
	if (f) {
		f->flags = (u8)flags;
		f->scale = scale < 0.3f ? 0.3f : scale > 3 ? 3 : scale;
	}
	if (!(flags & EXPL_NOSOUND))
		play(xsnd[X_EXPLODE], at, 1.0f, 4000.0f);
	if (!(flags & EXPL_NOSPARKS))
		Weap_Sparks(at, NULL, 2, 2);
}

/* env_muzzleflash, env_gunfire */
void Weap_Muzzle(const f32 at[3], f32 scale)
{
	Fx *f = new_fx(WFX_MUZZLE, at);
	if (f)
		f->scale = scale;
}

/* ------------------------------------------------------------------ the player's health */

void Weap_Hurt(f32 damage) { Weap_HurtFrom(damage, 0, NULL); }

void Weap_HurtFrom(f32 damage, u32 type, const f32 *from)
{
	if (deadAt > 0 || damage <= 0 || gCheats.god || Ent_NoTarget())
		return;
	f32 healthPrev = inv.health;
	/* the suit takes 80% of it, using one point of armour per point absorbed (Source's defaults);
	 * falling and drowning go straight through */
	if (inv.armor > 0 && !(type & (DMG_FALL | DMG_DROWN | DMG_POISON | DMG_RADIATION))) { /* (CBasePlayer: these go past the suit) */
		f32 take = damage * 0.2f;
		f32 absorbed = damage - take;
		if (absorbed > inv.armor) {
			take += absorbed - inv.armor;
			inv.armor = 0;
		} else {
			inv.armor -= absorbed;
		}
		damage = take;
	}
	inv.health -= damage;
	if ((type & DMG_POISON) && inv.health > 0) {
		/* CBasePlayer: poison damage is given back over time (the suit's antidote) */
		poisonLeft += damage;
		poisonNext = wclock + 2.0f;
	}
	flashUntil = wclock + 0.4f;
	flashAmt = damage > 40 ? 1.0f : 0.35f + damage / 60.0f;
	damage_direction(from);
	if (inv.health <= 0) {
		inv.health = 0;
		deadAt = wclock > 0 ? wclock : 0.001f;
		if (Phys_Holding())
			Phys_Drop(0);
		return;
	}
	/* CBasePlayer::OnTakeDamage: the suit's diagnosis */
	int trivial = inv.health > 75 || damage < 5;
	int major = damage > 25;
	int critical = inv.health < 30;
	const u32 timeBased = DMG_PARALYZE | DMG_NERVEGAS | DMG_POISON | DMG_RADIATION | DMG_ACID | DMG_SLOWBURN;
	if (!trivial || (type & timeBased)) {
		if ((type & DMG_CLUB) && major)
			suit_say(SUIT_DMG4, 30); /* minor fracture */
		if (type & (DMG_FALL | DMG_CRUSH))
			suit_say(major ? SUIT_DMG5 : SUIT_DMG4, 30);
		if ((type & DMG_BULLET) && damage > 5)
			suit_say(SUIT_DMG6, 30); /* blood loss */
		if (type & DMG_SLASH)
			suit_say(major ? SUIT_DMG1 : SUIT_DMG0, 30);
		if ((type & DMG_SONIC) && major)
			suit_say(SUIT_DMG2, 60);
		if (type & (DMG_POISON | DMG_PARALYZE))
			suit_say(SUIT_DMG3, 60);
		if (type & DMG_ACID)
			suit_say(SUIT_DET1, 60);
		if (type & DMG_NERVEGAS)
			suit_say(SUIT_DET0, 60);
		if (type & DMG_RADIATION)
			suit_say(SUIT_DET2, 60);
	}
	if (!trivial && major && healthPrev >= 75) {
		suit_say(SUIT_MED1, 1800); /* automedic on */
		suit_say(SUIT_HEAL7, 1800); /* morphine */
	}
	if (!trivial && critical && healthPrev < 75) {
		if (inv.health < 6)
			suit_say(SUIT_HLTH3, 600); /* near death */
		else if (inv.health < 20)
			suit_say(SUIT_HLTH2, 600); /* health critical */
		if ((rand() & 3) == 0 && healthPrev < 50)
			suit_say(SUIT_DMG7, 300); /* seek medical attention */
	}
	if ((type & timeBased) && healthPrev < 75) {
		if (healthPrev < 50) {
			if ((rand() & 3) == 0)
				suit_say(SUIT_DMG7, 300);
		} else {
			suit_say(SUIT_HLTH1, 600); /* health dropping */
		}
	}
}

f32 Weap_DeadTime(void) { return deadAt > 0 ? wclock - deadAt + 0.001f : 0; }

u32 Weap_DamageFlash(void)
{
	if (deadAt > 0) {
		f32 a = (wclock - deadAt) / 2.0f;
		if (a > 1)
			a = 1;
		return 0x60000000u | (u32)(a * 200);
	}
	if (wclock >= flashUntil)
		return 0;
	f32 a = (flashUntil - wclock) / 0.4f * flashAmt;
	return 0xC0000000u | (u32)(a * 110);
}

/* ------------------------------------------------------------------ inventory */

/* ---- HUD icons (halflife2.ttf glyphs, the weapon scripts' TextureData) and the pickup history
 * (CHudHistoryResource): what was just picked up, on the right, fading out over 5 s */

static u8 ammo_icon(int type, int *weapon)
{
	for (u32 i = 0; i < numWeaps && i < 32; i++) {
		const char *cls = wstr(weaps[i].name);
		if ((type == A_SMG1_GRENADE && !strcmp(cls, "weapon_smg1")) || (type == A_AR2_ALT && !strcmp(cls, "weapon_ar2"))) {
			*weapon = (int)i;
			return wIcon[i][2];
		}
		if (weaps[i].ammo == type && type != A_SMG1_GRENADE && type != A_AR2_ALT) {
			*weapon = (int)i;
			return wIcon[i][1];
		}
	}
	*weapon = -1;
	return 0;
}

enum { HIST_AMMO, HIST_WEAPON, HIST_ITEM };
#define MAX_HIST 5
typedef struct {
	u8 kind, icon, icon2, alive;
	s16 count, id;
	f32 until;
} Hist;
static Hist hist[MAX_HIST];
static int histSlot;

static void hist_add(int kind, int id, u8 icon, u8 icon2, int count)
{
	if (!icon)
		return;
	/* (the same ammo again while it's still up: add to it) */
	for (int i = 0; i < MAX_HIST; i++)
		if (hist[i].alive && kind == HIST_AMMO && hist[i].kind == HIST_AMMO && hist[i].id == id) {
			hist[i].count = (s16)(hist[i].count + count);
			hist[i].until = wclock + 5.0f;
			return;
		}
	int any = 0;
	for (int i = 0; i < MAX_HIST; i++)
		any |= hist[i].alive;
	if (!any || histSlot >= MAX_HIST)
		histSlot = 0; /* CHudHistoryResource starts again from the bottom once the list is clear */
	Hist *h = &hist[histSlot++];
	h->kind = (u8)kind;
	h->id = (s16)id;
	h->icon = icon;
	h->icon2 = icon2;
	h->count = (s16)count;
	h->until = wclock + 5.0f; /* hud_drawhistory_time */
	h->alive = 1;
}


static int give_ammo(int type, int amount)
{
	if (type <= A_NONE || type >= NUM_AMMO || amount <= 0)
		return 0;
	if (inv.ammo[type] >= ammoMax[type])
		return 0;
	int before = inv.ammo[type];
	inv.ammo[type] += amount;
	if (inv.ammo[type] > ammoMax[type])
		inv.ammo[type] = ammoMax[type];
	{
		int w;
		u8 ai = ammo_icon(type, &w);
		/* weapons with clips show the gun small with the ammo beside it; the rest just the ammo */
		u8 main = w >= 0 && weaps[w].clip > 0 && type != A_SMG1_GRENADE && type != A_AR2_ALT ? wIcon[w][0] : ai;
		hist_add(HIST_AMMO, type, main, main == ai ? 0 : ai, inv.ammo[type] - before);
	}
	return 1;
}

static int give_weapon(s32 w)
{
	if (w < 0 || (u32)w >= numWeaps)
		return 0;
	const WeapRec *r = &weaps[w];
	int fresh = !inv.have[w];
	int got = fresh;
	/* the default clip: guns come loaded, grenades and rockets come as ammo */
	int def = r->clip > 0 ? r->clip : (r->ammo == A_GRENADE ? 1 : r->ammo == A_RPG ? 3 : 0);
	if (fresh) {
		inv.have[w] = 1;
		if (r->clip > 0)
			inv.clip[w] = r->clip;
		else
			got |= give_ammo(r->ammo, def);
	} else {
		got |= give_ammo(r->ammo, def);
	}
	if (fresh && w < 32)
		hist_add(HIST_WEAPON, w, wIcon[w][0], 0, 0);
	if (fresh && strcmp(wstr(r->name), "weapon_bugbait")) {
		if (inv.cur >= 0)
			inv.last = inv.cur;
		select_weapon(w);
	}
	return got;
}

void Weap_GiveAll(void)
{
	inv.suit = 1;
	for (u32 i = 0; i < numWeaps; i++)
		if (strcmp(wstr(weaps[i].name), "weapon_bugbait"))
			give_weapon((s32)i);
	for (int a = 1; a < NUM_AMMO; a++)
		inv.ammo[a] = ammoMax[a];
	inv.health = 100;
	inv.armor = 100;
	inv.aux = 100;
}

void Weap_Strip(int dissolve)
{
	s32 keep = dissolve ? weapon_named("weapon_physcannon") : -1;
	for (u32 i = 0; i < numWeaps; i++)
		if ((s32)i != keep) {
			inv.have[i] = 0;
			inv.clip[i] = 0;
		}
	if (dissolve && keep >= 0 && inv.have[keep])
		inv.supercharged = 1;
	if (!dissolve)
		memset(inv.ammo, 0, sizeof(inv.ammo));
	if (inv.cur >= 0 && !inv.have[inv.cur]) {
		inv.cur = -1;
		vseq = -1;
		if (keep >= 0 && inv.have[keep])
			select_weapon(keep);
	}
	inv.last = -1;
}

/* an item touched: returns 1 if the player took it */
static int take_item(const WorldItem *it)
{
	const ItemRec *r = &items[it->item];
	const char *cls = wstr(r->name);
	switch (r->kind) {
	case K_HEALTH:
		if (!strcmp(cls, "item_dynamic_resupply")) {
			/* hands out what the player needs most, like on PC */
			if (inv.health < 100) {
				inv.health = inv.health + 25 > 100 ? 100 : inv.health + 25;
			} else if (inv.cur >= 0 && weaps[inv.cur].ammo != A_NONE) {
				const WeapRec *w = &weaps[inv.cur];
				if (!give_ammo(w->ammo, w->clip > 0 ? w->clip : 1))
					return 0;
			} else {
				return 0;
			}
		} else {
			if (inv.health >= 100)
				return 0;
			if (!strcmp(cls, "item_healthkit"))
				hist_add(HIST_ITEM, 1, '+', 0, 0); /* item_healthkit (a vial has no icon on PC) */
			inv.health += r->amount;
			if (inv.health > 100)
				inv.health = 100;
		}
		break;
	case K_SUIT:
		if (!inv.suit || inv.armor >= 100)
			return 0;
		hist_add(HIST_ITEM, 0, '*', 0, 0); /* hud_textures.txt item_battery */
		inv.armor += r->amount;
		if (inv.armor > 100)
			inv.armor = 100;
		{
			/* CHL2_Player::ApplyBattery: "power level is N percent", in 5% steps */
			int pct = (int)(inv.armor * 100.0f / 100.0f + 0.5f) / 5;
			if (pct > 0)
				pct--;
			suit_say(SUIT_P0 + (pct > 19 ? 19 : pct), 30);
		}
		break;
	case K_HEV:
		inv.suit = 1;
		/* CItemSuit: the logon, straight away */
		noRepeatUntil[SUIT_LOGON] = 0;
		suitNext = 0;
		suit_say(SUIT_LOGON, 0);
		break;
	case K_AMMO:
		if (!give_ammo(r->arg, r->amount))
			return 0;
		break;
	case K_WEAPON:
		if (!give_weapon(r->arg))
			return 0;
		break;
	default:
		return 0;
	}
	play(r->snd >= 0 ? r->snd : xsnd[X_PICKUP_WEAPON], NULL, 1.0f, 0);
	return 1;
}

/* ------------------------------------------------------------------ level */

static void sample_tint(const f32 pos[3], u8 out[4])
{
	/* the nearest baked light sample: static props and entities carry one from the converter */
	f32 best = 700.0f * 700.0f;
	out[0] = out[1] = out[2] = 128;
	out[3] = 255;
	if (!map)
		return;
	for (u32 i = 0; i < map->numStaticProps; i++) {
		const PackStaticProp *sp = &map->staticProps[i];
		f32 d[3] = {sp->origin[0] - pos[0], sp->origin[1] - pos[1], sp->origin[2] - pos[2]};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] * 4.0f;
		if (dd < best) {
			best = dd;
			memcpy(out, sp->tint, 3);
		}
	}
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		const Entity *e = &ents[i];
		if ((e->mdl < 0 && e->npcModel < 0) || (e->flags & EF_KILLED))
			continue;
		f32 d[3] = {e->origin[0] - pos[0], e->origin[1] - pos[1], e->origin[2] - pos[2]};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] * 4.0f;
		if (dd < best) {
			best = dd;
			memcpy(out, e->tint, 3);
		}
	}
}

static void place_item(WorldItem *it)
{
	/* items rest on whatever is under them */
	f32 a[3] = {it->pos[0], it->pos[1], it->pos[2] + 8};
	f32 b[3] = {it->pos[0], it->pos[1], it->pos[2] - 512};
	f32 mn[3] = {-4, -4, 0}, mx[3] = {4, 4, 4};
	Trace tr;
	CM_BoxTrace(&tr, a, b, mn, mx, MASK_PLAYERSOLID);
	if (!tr.startsolid && tr.fraction < 1)
		memcpy(it->pos, tr.endpos, 12);
	f32 tp[3] = {it->pos[0], it->pos[1], it->pos[2] + 16};
	sample_tint(tp, it->tint);
	it->placed = 1;
}

static WorldItem *add_item(s32 ent, s32 item, const f32 pos[3], f32 yaw)
{
	if (numWItems >= MAX_ITEMS || item < 0)
		return NULL;
	WorldItem *it = &witems[numWItems++];
	memset(it, 0, sizeof(*it));
	it->ent = ent;
	if (ent > 0 && (u32)ent < itemOfEntN)
		itemOfEnt[ent] = 1;
	it->item = (s16)item;
	it->alive = 1;
	memcpy(it->pos, pos, 12);
	f32 ang[3] = {0, yaw, 0};
	Ent_AngleMatrix(ang, it->R);
	return it;
}

void Weap_LevelStart(const Pack *m, Player *pl)
{
	Weap_LevelEnd();
	map = m;
	player = pl;
	stepTime = 0;
	levelStartInv = inv;
	witems = calloc(MAX_ITEMS, sizeof(WorldItem));
	numWItems = 0;
	free(itemOfEnt);
	{
		u32 ne;
		Ent_List(&ne);
		itemOfEnt = calloc(ne ? ne : 1, 1);
		itemOfEntN = itemOfEnt ? ne : 0;
	}
	itemsClock = itemsScan = 0;
	memset(projs, 0, sizeof(projs));
	memset(fxs, 0, sizeof(fxs));
	Weap_ClearImpacts();
	memset(hist, 0, sizeof(hist));
	poisonLeft = 0;
	numChargers = 0;
	tintAt = 0;
	if (!witems || !loaded)
		return;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		const Entity *e = &ents[i];
		if (strncmp(e->classname, "item_", 5) && strncmp(e->classname, "weapon_", 7))
			continue;
		s32 k = item_named(e->classname);
		if (k < 0)
			continue;
		WorldItem *it = add_item((s32)i, k, e->origin, e->angles[1]);
		if (it)
			Ent_AngleMatrix(e->angles, it->R);
	}
	/* the gun comes back out after a level change */
	if (inv.cur >= 0 && inv.have[inv.cur]) {
		s32 w = inv.cur;
		inv.cur = -1;
		switchTo = -1;
		select_weapon(w);
	}
	prevOnGround = 1;
	prevVz = 0;
}

void Weap_Respawn(void)
{
	inv = levelStartInv;
	if (inv.health <= 0)
		inv.health = 100;
	deadAt = 0;
}

void Weap_LevelEnd(void)
{
	suitQueued = 0;
	sayingSent = -1; /* sounds stop at a level change */
	memset(dmgDir, 0, sizeof(dmgDir));
	geigerRange = 1000;
	free(witems);
	witems = NULL;
	numWItems = 0;
	map = NULL;
	switchTo = -1;
	reloadDone = 0;
}

u32 Weap_PlaySound(int which, const f32 *pos, f32 vol)
{
	s16 snd = (which >= 0 && which < NUM_X) ? xsnd[which] : -1;
	if (snd < 0 || (u32)snd >= wp.numSounds)
		return 0;
	return Audio_Play(&wp.sounds[snd], pos, vol, pos ? SNDLVL_DEFAULT : 0);
}

void Weap_BreakSound(int material, const f32 at[3])
{
	static const int snd[4] = {X_BREAK_WOOD, X_BREAK_GLASS, X_BREAK_METAL, X_BREAK_CONCRETE};
	if (material <= -1 && material >= -4)
		play(xsnd[snd[-material - 1]], at, 1, 1);
}

void Weap_Broke(s32 ent, const f32 at[3])
{
	const char *cls = Ent_KV(ent, "classname");
	if (!cls || strcmp(cls, "item_item_crate"))
		return;
	const char *ic = Ent_KV(ent, "itemclass");
	const char *cnt = Ent_KV(ent, "itemcount");
	s32 k = ic ? item_named(ic) : -1;
	int c = cnt ? atoi(cnt) : 1;
	if (k < 0 && ic && !strcmp(ic, "item_dynamic_resupply"))
		k = item_named("item_healthvial");
	if (k < 0)
		return;
	if (c > 8)
		c = 8;
	for (int i = 0; i < c; i++) {
		f32 p[3] = {at[0] + (frand() - 0.5f) * 16, at[1] + (frand() - 0.5f) * 16, at[2]};
		WorldItem *it = add_item(-1, k, p, frand() * 360);
		if (it)
			place_item(it);
	}
}

/* an item in the air: gravity (sv_gravity 600), it bounces a little off what it hits and comes to rest */
static void item_fall(WorldItem *it, f32 dt)
{
	it->vel[2] -= 600.0f * dt;
	f32 to[3] = {it->pos[0] + it->vel[0] * dt, it->pos[1] + it->vel[1] * dt, it->pos[2] + it->vel[2] * dt};
	f32 mn[3] = {-4, -4, 0}, mx[3] = {4, 4, 4};
	Trace tr;
	CM_BoxTrace(&tr, it->pos, to, mn, mx, MASK_PLAYERSOLID);
	if (tr.startsolid) {
		it->falling = 0;
		return;
	}
	memcpy(it->pos, tr.endpos, 12);
	if (tr.fraction < 1) {
		f32 vn = it->vel[0] * tr.normal[0] + it->vel[1] * tr.normal[1] + it->vel[2] * tr.normal[2];
		for (int k = 0; k < 3; k++)
			it->vel[k] = (it->vel[k] - tr.normal[k] * vn * 1.3f) * 0.6f;
		if (tr.normal[2] > 0.7f && fabsf(it->vel[2]) < 40.0f) {
			it->falling = 0;
			memset(it->vel, 0, 12);
			f32 tp[3] = {it->pos[0], it->pos[1], it->pos[2] + 16};
			sample_tint(tp, it->tint);
		}
	}
}

void Weap_ItemImpulse(s32 ent, const f32 vel[3])
{
	for (u32 i = 0; i < numWItems; i++)
		if (witems[i].alive && witems[i].ent == ent) {
			for (int k = 0; k < 3; k++)
				witems[i].vel[k] += vel[k];
			witems[i].falling = 1;
			witems[i].placed = 1;
		}
}

int Weap_DropItem(const char *cls, const f32 pos[3], const f32 vel[3])
{
	s32 k = item_named(cls);
	if (k < 0)
		return 0;
	WorldItem *it = add_item(-1, k, pos, frand() * 360);
	if (!it)
		return 0;
	it->placed = 1;
	it->falling = 1;
	if (vel)
		memcpy(it->vel, vel, 12);
	f32 tp[3] = {pos[0], pos[1], pos[2] + 16};
	sample_tint(tp, it->tint);
	return 1;
}

static void items_tick(void)
{
	if (!witems || !player || deadAt > 0)
		return;
	u32 n;
	Entity *ents = Ent_List(&n);
	f32 dt = wclock - itemsClock;
	itemsClock = wclock;
	if (dt < 0 || dt > 0.1f)
		dt = 0.015f;
	/* items the map makes as it goes (point_template / env_entity_maker copies: the crowbar Barney throws) */
	if (wclock >= itemsScan && itemOfEnt) {
		itemsScan = wclock + 0.2f;
		for (u32 i = 1; i < n && i < itemOfEntN; i++) {
			const Entity *e = &ents[i];
			if (itemOfEnt[i] || (e->flags & (EF_KILLED | EF_DISABLED)) ||
			    (strncmp(e->classname, "item_", 5) && strncmp(e->classname, "weapon_", 7)))
				continue;
			s32 k = item_named(e->classname);
			if (k < 0)
				continue;
			WorldItem *it = add_item((s32)i, k, e->origin, e->angles[1]);
			if (it) {
				Ent_AngleMatrix(e->angles, it->R);
				it->placed = 1;
				it->falling = 1; /* (it drops from where it appeared, unless something throws it) */
				f32 tp[3] = {it->pos[0], it->pos[1], it->pos[2] + 16};
				sample_tint(tp, it->tint);
			}
		}
	}
	f32 mn[3], mx[3];
	Player_Bounds(player, mn, mx);
	for (u32 i = 0; i < numWItems; i++) {
		WorldItem *it = &witems[i];
		if (!it->alive)
			continue;
		if (it->falling)
			item_fall(it, dt);
		if (it->ent >= 0) {
			const Entity *e = &ents[it->ent];
			if (e->flags & EF_KILLED) {
				it->alive = 0;
				if ((u32)it->ent < itemOfEntN)
					itemOfEnt[it->ent] = 0; /* (its slot may be reused by a copy) */
				continue;
			}
			if (e->flags & EF_DISABLED)
				continue; /* waiting in a point_template */
			if (!it->placed) {
				memcpy(it->pos, e->origin, 12);
				place_item(it);
			}
		}
		f32 lo[3] = {it->pos[0] - 16, it->pos[1] - 16, it->pos[2] - 4};
		f32 hi[3] = {it->pos[0] + 16, it->pos[1] + 16, it->pos[2] + 52}; /* reach onto tables */
		int touch = 1;
		for (int k = 0; k < 3; k++)
			if (player->origin[k] + mx[k] < lo[k] || player->origin[k] + mn[k] > hi[k])
				touch = 0;
		if (!touch || !take_item(it))
			continue;
		it->alive = 0;
		if (it->ent >= 0) {
			Ent_FireOutput(it->ent, "onplayertouch", 0);
			Ent_FireOutput(it->ent, "onplayerpickup", 0);
			Ent_FireOutput(it->ent, "oncacheinteraction", 0);
			ents[it->ent].flags |= EF_KILLED;
			if ((u32)it->ent < itemOfEntN)
				itemOfEnt[it->ent] = 0;
		}
	}
}

/* item_healthcharger / item_suitcharger / item_ammo_crate on the wall */
static Charger *charger_of(s32 ent, f32 full)
{
	for (u32 i = 0; i < numChargers; i++)
		if (chargers[i].ent == ent)
			return &chargers[i];
	if (numChargers >= MAX_CHARGERS)
		return NULL;
	chargers[numChargers].ent = ent;
	chargers[numChargers].juice = full;
	return &chargers[numChargers++];
}

static void use_tick(f32 dt)
{
	if (!(inHeld & WB_USE)) {
		chargeAcc = 0;
		return;
	}
	s32 t = Ent_UseTarget(eye, fwd, 80.0f);
	if (t <= 0)
		return;
	const char *cls = Ent_KV(t, "classname");
	if (!cls)
		return;
	if (!strcmp(cls, "momentary_rot_button")) {
		Ent_UseHeld(t, 0); /* (a valve wheel turns while use is held: FCAP_CONTINUOUS_USE) */
		return;
	}
	if (!strcmp(cls, "item_ammo_crate")) {
		if (!(inDown & WB_USE))
			return;
		static const u8 crateAmmo[10] = {A_PISTOL, A_SMG1, A_AR2, A_RPG, A_BUCKSHOT, A_GRENADE, A_357, A_BOLT,
		                                 A_AR2_ALT, A_SMG1_GRENADE};
		const char *at = Ent_KV(t, "ammotype");
		int k = at ? atoi(at) : 0;
		if (k < 0 || k > 9)
			k = 0;
		int a = crateAmmo[k];
		if (give_ammo(a, ammoMax[a]))
			play(items[0].snd, NULL, 1, 0);
		if (a == A_GRENADE && !inv.have[weapon_named("weapon_frag")])
			give_weapon(weapon_named("weapon_frag"));
		return;
	}
	int health = !strcmp(cls, "item_healthcharger");
	int suit = !strcmp(cls, "item_suitcharger");
	if (!health && !suit)
		return;
	/* item_suitcharger: 75 (sk_suitcharger), Kleiner's (SF_KLEINER_RECHARGER 0x4000) 25 */
	const char *sfs = Ent_KV(t, "spawnflags");
	u32 sf = sfs ? (u32)atoi(sfs) : 0;
	Charger *c = charger_of(t, health ? 50.0f : (sf & 0x4000) ? 25.0f : 75.0f);
	if (!c)
		return;
	f32 *v = health ? &inv.health : &inv.armor;
	if (c->juice <= 0 || *v >= 100 || (suit && !inv.suit)) {
		if (inDown & WB_USE)
			play(xsnd[X_DENY], NULL, 1, 0);
		return;
	}
	chargeAcc += dt;
	while (chargeAcc >= 0.1f && c->juice > 0 && *v < 100) {
		chargeAcc -= 0.1f;
		*v += 1;
		c->juice -= 1;
	}
	if (*v > 100)
		*v = 100; /* (never past full) */
	*v = floorf(*v + 0.5f) > 100 ? 100 : *v;
	if (c->juice <= 0)
		Ent_FireOutput(t, "onempty", 0);
}

/* ------------------------------------------------------------------ firing */

static void aim_dir(f32 spread, f32 out[3])
{
	/* a random point in the spread cone around the view */
	f32 ang[3] = {viewPitch, viewYaw, 0};
	f32 R[3][3];
	Ent_AngleMatrix(ang, R);
	f32 x = (frand() + frand() - 1) * spread, y = (frand() + frand() - 1) * spread;
	for (int r = 0; r < 3; r++)
		out[r] = R[r][0] + R[r][1] * x + R[r][2] * y;
	f32 l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	for (int r = 0; r < 3; r++)
		out[r] /= l;
}

static void trace_line(Trace *tr, const f32 a[3], const f32 b[3], f32 size)
{
	f32 mn[3] = {-size, -size, -size}, mx[3] = {size, size, size};
	CM_BoxTrace(tr, a, b, mn, mx, MASK_SHOT);
}

/* NPC hitboxes: every live NPC's body box, including the small ones the player walks through
 * (birds, scanners, headcrabs). Returns the entity hit nearer than *frac along a->b, or -1. */
static s32 trace_npcs(const f32 a[3], const f32 b[3], f32 *frac)
{
	u32 nn, ne;
	Npc *list = Npc_List(&nn);
	Entity *ents = Ent_List(&ne);
	s32 hit = -1;
	f32 d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
	for (u32 i = 0; i < nn; i++) {
		const Npc *n = &list[i];
		const Entity *e = &ents[n->ent];
		if (n->dead || (e->flags & (EF_KILLED | EF_DISABLED)) || !map || n->model >= map->numNpcModels)
			continue;
		const PackNpcModel *m = &map->npcModels[n->model];
		f32 r = m->radius < 6 ? 6 : m->radius > 24 ? 24 : m->radius;
		f32 h = m->height < 8 ? 8 : m->height > 96 ? 96 : m->height;
		f32 lo[3] = {e->origin[0] - r, e->origin[1] - r, e->origin[2]};
		f32 hi[3] = {e->origin[0] + r, e->origin[1] + r, e->origin[2] + h};
		f32 t0 = 0, t1 = *frac;
		int ok = 1;
		for (int k = 0; k < 3 && ok; k++) {
			if (fabsf(d[k]) < 1e-6f) {
				if (a[k] < lo[k] || a[k] > hi[k])
					ok = 0;
				continue;
			}
			f32 u = (lo[k] - a[k]) / d[k], v = (hi[k] - a[k]) / d[k];
			if (u > v) {
				f32 s = u;
				u = v;
				v = s;
			}
			if (u > t0)
				t0 = u;
			if (v < t1)
				t1 = v;
			if (t0 > t1)
				ok = 0;
		}
		if (ok && t0 < *frac) {
			*frac = t0;
			hit = n->ent;
		}
	}
	return hit;
}

static void shoot_bullet(const f32 dir[3], f32 damage);
/* a vehicle's gun: a bullet from `from` along dir (the jeep's tau cannon, the airboat's gun) */
void Weap_VehicleShot(const f32 from[3], const f32 dir[3], f32 damage)
{
	f32 save[3];
	memcpy(save, eye, 12);
	memcpy(eye, from, 12);
	shoot_bullet(dir, damage);
	memcpy(eye, save, 12);
}

static void shoot_bullet(const f32 dir[3], f32 damage)
{
	/* the gunshot is a combat sound (CSoundEnt SOUND_COMBAT, SOUNDENT_VOLUME_MACHINEGUN 1500): NPCs that
	 * hear it look around */
	Npc_HearCombat(eye, 1500.0f);
	f32 end[3] = {eye[0] + dir[0] * 8192, eye[1] + dir[1] * 8192, eye[2] + dir[2] * 8192};
	Trace tr;
	trace_line(&tr, eye, end, 0);
	f32 f = tr.fraction;
	s32 npc = trace_npcs(eye, end, &f);
	if (npc > 0) {
		tr.fraction = f;
		tr.ent = npc;
		for (int k = 0; k < 3; k++)
			tr.endpos[k] = eye[k] + (end[k] - eye[k]) * f;
	}
	if (tr.fraction >= 1 || (tr.ent <= 0 && tr.surface == SURF_SKY)) /* (shots into the sky just vanish) */
		return;
	if (tr.ent > 0)
		Ent_Damage(tr.ent, damage, dir, tr.endpos);
	f32 p[3] = {tr.endpos[0] - dir[0] * 2, tr.endpos[1] - dir[1] * 2, tr.endpos[2] - dir[2] * 2};
	Weap_BulletImpact(&tr, dir);
	/* the hit surface's bullet impact sound (concrete, metal, flesh...) */
	Weap_SurfaceSound(tr.ent > 0 ? Weap_EntSurface(tr.ent) : tr.surface, 2, p, 0.8f);
}

static void launch(int type, f32 speed, f32 up)
{
	for (int i = 0; i < MAX_PROJ; i++)
		if (!projs[i].alive) {
			Proj *p = &projs[i];
			memset(p, 0, sizeof(*p));
			p->alive = 1;
			p->type = (u8)type;
			f32 ang[3] = {viewPitch, viewYaw, 0};
			f32 R[3][3];
			Ent_AngleMatrix(ang, R);
			for (int k = 0; k < 3; k++) {
				/* from just in front of the eye, a little right and below like the PC's */
				p->pos[k] = eye[k] + R[k][0] * 16 - R[k][1] * 6 - R[k][2] * 6;
				p->vel[k] = fwd[k] * speed + R[k][2] * up;
			}
			if (player)
				for (int k = 0; k < 2; k++)
					p->vel[k] += player->velocity[k];
			p->fuse = type == P_FRAG ? 3.0f : type == P_BALL ? 4.0f : 10.0f;
			return;
		}
}

static int has_clip(const WeapRec *w) { return w->clip > 0; }

static int consume(s32 wi, int n)
{
	const WeapRec *w = &weaps[wi];
	if (gCheats.infiniteAmmo)
		return 1;
	if (has_clip(w)) {
		if (inv.clip[wi] < n)
			return 0;
		inv.clip[wi] -= (s16)n;
		return 1;
	}
	if (w->ammo == A_NONE)
		return 1;
	if (inv.ammo[w->ammo] < n)
		return 0;
	inv.ammo[w->ammo] -= (s16)n;
	return 1;
}

static void start_reload(void)
{
	s32 wi = inv.cur;
	if (wi < 0 || reloadDone > 0)
		return;
	const WeapRec *w = &weaps[wi];
	if (!has_clip(w) || inv.clip[wi] >= w->clip || inv.ammo[w->ammo] <= 0)
		return;
	f32 d = vm_play(SQ_RELOAD);
	if (w->flags & F_SHOTGUN) {
		/* shells go in one at a time */
		int need = w->clip - inv.clip[wi];
		if (need > inv.ammo[w->ammo])
			need = inv.ammo[w->ammo];
		d = 0.5f * (f32)need + 0.3f;
		vhold = 0;
		seqEnd = wclock + d;
	}
	play(w->snd[SN_RELOAD], NULL, 1, 0);
	reloadDone = wclock + d;
	nextAttack = nextAlt = reloadDone;
}

static void finish_reload(void)
{
	s32 wi = inv.cur;
	reloadDone = 0;
	if (wi < 0)
		return;
	const WeapRec *w = &weaps[wi];
	int need = w->clip - inv.clip[wi];
	if (need > inv.ammo[w->ammo])
		need = inv.ammo[w->ammo];
	if (need > 0) {
		inv.clip[wi] += (s16)need;
		inv.ammo[w->ammo] -= (s16)need;
	}
}

static void melee(const WeapRec *w)
{
	f32 end[3] = {eye[0] + fwd[0] * 75, eye[1] + fwd[1] * 75, eye[2] + fwd[2] * 75};
	Trace tr;
	trace_line(&tr, eye, end, 0);
	if (tr.fraction >= 1)
		trace_line(&tr, eye, end, 8); /* a near miss still connects, like Source's hull trace */
	{
		f32 f = tr.fraction;
		s32 npc = trace_npcs(eye, end, &f);
		if (npc > 0) {
			tr.fraction = f;
			tr.ent = npc;
			for (int k = 0; k < 3; k++)
				tr.endpos[k] = eye[k] + (end[k] - eye[k]) * f;
		}
	}
	if (tr.fraction < 1) {
		vm_play(SQ_PRIMARY);
		if (tr.ent > 0)
			Ent_Damage(tr.ent, w->damage, fwd, tr.endpos);
		play(tr.ent > 0 && w->snd[SN_HIT] >= 0 ? w->snd[SN_HIT] : xsnd[X_CROWBAR_WORLD], NULL, 1, 0);
		Weap_Effect(WFX_SPARK, tr.endpos);
		nextAttack = wclock + w->rate;
	} else {
		vm_play(SQ_MISS);
		play(w->snd[SN_SHOT] >= 0 ? w->snd[SN_SHOT] : xsnd[X_CROWBAR_SWING], NULL, 1, 0);
		nextAttack = wclock + w->rate + 0.1f;
	}
}

#define PHYSCANNON_MAX_MASS 250.0f

static void physcannon_primary(void)
{
	nextAttack = wclock + 0.5f;
	if (Phys_HeldEnt() >= 0) {
		Phys_Launch(inv.supercharged ? 60000.0f : 30000.0f);
		play(xsnd[X_PC_LAUNCH], NULL, 1, 0);
		vm_play(SQ_PRIMARY);
		return;
	}
	f32 reach = inv.supercharged ? 1024.0f : 250.0f;
	f32 end[3] = {eye[0] + fwd[0] * reach, eye[1] + fwd[1] * reach, eye[2] + fwd[2] * reach};
	Trace tr;
	trace_line(&tr, eye, end, 4);
	if (tr.fraction < 1 && tr.ent > 0) {
		if (Phys_IsBody(tr.ent) && (Phys_Mass(tr.ent) <= PHYSCANNON_MAX_MASS || inv.supercharged)) {
			f32 imp[3] = {fwd[0] * 30000.0f, fwd[1] * 30000.0f, fwd[2] * 30000.0f};
			Phys_Impulse(tr.ent, tr.endpos, imp);
			play(xsnd[X_PC_LAUNCH], NULL, 1, 0);
			vm_play(SQ_PRIMARY);
			return;
		}
		/* the supercharged gun throws people too; the normal one just shoves */
		if (inv.supercharged) {
			Ent_Damage(tr.ent, 1000, fwd, tr.endpos);
			play(xsnd[X_PC_LAUNCH], NULL, 1, 0);
			vm_play(SQ_PRIMARY);
			return;
		}
		Ent_Damage(tr.ent, 1, fwd, tr.endpos);
	}
	play(xsnd[X_PC_DRY], NULL, 1, 0);
	vm_play(SQ_DRY);
}

static void physcannon_secondary(int pressed)
{
	if (Phys_HeldEnt() >= 0) {
		if (pressed) {
			Phys_Drop(0);
			play(xsnd[X_PC_DROP], NULL, 1, 0);
			nextAlt = wclock + 0.4f;
		}
		return;
	}
	f32 reach = inv.supercharged ? 2048.0f : 1024.0f;
	f32 end[3] = {eye[0] + fwd[0] * reach, eye[1] + fwd[1] * reach, eye[2] + fwd[2] * reach};
	Trace tr;
	trace_line(&tr, eye, end, 6);
	if (tr.fraction >= 1 || tr.ent <= 0 || !Phys_IsBody(tr.ent))
		return;
	f32 maxMass = inv.supercharged ? 1500.0f : PHYSCANNON_MAX_MASS;
	if (Phys_Mass(tr.ent) > maxMass) {
		if (!pullWarned)
			play(xsnd[X_PC_HEAVY], NULL, 1, 0);
		pullWarned = 1;
		return;
	}
	f32 dist = tr.fraction * reach;
	if (dist < 120.0f) {
		if (Phys_GrabFar(tr.ent, maxMass)) {
			play(xsnd[X_PC_PICKUP], NULL, 1, 0);
			vm_play(SQ_SECONDARY);
		}
	} else {
		Phys_Pull(tr.ent, eye, Phys_Mass(tr.ent) * 1.5f + 20.0f);
	}
}

static void fire_primary(int pressed)
{
	s32 wi = inv.cur;
	const WeapRec *w = &weaps[wi];
	if (wclock < nextAttack || reloadDone > 0)
		return;
	if (!(w->flags & (F_AUTO | F_MELEE | F_PHYS)) && !pressed)
		return; /* semi-automatic */
	if (w->flags & F_PHYS) {
		if (pressed || Phys_HeldEnt() >= 0)
			physcannon_primary();
		return;
	}
	if (w->flags & F_MELEE) {
		melee(w);
		return;
	}
	if (!consume(wi, 1)) {
		play(w->snd[SN_EMPTY], NULL, 1, 0);
		vm_play(SQ_DRY);
		nextAttack = wclock + 0.3f;
		if (has_clip(w) && inv.ammo[w->ammo] > 0)
			start_reload();
		else if (!has_clip(w) && inv.ammo[w->ammo] <= 0)
			cycle_weapon(1);
		return;
	}
	const char *cls = wstr(w->name);
	nextAttack = wclock + w->rate;
	if (w->flags & F_THROW) {
		vm_play(SQ_MISS);
		launch(!strcmp(cls, "weapon_bugbait") ? P_BUGBAIT : P_FRAG, 1200.0f, 50.0f);
		play(w->snd[SN_SHOT], NULL, 1, 0);
		if (!gCheats.infiniteAmmo && w->ammo != A_NONE && inv.ammo[w->ammo] <= 0)
			cycle_weapon(1);
		return;
	}
	if (w->flags & F_ROCKET) {
		vm_play(SQ_PRIMARY);
		launch(P_ROCKET, 1500.0f, 0);
		play(w->snd[SN_SHOT] >= 0 ? w->snd[SN_SHOT] : xsnd[X_ROCKET], NULL, 1, 0);
		return;
	}
	vm_play(SQ_PRIMARY);
	play(w->snd[SN_SHOT], NULL, 1, 0);
	muzzleUntil = wclock + 0.05f;
	if (!strcmp(cls, "weapon_crossbow")) {
		launch(P_BOLT, 3500.0f, 0);
		return;
	}
	for (int k = 0; k < w->pellets; k++) {
		f32 d[3];
		aim_dir(w->spread, d);
		shoot_bullet(d, w->damage);
	}
}

static void fire_secondary(int pressed)
{
	s32 wi = inv.cur;
	const WeapRec *w = &weaps[wi];
	const char *cls = wstr(w->name);
	if (w->flags & F_PHYS) {
		if (wclock >= nextAlt)
			physcannon_secondary(pressed);
		return;
	}
	if (!pressed || wclock < nextAlt || wclock < nextAttack || reloadDone > 0)
		return;
	if (!strcmp(cls, "weapon_smg1") || !strcmp(cls, "weapon_ar2")) {
		int a = !strcmp(cls, "weapon_smg1") ? A_SMG1_GRENADE : A_AR2_ALT;
		if (inv.ammo[a] <= 0 && !gCheats.infiniteAmmo) {
			play(w->snd[SN_EMPTY], NULL, 1, 0);
			nextAlt = wclock + 0.5f;
			return;
		}
		if (!gCheats.infiniteAmmo)
			inv.ammo[a]--;
		vm_play(SQ_SECONDARY);
		launch(a == A_SMG1_GRENADE ? P_SMGNADE : P_BALL, a == A_SMG1_GRENADE ? 1000.0f : 1000.0f, 0);
		play(w->snd[SN_SHOT], NULL, 1, 0);
		nextAlt = nextAttack = wclock + 1.0f;
	} else if (w->flags & F_SHOTGUN) {
		if (inv.clip[wi] < 2 && !gCheats.infiniteAmmo) {
			fire_primary(1);
			return;
		}
		consume(wi, 2);
		vm_play(SQ_SECONDARY);
		play(w->snd[SN_SHOT], NULL, 1, 0);
		for (int k = 0; k < 12; k++) {
			f32 d[3];
			aim_dir(w->spread * 1.3f, d);
			shoot_bullet(d, w->damage);
		}
		nextAlt = nextAttack = wclock + 1.1f;
	} else if (w->flags & F_THROW) {
		if (!consume(wi, 1))
			return;
		vm_play(SQ_SECONDARY);
		launch(!strcmp(cls, "weapon_bugbait") ? P_BUGBAIT : P_FRAG, 350.0f, 150.0f); /* lob */
		nextAlt = nextAttack = wclock + w->rate;
	}
}

/* ------------------------------------------------------------------ projectiles */

static void proj_tick(Proj *p, f32 dt)
{
	p->t += dt;
	if (p->type == P_FRAG || p->type == P_SMGNADE || p->type == P_BUGBAIT)
		p->vel[2] -= 600.0f * dt;
	else if (p->type == P_BOLT)
		p->vel[2] -= 40.0f * dt;
	else if (p->type == P_ROCKET && inv.cur >= 0 && !strcmp(wstr(weaps[inv.cur].name), "weapon_rpg")) {
		/* the laser guides it: steer towards where the player is pointing */
		f32 end[3] = {eye[0] + fwd[0] * 8192, eye[1] + fwd[1] * 8192, eye[2] + fwd[2] * 8192};
		Trace tr;
		trace_line(&tr, eye, end, 0);
		f32 d[3] = {tr.endpos[0] - p->pos[0], tr.endpos[1] - p->pos[1], tr.endpos[2] - p->pos[2]};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		f32 sp = sqrtf(p->vel[0] * p->vel[0] + p->vel[1] * p->vel[1] + p->vel[2] * p->vel[2]);
		if (l > 1 && sp > 1)
			for (int k = 0; k < 3; k++) {
				f32 want = d[k] / l * sp;
				p->vel[k] += (want - p->vel[k]) * (dt * 4 > 1 ? 1 : dt * 4);
			}
	}
	f32 to[3] = {p->pos[0] + p->vel[0] * dt, p->pos[1] + p->vel[1] * dt, p->pos[2] + p->vel[2] * dt};
	Trace tr;
	trace_line(&tr, p->pos, to, p->type == P_BALL ? 6 : 2);
	{
		f32 f = tr.fraction;
		s32 npc = trace_npcs(p->pos, to, &f);
		if (npc > 0) {
			tr.fraction = f;
			tr.ent = npc;
			for (int k = 0; k < 3; k++) {
				tr.endpos[k] = p->pos[k] + (to[k] - p->pos[k]) * f;
				tr.normal[k] = p->vel[k] ? -p->vel[k] / (fabsf(p->vel[0]) + fabsf(p->vel[1]) + fabsf(p->vel[2])) : 0;
			}
		}
	}
	if (tr.fraction < 1) {
		switch (p->type) {
		case P_SMGNADE:
		case P_ROCKET:
			explode(tr.endpos, p->type == P_ROCKET ? 150.0f : 100.0f, 250.0f);
			p->alive = 0;
			return;
		case P_BOLT:
			if (tr.ent > 0) {
				f32 d[3] = {p->vel[0] / 3500, p->vel[1] / 3500, p->vel[2] / 3500};
				Ent_Damage(tr.ent, 100.0f, d, tr.endpos);
			}
			Weap_Effect(WFX_SPARK, tr.endpos);
			p->alive = 0;
			return;
		case P_BALL:
			if (tr.ent > 0) {
				u32 n;
				Entity *ents = Ent_List(&n);
				f32 d[3] = {p->vel[0] / 1000, p->vel[1] / 1000, p->vel[2] / 1000};
				if (ents[tr.ent].cls == EC_NPC || Phys_IsBody(tr.ent))
					Ent_Damage(tr.ent, 1000.0f, d, tr.endpos);
			}
			/* fallthrough: bounce */
		default: {
			/* bounce off */
			f32 dn = p->vel[0] * tr.normal[0] + p->vel[1] * tr.normal[1] + p->vel[2] * tr.normal[2];
			f32 keep = p->type == P_BALL ? 1.0f : 0.45f;
			for (int k = 0; k < 3; k++)
				p->vel[k] = (p->vel[k] - 2 * dn * tr.normal[k]) * keep;
			memcpy(p->pos, tr.endpos, 12);
			for (int k = 0; k < 3; k++)
				p->pos[k] += tr.normal[k] * 0.5f;
			if (p->type == P_BUGBAIT) {
				Weap_Effect(WFX_IMPACT, p->pos);
				p->alive = 0;
			}
			return;
		}
		}
	}
	memcpy(p->pos, to, 12);
	p->spin += dt * 720;
	if (p->t >= p->fuse) {
		if (p->type == P_FRAG)
			explode(p->pos, 150.0f, 250.0f);
		else if (p->type == P_BALL)
			Weap_Effect(WFX_EXPLOSION, p->pos);
		p->alive = 0;
	}
}

/* ------------------------------------------------------------------ per frame / per tick */

void Weap_Input(u32 held, u32 down, const f32 e[3], f32 pitch, f32 yaw, int frozen)
{
	inHeld = held;
	inDown |= down;
	memcpy(eye, e, 12);
	viewPitch = pitch;
	viewYaw = yaw;
	f32 sp = sinf(pitch * PI_F / 180), cp = cosf(pitch * PI_F / 180);
	fwd[0] = cp * cosf(yaw * PI_F / 180);
	fwd[1] = cp * sinf(yaw * PI_F / 180);
	fwd[2] = -sp;
	inFrozen = frozen;
}

f32 Weap_PlayerHealth(void) { return inv.health; }

void Weap_Tick(f32 dt)
{
	if (poisonLeft > 0 && wclock >= poisonNext && deadAt == 0) {
		f32 h = poisonLeft > 10 ? 10 : poisonLeft;
		inv.health = inv.health + h > 100 ? 100 : inv.health + h;
		poisonLeft -= h;
		poisonNext = wclock + 1.0f;
	}
	wclock += dt;
	suit_tick(dt); /* the suit works with or without the weapons pack */
	suit_voice_tick(dt);
	geiger_tick(dt);
	footsteps(dt);
	for (int i = 0; i < 4; i++)
		if (dmgDir[i] > 0)
			dmgDir[i] -= dt; /* the indicator fades over a second */
	if (!loaded)
		return;
	u32 down = inDown;
	inDown = 0;
	u32 held = inFrozen || deadAt > 0 ? 0 : inHeld;
	if (inFrozen || deadAt > 0)
		down = 0;

	/* falls hurt, like Source's (safe below 526.5 units/s, fatal at 922.5) */
	if (player && !player->noclip) {
		if (player->onGround && !prevOnGround && -prevVz > 526.5f)
			Weap_HurtFrom((-prevVz - 526.5f) * (100.0f / (922.5f - 526.5f)), DMG_FALL, NULL);
		/* landing on a prop: the player's physics shadow hits it, and the prop takes impact damage by the
		 * speed (gDefaultImpactDamageTable, physics_impact_damage.cpp): a crate breaks under a jump from a
		 * height, as on the PC */
		if (player->onGround && !prevOnGround && player->groundEnt > 0 && -prevVz > 150.0f) {
			static const f32 spd[6] = {150, 250, 450, 550, 700, 1000}, dmg[6] = {5, 10, 20, 50, 100, 500};
			f32 v = -prevVz, d = 0;
			for (int i = 0; i < 6; i++)
				if (v >= spd[i])
					d = dmg[i];
			if (d > 0 && Ent_IsProp(player->groundEnt)) {
				f32 down[3] = {0, 0, -1};
				Ent_Damage(player->groundEnt, d, down, player->origin);
			}
		}
		prevOnGround = (u8)(player->onGround != 0);
		prevVz = player->velocity[2];
	} else {
		prevOnGround = 1;
	}

	items_tick();
	use_tick(dt);

	/* weapon switching */
	if (switchTo >= 0 && wclock >= switchAt) {
		inv.cur = switchTo;
		switchTo = -1;
		nextAttack = nextAlt = wclock + vm_play(SQ_DRAW) * 0.8f;
	}
	if (switchTo < 0) {
		if (down & WB_NEXT)
			cycle_weapon(1);
		else if (down & WB_PREV)
			cycle_weapon(-1);
		else if ((down & WB_LAST) && inv.last >= 0 && inv.have[inv.last]) {
			s32 l = inv.cur;
			select_weapon(inv.last);
			inv.last = l;
		}
	}
	if (reloadDone > 0 && wclock >= reloadDone)
		finish_reload();

	/* hands: what the player carries with "use" gets thrown by the fire button */
	int gravGun = inv.cur >= 0 && (weaps[inv.cur].flags & F_PHYS);
	if (Phys_Holding() && !gravGun) {
		if (down & WB_FIRE) {
			Phys_Drop(1);
			nextAttack = wclock + 0.5f;
		}
	} else if (inv.cur >= 0 && switchTo < 0) {
		if (held & WB_FIRE)
			fire_primary((down & WB_FIRE) != 0);
		if (held & WB_ALT)
			fire_secondary((down & WB_ALT) != 0);
		else
			pullWarned = 0;
		if (down & WB_RELOAD)
			start_reload();
		/* empty clip: reload on its own, like on PC */
		const WeapRec *w = &weaps[inv.cur];
		if (has_clip(w) && inv.clip[inv.cur] == 0 && inv.ammo[w->ammo] > 0 && reloadDone == 0 && wclock >= nextAttack)
			start_reload();
	}

	for (int i = 0; i < MAX_PROJ; i++)
		if (projs[i].alive)
			proj_tick(&projs[i], dt);
	for (int i = 0; i < MAX_FX; i++)
		if (fxs[i].alive && (fxs[i].t += dt) >= fxs[i].dur)
			fxs[i].alive = 0;
	tick_puffs(dt);

	/* viewmodel animation */
	if (inv.cur >= 0) {
		vt += dt;
		if (vblend < 1) {
			vblend += dt / 0.1f;
			vprevT += dt;
			if (vblend > 1)
				vblend = 1;
		}
		if (seqEnd > 0 && wclock >= seqEnd && switchTo < 0 && reloadDone == 0)
			vm_play(SQ_IDLE);
	}
	if (player) {
		f32 sp = sqrtf(player->velocity[0] * player->velocity[0] + player->velocity[1] * player->velocity[1]);
		f32 want = player->onGround ? (sp > 320 ? 1.0f : sp / 320.0f) : 0;
		bobAmt += (want - bobAmt) * (dt * 8 > 1 ? 1 : dt * 8);
		bobPhase += dt * (4.0f + sp * 0.02f);
	}
	if (gCheats.god && inv.health < 100)
		inv.health = 100;
}

#ifndef HOST_TEST
/* ------------------------------------------------------------------ drawing */

static void begin_sprites(void)
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT6, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT6, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR); /* additive */
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
}

static void end_sprites(void)
{
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

/* a soft-edged glow: centre quad plus a fainter, bigger one */
static void sprite(const Camera *cam, const f32 p[3], f32 size, u32 rgba)
{
	const guVector *r = &cam->right, *u = &cam->up;
	for (int layer = 0; layer < 2; layer++) {
		f32 s = layer ? size : size * 0.45f;
		u32 c = layer ? (rgba & 0xFFFFFF00u) | ((rgba & 0xFF) / 3) : rgba;
		GX_Begin(GX_QUADS, GX_VTXFMT6, 4);
		GX_Position3f32(p[0] - r->x * s - u->x * s, p[1] - r->y * s - u->y * s, p[2] - r->z * s - u->z * s);
		GX_Color1u32(c);
		GX_Position3f32(p[0] + r->x * s - u->x * s, p[1] + r->y * s - u->y * s, p[2] + r->z * s - u->z * s);
		GX_Color1u32(c);
		GX_Position3f32(p[0] + r->x * s + u->x * s, p[1] + r->y * s + u->y * s, p[2] + r->z * s + u->z * s);
		GX_Color1u32(c);
		GX_Position3f32(p[0] - r->x * s + u->x * s, p[1] - r->y * s + u->y * s, p[2] - r->z * s + u->z * s);
		GX_Color1u32(c);
		GX_End();
	}
}

/* ---- textured effects: the game's own sprites and decals (FXTX), drawn as quads with a texture
 * times a vertex colour. Modes: additive (muzzle flashes, sparks, explosions: "$additive"), blended
 * by the texture's alpha (smoke, flecks, blood: "$translucent"), and modulate (bullet holes: the
 * DecalModulate shader, destination x texture x 2, mid grey leaves the wall as it is). */
enum { FXM_ADD, FXM_ALPHA, FXM_MODULATE };

static void begin_fx(void)
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
}

/* the slot's texture and blending; 0 if the pack has no such texture (then nothing is drawn) */
static int fx_bind(int slot, int mode)
{
	if (slot < 0 || slot >= FX_COUNT || !Render_SkinTexture(fxTex[slot]))
		return 0;
	if (mode == FXM_ADD) {
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
	} else if (mode == FXM_ALPHA) {
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	} else {
		GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_SRCCLR, GX_LO_CLEAR);
	}
	return 1;
}

/* a quad at p spanned by the unit vectors a and b (half sizes sa, sb) */
static void fx_quad(const f32 p[3], const f32 a[3], const f32 b[3], f32 sa, f32 sb, u32 rgba)
{
	static const f32 cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	for (int k = 0; k < 4; k++) {
		GX_Position3f32(p[0] + a[0] * cs[k][0] * sa + b[0] * cs[k][1] * sb, p[1] + a[1] * cs[k][0] * sa + b[1] * cs[k][1] * sb,
		                p[2] + a[2] * cs[k][0] * sa + b[2] * cs[k][1] * sb);
		GX_Color1u32(rgba);
		GX_TexCoord2f32(cs[k][0] * 0.5f + 0.5f, 0.5f - cs[k][1] * 0.5f);
	}
	GX_End();
}

/* facing the camera, turned by roll (radians) about the view axis */
static void fx_billboard(const Camera *cam, const f32 p[3], f32 size, f32 roll, u32 rgba)
{
	f32 c = cosf(roll), sn = sinf(roll);
	f32 a[3] = {cam->right.x * c + cam->up.x * sn, cam->right.y * c + cam->up.y * sn, cam->right.z * c + cam->up.z * sn};
	f32 b[3] = {cam->up.x * c - cam->right.x * sn, cam->up.y * c - cam->right.y * sn, cam->up.z * c - cam->right.z * sn};
	fx_quad(p, a, b, size, size, rgba);
}

/* bullet holes on the walls, then the dust, flecks, sparks and blood thrown off hits */
static void draw_impacts(const Camera *cam)
{
	const f32 cx = cam->origin.x, cy = cam->origin.y, cz = cam->origin.z;
	const f32 fx = cam->forward.x, fy = cam->forward.y, fz = cam->forward.z;
	for (int i = 0; i < MAX_DECALS; i++) {
		const Decal *d = &decals[i];
		if (!d->alive)
			continue;
		f32 o[3] = {d->p[0] - cx, d->p[1] - cy, d->p[2] - cz};
		if (o[0] * fx + o[1] * fy + o[2] * fz < -8 || o[0] * o[0] + o[1] * o[1] + o[2] * o[2] > 1200.0f * 1200.0f)
			continue;
		/* (decals/wood/shot* are alpha-blended; the others are DecalModulate) */
		int wood = d->tex >= FX_SHOT_WOOD && d->tex < FX_SHOT_WOOD + 3;
		if (!fx_bind(d->tex, wood ? FXM_ALPHA : FXM_MODULATE))
			continue;
		f32 ang = (f32)d->seed * 0.0246f, c = cosf(ang), sn = sinf(ang);
		f32 a[3] = {d->u[0] * c + d->v[0] * sn, d->u[1] * c + d->v[1] * sn, d->u[2] * c + d->v[2] * sn};
		f32 b[3] = {d->v[0] * c - d->u[0] * sn, d->v[1] * c - d->u[1] * sn, d->v[2] * c - d->u[2] * sn};
		fx_quad(d->p, a, b, d->r, d->r, 0xFFFFFFFFu);
	}
	for (int i = 0; i < MAX_PUFFS; i++) {
		const Puff *q = &puffs[i];
		if (!q->alive || !fx_bind(q->tex, q->add ? FXM_ADD : FXM_ALPHA))
			continue;
		f32 k = q->t / q->dur;
		u32 al = (u32)((q->rgba & 0xFF) * (1 - k));
		fx_billboard(cam, q->pos, q->s0 + (q->s1 - q->s0) * k, (f32)q->roll * 0.0246f, (q->rgba & 0xFFFFFF00u) | al);
	}
}

static void model_pose(s16 model, s16 seq, f32 t, int hold, u32 *nb)
{
	const PackSkel *sk = &wp.skels[wp.npcModels[model].skel];
	if (seq < 0)
		seq = sk->seqIdle >= 0 ? sk->seqIdle : (sk->numSeqs ? 0 : -1);
	Anim_Pose(&wp, sk, seq, t, hold, -1, 0, 1, pose);
	*nb = sk->numBones;
}

/* ---- what NPCs hold (Source bone-merges the weapon's world model onto the character: its right-hand bone
 * on theirs). Muzzle flashes at the weapon's "muzzle" attachment while it fires. */
#define MAX_NPC_FLASH 8
#define MAX_NPCS_TINT 64
static f32 npcFlash[MAX_NPC_FLASH][3];
static int numNpcFlash;
static u8 npcTint[MAX_NPCS_TINT][4];
static f32 npcTintAt[MAX_NPCS_TINT];

static int wp_attach(s16 model, const char *name, s32 *bone, f32 local[3][4])
{
	if (!wp.npcFaces || !wp.attachments || model < 0 || (u32)model >= wp.numNpcModels)
		return 0;
	const PackNpcFace *f = &wp.npcFaces[model];
	for (u32 i = 0; i < f->numAtt; i++) {
		const PackAttachment *a = &wp.attachments[f->firstAtt + i];
		if (!strcasecmp(wstr(a->name), name)) {
			*bone = a->bone;
			memcpy(local, a->local, sizeof(a->local));
			return 1;
		}
	}
	return 0;
}

/* a = b * c for 3x4 rigid transforms */
static void m34_mul(f32 a[3][4], const f32 b[3][4], const f32 c[3][4])
{
	f32 t[3][4];
	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 4; k++)
			t[r][k] = b[r][0] * c[0][k] + b[r][1] * c[1][k] + b[r][2] * c[2][k] + (k == 3 ? b[r][3] : 0);
	memcpy(a, t, sizeof(t));
}

static void m34_inv(f32 a[3][4], const f32 b[3][4])
{
	f32 t[3][4];
	for (int r = 0; r < 3; r++) {
		for (int k = 0; k < 3; k++)
			t[r][k] = b[k][r];
		t[r][3] = -(b[0][r] * b[0][3] + b[1][r] * b[1][3] + b[2][r] * b[2][3]);
	}
	memcpy(a, t, sizeof(t));
}

static void draw_npc_weapons(Mtx view, const Camera *cam)
{
	numNpcFlash = 0;
	u32 nn;
	Npc *L = Npc_List(&nn);
	u32 ne;
	Entity *E = Ent_List(&ne);
	for (u32 i = 0; i < nn; i++) {
		Npc *n = &L[i];
		const Entity *e = &E[n->ent];
		if (n->dead || n->prop || (e->flags & (EF_DISABLED | EF_KILLED)) || !n->held[0])
			continue;
		if (n->heldItem == -2)
			n->heldItem = (s16)item_named(n->held);
		if (n->heldItem < 0)
			continue;
		s16 m = items[n->heldItem].model;
		if (m < 0 || (u32)m >= wp.numNpcModels)
			continue;
		f32 d[3] = {e->origin[0] - cam->origin.x, e->origin[1] - cam->origin.y, e->origin[2] + 40 - cam->origin.z};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd > 2500.0f * 2500.0f || d[0] * cam->forward.x + d[1] * cam->forward.y + d[2] * cam->forward.z < -64)
			continue;
		f32 hp[3], hR[3][3];
		s32 wb;
		f32 loc[3][4];
		if (!Npc_Attachment(n, "_rhand", hp, hR) || !wp_attach(m, "_rhand", &wb, loc))
			continue;
		u32 nb;
		model_pose(m, -1, 0, 1, &nb);
		if (wb < 0 || (u32)wb >= nb)
			continue;
		/* the weapon's hand bone now: skin = bone * poseToBone, so bone = skin * inverse(poseToBone) */
		const PackSkel *sk = &wp.skels[wp.npcModels[m].skel];
		const PackBone *bn = &wp.npcBones[sk->firstBone + (u32)wb];
		f32 inv[3][4], Wh[3][4], T[3][4], H[3][4];
		m34_inv(inv, bn->poseToBone);
		m34_mul(Wh, pose[wb], inv);
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++)
				H[r][c] = hR[r][c];
			H[r][3] = hp[r];
		}
		m34_inv(inv, Wh);
		m34_mul(T, H, inv); /* the weapon model's placement: its hand bone lands on theirs */
		{
			/* Source bone-merges every bone the two share: the hand's attachment bone too (the suitcases
			 * are skinned to it), so it takes the character's, not its own idle's, place */
			s32 ab;
			f32 al[3][4], ap[3], aR[3][3];
			if (wp_attach(m, "_rattach", &ab, al) && ab >= 0 && (u32)ab < nb && Npc_Attachment(n, "_rattach", ap, aR)) {
				f32 A[3][4], Ti[3][4], L[3][4];
				for (int r = 0; r < 3; r++) {
					for (int c = 0; c < 3; c++)
						A[r][c] = aR[r][c];
					A[r][3] = ap[r];
				}
				m34_inv(Ti, T);
				m34_mul(L, Ti, A); /* its frame in the weapon model's space */
				m34_mul(pose[ab], L, wp.npcBones[sk->firstBone + (u32)ab].poseToBone);
			}
		}
		f32 org[3] = {T[0][3], T[1][3], T[2][3]}, R[3][3];
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				R[r][c] = T[r][c];
		u32 slot = i % MAX_NPCS_TINT;
		if (wclock >= npcTintAt[slot]) {
			sample_tint(hp, npcTint[slot]);
			npcTintAt[slot] = wclock + 0.5f;
		}
		Render_SkinnedModel(&wp, (u32)m, pose, nb, org, R, npcTint[slot], view);
		/* the muzzle flash, at the weapon's own muzzle */
		s32 mb;
		f32 ml[3][4];
		if (Npc_MuzzleFlash(n) && numNpcFlash < MAX_NPC_FLASH && wp_attach(m, "muzzle", &mb, ml) && mb >= 0 &&
		    (u32)mb < nb) {
			f32 Wm[3][4], M[3][4];
			bn = &wp.npcBones[sk->firstBone + (u32)mb];
			m34_inv(inv, bn->poseToBone);
			m34_mul(Wm, pose[mb], inv);
			m34_mul(M, Wm, ml);
			m34_mul(M, T, M);
			npcFlash[numNpcFlash][0] = M[0][3];
			npcFlash[numNpcFlash][1] = M[1][3];
			npcFlash[numNpcFlash][2] = M[2][3];
			numNpcFlash++;
		}
	}
}

/* move_rope / keyframe_rope: C_RopeKeyframe draws each cable as a strip facing the camera, "Width" wide,
 * lit by the world where it hangs; here in its material's colour (tools/hgmpatch.rope_colour) */
static void draw_ropes(const Camera *cam)
{
	int nr = EnvEnts_NumRopes();
	if (!nr)
		return;
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT6, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT6, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetCullMode(GX_CULL_NONE);
	const f32 ex = cam->origin.x, ey = cam->origin.y, ez = cam->origin.z;
	for (int i = 0; i < nr; i++) {
		RopeDraw rd;
		if (!EnvEnts_Rope(i, &rd) || rd.n < 2)
			continue;
		const f32 *mid = rd.pts[rd.n / 2];
		f32 o[3] = {mid[0] - ex, mid[1] - ey, mid[2] - ez};
		if (o[0] * o[0] + o[1] * o[1] + o[2] * o[2] > 3000.0f * 3000.0f)
			continue;
		u32 col = lit_colour(mid, rd.rgba & 0xFFFFFF00u, 255);
		f32 hw = rd.width * 0.5f;
		GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT6, (u16)(rd.n * 2));
		for (int k = 0; k < rd.n; k++) {
			const f32 *p = rd.pts[k], *q = rd.pts[k + 1 < rd.n ? k + 1 : k], *r0 = rd.pts[k > 0 ? k - 1 : k];
			f32 t[3] = {q[0] - r0[0], q[1] - r0[1], q[2] - r0[2]};
			f32 v[3] = {p[0] - ex, p[1] - ey, p[2] - ez};
			f32 sd[3] = {t[1] * v[2] - t[2] * v[1], t[2] * v[0] - t[0] * v[2], t[0] * v[1] - t[1] * v[0]};
			f32 l = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
			/* (at least about a pixel wide far off, as Source's minimum rope width) */
			f32 dist = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			f32 w = hw > dist * 0.0015f ? hw : dist * 0.0015f;
			f32 s = l > 1e-5f ? w / l : 0;
			GX_Position3f32(p[0] + sd[0] * s, p[1] + sd[1] * s, p[2] + sd[2] * s);
			GX_Color1u32(col);
			GX_Position3f32(p[0] - sd[0] * s, p[1] - sd[1] * s, p[2] - sd[2] * s);
			GX_Color1u32(col);
		}
		GX_End();
	}
	GX_SetCullMode(GX_CULL_BACK);
}

/* env_beam / point_tesla / env_gunfire tracers: textured strips along the bolt (additive); env_fire's
 * flames (sprites/flamelet1-5, animated, stacked) */
static void draw_beams_and_fires(const Camera *cam)
{
	static const int texOf[3] = {FX_LASERBEAM, FX_PHYSBEAM, FX_LIGHTNING};
	int nb = EnvEnts_NumBeams();
	for (int i = 0; i < nb; i++) {
		BeamDraw bd;
		if (!EnvEnts_Beam(i, &bd) || bd.n < 2)
			continue;
		int slot = texOf[bd.tex < 0 || bd.tex > 2 ? 0 : bd.tex];
		if (!fx_bind(slot, FXM_ADD) && !fx_bind(FX_GLOW, FXM_ADD))
			continue;
		for (int k = 0; k + 1 < bd.n; k++) {
			const f32 *a = bd.pts[k], *b = bd.pts[k + 1];
			f32 d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
			f32 len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (len < 1e-3f)
				continue;
			f32 mid[3] = {(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f};
			f32 v[3] = {mid[0] - cam->origin.x, mid[1] - cam->origin.y, mid[2] - cam->origin.z};
			f32 ax[3] = {d[0] / len, d[1] / len, d[2] / len};
			f32 sd[3] = {ax[1] * v[2] - ax[2] * v[1], ax[2] * v[0] - ax[0] * v[2], ax[0] * v[1] - ax[1] * v[0]};
			f32 l = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
			if (l < 1e-5f)
				continue;
			for (int c = 0; c < 3; c++)
				sd[c] /= l;
			fx_quad(mid, ax, sd, len * 0.5f, bd.width * 0.5f, bd.rgba);
		}
	}
	/* cameras' and turrets' eyes (sprites/light_glow03) */
	if (fx_bind(FX_CAMGLOW, FXM_ADD) || fx_bind(FX_GLOW, FXM_ADD))
		for (int i = 0; i < Sentries_Count(); i++) {
			f32 gp[3], gs;
			u32 gc;
			if (Sentries_Glow(i, gp, &gc, &gs))
				fx_billboard(cam, gp, gs, 0, gc);
		}
	if (fx_bind(FX_GLOW, FXM_ADD))
		for (int i = 0; i < Objects_NumMissiles(); i++) {
			f32 mp[3];
			if (Objects_Missile(i, mp)) /* (the rocket's motor) */
				fx_billboard(cam, mp, 12, 0, 0xFFC080FFu);
		}
	if (fx_bind(FX_GLOW, FXM_ADD))
		for (int i = 0; i < Flyers_Count(); i++) {
			f32 gp[3], gs;
			u32 gc;
			if (Flyers_Glow(i, gp, &gc, &gs))
				fx_billboard(cam, gp, gs, 0, gc);
		}
	int nf = EnvEnts_NumFires();
	for (int i = 0; i < nf; i++) {
		FireDraw fd;
		if (!EnvEnts_Fire(i, &fd))
			continue;
		f32 o[3] = {fd.pos[0] - cam->origin.x, fd.pos[1] - cam->origin.y, fd.pos[2] - cam->origin.z};
		if (o[0] * o[0] + o[1] * o[1] + o[2] * o[2] > 3000.0f * 3000.0f)
			continue;
		for (int layer = 0; layer < 3; layer++) {
			int fr = ((int)(wclock * 12.0f + fd.phase * 7 + layer * 2)) % 5;
			if (!fx_bind(FX_FLAME + fr, FXM_ADD))
				break;
			f32 sz = fd.size * (0.5f - layer * 0.1f);
			f32 p[3] = {fd.pos[0], fd.pos[1], fd.pos[2] + sz * (0.9f + layer * 0.7f)};
			u32 a = (u32)(fd.alpha * (220 - layer * 50));
			fx_billboard(cam, p, sz, (f32)layer * 0.5f + fd.phase, 0xFFFFFF00u | a);
		}
	}
}

void Weap_DrawWorld(Mtx view, const Camera *cam)
{
	if (!loaded)
		return;
	draw_npc_weapons(view, cam);
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 0; witems && i < numWItems; i++) {
		WorldItem *it = &witems[i];
		if (!it->alive || !it->placed)
			continue;
		if (it->ent >= 0 && (ents[it->ent].flags & (EF_DISABLED | EF_KILLED)))
			continue;
		s16 m = items[it->item].model;
		if (m < 0 || (u32)m >= wp.numNpcModels)
			continue;
		f32 d[3] = {it->pos[0] - cam->origin.x, it->pos[1] - cam->origin.y, it->pos[2] - cam->origin.z};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd > 2500.0f * 2500.0f || d[0] * cam->forward.x + d[1] * cam->forward.y + d[2] * cam->forward.z < -48)
			continue;
		u32 nb;
		model_pose(m, -1, 0, 1, &nb);
		Render_SkinnedModel(&wp, (u32)m, pose, nb, it->pos, it->R, it->tint, view);
	}
	/* what's in flight, as the game's models: grenades (the world model), rockets, crossbow bolts, SMG
	 * grenades; these point the way they fly */
	s32 frag = weapon_named("weapon_frag");
	static s16 projModel[3] = {-2, -2, -2};
	static const char *const projItem[3] = {"proj_rocket", "proj_bolt", "proj_smgnade"};
	for (int k = 0; k < 3; k++)
		if (projModel[k] == -2) {
			int it = item_named(projItem[k]);
			projModel[k] = it >= 0 ? items[it].model : -1;
		}
	for (int i = 0; i < MAX_PROJ; i++) {
		Proj *p = &projs[i];
		if (!p->alive)
			continue;
		s16 m = -1;
		f32 ang[3] = {p->spin, viewYaw, 0};
		if (p->type == P_FRAG) {
			m = frag >= 0 ? weaps[frag].wm : -1;
		} else if (p->type == P_ROCKET || p->type == P_BOLT || p->type == P_SMGNADE) {
			m = projModel[p->type == P_ROCKET ? 0 : p->type == P_BOLT ? 1 : 2];
			f32 h = sqrtf(p->vel[0] * p->vel[0] + p->vel[1] * p->vel[1]);
			ang[0] = -atan2f(p->vel[2], h) * 180.0f / PI_F;
			ang[1] = atan2f(p->vel[1], p->vel[0]) * 180.0f / PI_F;
		}
		if (m < 0 || (u32)m >= wp.numNpcModels)
			continue;
		f32 R[3][3];
		Ent_AngleMatrix(ang, R);
		u32 nb;
		model_pose(m, -1, 0, 1, &nb);
		Render_SkinnedModel(&wp, (u32)m, pose, nb, p->pos, R, vmTint, view);
	}

	GX_LoadPosMtxImm(view, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	draw_ropes(cam);
	begin_fx();
	draw_impacts(cam);
	draw_beams_and_fires(cam);
	for (int i = 0; i < MAX_PROJ; i++) {
		Proj *p = &projs[i];
		if (!p->alive)
			continue;
		if (p->type == P_ROCKET && fx_bind(FX_GLOW, FXM_ADD)) { /* the motor's glow behind it */
			f32 l = sqrtf(p->vel[0] * p->vel[0] + p->vel[1] * p->vel[1] + p->vel[2] * p->vel[2]);
			f32 q[3] = {p->pos[0], p->pos[1], p->pos[2]};
			if (l > 1)
				for (int k = 0; k < 3; k++)
					q[k] -= p->vel[k] / l * 14.0f;
			fx_billboard(cam, q, 16, 0, 0xFFC878FFu);
		} else if (p->type == P_BALL && fx_bind(FX_BALL, FXM_ADD)) { /* the AR2's energy ball (effects/ar2_altfire1) */
			fx_billboard(cam, p->pos, 12, p->t * 6.0f, 0xFFFFFFFFu);
		} else if (p->type == P_FRAG && (((int)(p->t * 4)) & 1) && fx_bind(FX_GLOW, FXM_ADD)) {
			fx_billboard(cam, p->pos, 5, 0, 0xFF2020FFu); /* the blinking light */
		}
	}
	for (int i = 0; i < numNpcFlash; i++) /* NPC muzzle flashes (effects/muzzleflash1-4) */
		if (fx_bind(FX_MUZZLE + (rand() & 3), FXM_ADD))
			fx_billboard(cam, npcFlash[i], 9 + (f32)(rand() & 3), (f32)(rand() & 255) * 0.0246f, 0xFFFFFFFFu);
	for (int i = 0; i < MAX_FX; i++) {
		Fx *f = &fxs[i];
		if (!f->alive)
			continue;
		f32 k = f->t / f->dur;
		if (f->type == WFX_EXPLOSION) {
			/* sprites/zerogxplode: its 15 frames over the blast, then smoke hanging where it was */
			int fr = (int)(k * 15.0f * 1.6f);
			if (!(f->flags & EXPL_NOFIREBALL) && fr < 15 && fx_bind(FX_EXPLODE + fr, FXM_ADD))
				fx_billboard(cam, f->pos, 110 * f->scale, 0, 0xFFFFFFFFu);
			if (!(f->flags & EXPL_NOSMOKE) && k > 0.25f && fx_bind(FX_SMOKE, FXM_ALPHA)) {
				f32 up[3] = {f->pos[0], f->pos[1], f->pos[2] + (k - 0.25f) * 60};
				fx_billboard(cam, up, (60 + k * 70) * f->scale, (f32)i, 0x30303000u | (u32)((1 - k) * 150));
			}
		} else if (f->type == WFX_MUZZLE) {
			if (fx_bind(FX_MUZZLE + (i & 3), FXM_ADD))
				fx_billboard(cam, f->pos, 12 * f->scale, (f32)(i * 37) * 0.0246f, 0xFFFFFFFFu);
		} else if (f->type == WFX_SPARK) {
			if (fx_bind(FX_SPARK, FXM_ADD))
				fx_billboard(cam, f->pos, 3 + k * 4, (f32)i, 0xFFFFFF00u | (u32)((1 - k) * 255));
		} else if (fx_bind(FX_SMOKE, FXM_ALPHA)) {
			fx_billboard(cam, f->pos, 2 + k * 5, (f32)i, 0xC0B0A000u | (u32)((1 - k) * 160));
		}
	}
	end_sprites();
}

void Weap_DrawViewmodel(Mtx view, const Camera *cam, f32 proj[4][4], u16 width, u16 height)
{
	(void)view;
	if (!loaded || inv.cur < 0 || deadAt > 0)
		return;
	const PackSkel *sk = vm_skel(inv.cur);
	if (!sk || vseq < 0)
		return;
	s16 m = weaps[inv.cur].vm;
	if (wclock >= tintAt) {
		f32 p[3] = {cam->origin.x, cam->origin.y, cam->origin.z};
		sample_tint(p, vmTint);
		tintAt = wclock + 0.5f;
	}
	Anim_Pose(&wp, sk, vseq, vt, vhold, vprev, vprevT, vblend, pose);
	/* Source's viewmodel: at the eye, facing the view, its own 54 degree field of view */
	f32 ang[3] = {cam->pitch, cam->yaw, 0}, R[3][3];
	Ent_AngleMatrix(ang, R);
	f32 bx = sinf(bobPhase) * 0.6f * bobAmt, bz = -fabsf(cosf(bobPhase)) * 0.5f * bobAmt;
	f32 o[3];
	for (int r = 0; r < 3; r++)
		o[r] = (r == 0 ? cam->origin.x : r == 1 ? cam->origin.y : cam->origin.z) + R[r][1] * bx + R[r][2] * bz;
	Mtx vv;
	guVector eyeV = {0, 0, 0}, up = cam->up, tgt = cam->forward;
	guLookAt(vv, &eyeV, &up, &tgt);
	/* view matrix relative to the eye, then the model placed relative to the eye too (precision) */
	f32 rel[3] = {o[0] - cam->origin.x, o[1] - cam->origin.y, o[2] - cam->origin.z};
	Mtx44 vp;
	f32 fovy = 2.0f * atanf(tanf(54.0f * PI_F / 360.0f) * 0.75f) * 180.0f / PI_F;
	guPerspective(vp, fovy, 4.0f / 3.0f, 0.1f, 512.0f); /* (close enough that no part of the gun is ever clipped) */
	GX_LoadProjectionMtx(vp, GX_PERSPECTIVE);
	GX_SetViewport(0, 0, width, height, 0.0f, 0.02f); /* always in front of the world */
	GX_SetCullMode(GX_CULL_NONE); /* (every face of the gun, whichever way it winds) */
	Render_SkinnedModel(&wp, (u32)m, pose, sk->numBones, rel, R, vmTint, vv);
	GX_SetCullMode(GX_CULL_BACK);
	if (wclock < muzzleUntil) {
		GX_LoadPosMtxImm(vv, GX_PNMTX0);
		GX_SetCurrentMtx(GX_PNMTX0);
		begin_fx();
		f32 mz[3] = {R[0][0] * 30 - R[0][1] * 5 - R[0][2] * 4, R[1][0] * 30 - R[1][1] * 5 - R[1][2] * 4,
		             R[2][0] * 30 - R[2][1] * 5 - R[2][2] * 4};
		if (fx_bind(FX_MUZZLE + (int)(muzzleUntil * 1000.0f) % 4, FXM_ADD)) /* effects/muzzleflash1-4 */
			fx_billboard(cam, mz, 7, muzzleUntil * 37.0f, 0xFFFFFFFFu);
		end_sprites();
	}
	GX_SetViewport(0, 0, width, height, 0.0f, 1.0f);
	GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
}

/* ------------------------------------------------------------------ HUD (HL2's layout at 640x480) */

#define HUD_FG 0xFFDC00FFu  /* "FgColor" 255 220 0 */
#define HUD_LOW 0xFF0000FFu
#define HUD_BG 0x0000004Cu  /* "BgColor" 0 0 0 76 */

static const char *const weaponTitles[] = {
	"weapon_crowbar", "CROWBAR", "weapon_physcannon", "ZERO-POINT ENERGY GUN", "weapon_pistol", "9MM PISTOL",
	"weapon_357", ".357 MAGNUM", "weapon_smg1", "SMG", "weapon_ar2", "PULSE-RIFLE", "weapon_shotgun", "SHOTGUN",
	"weapon_crossbow", "CROSSBOW", "weapon_frag", "GRENADE", "weapon_rpg", "RPG", "weapon_bugbait", "BUGBAIT",
};

static const char *weapon_title(s32 w)
{
	const char *n = wstr(weaps[w].name);
	for (unsigned i = 0; i < sizeof(weaponTitles) / sizeof(weaponTitles[0]); i += 2)
		if (!strcmp(n, weaponTitles[i]))
			return weaponTitles[i + 1];
	return n;
}

static void box(f32 x, f32 y, f32 w, f32 h, u32 rgba)
{
	/* rounded corners: trim the corner pixels */
	Text_Rect(x + 3, y, x + w - 3, y + h, rgba);
	Text_Rect(x, y + 3, x + 3, y + h - 3, rgba);
	Text_Rect(x + w - 3, y + 3, x + w, y + h - 3, rgba);
}

static void number(f32 x, f32 y, int v, u32 rgba, f32 scale)
{
	char s[16];
	snprintf(s, sizeof(s), "%d", v);
	if (Text_HasFont(FONT_TITLE)) {
		/* HudNumbers: halflife2.ttf, 32 tall (title font is built at 64); HudNumbersSmall 16 */
		f32 lh = Text_LineHeight(FONT_TITLE, 1);
		f32 sc = (scale >= 1.0f ? 32.0f : 16.0f) / (lh > 0 ? lh : 64.0f) * 1.22f;
		Text_Draw(FONT_TITLE, x, y, s, -1, rgba, sc);
		return;
	}
	Text_Draw(FONT_HUD, x, y, s, -1, rgba, scale);
}

/* a HUD icon: a halflife2.ttf glyph from the icon font, tall = its font's size on a 480-line screen */
static f32 icon_w(u8 ch, f32 tall)
{
	char c[2] = {(char)ch, 0};
	f32 lh = Text_LineHeight(FONT_ICONS, 1);
	return Text_Width(FONT_ICONS, c, 1, tall / (lh > 0 ? lh : 64.0f));
}

static void icon(f32 x, f32 y, u8 ch, f32 tall, u32 rgba)
{
	char c[2] = {(char)ch, 0};
	f32 lh = Text_LineHeight(FONT_ICONS, 1);
	Text_Draw(FONT_ICONS, x, y, c, 1, rgba, tall / (lh > 0 ? lh : 64.0f));
}

static f32 icon_line(f32 tall)
{
	f32 lh = Text_LineHeight(FONT_ICONS, 1);
	return Text_LineHeight(FONT_ICONS, tall / (lh > 0 ? lh : 64.0f));
}

/* CHUDQuickInfo: brackets either side of the crosshair (hl2crosshairs.ttf, QuickInfo 28 tall): health on
 * the left, the clip on the right. Each is the empty bracket ("{", "}") with the full one ("[", "]")
 * over the part that's left, filled from the bottom (DrawIconProgressBar, vertical). At 138/255 alpha;
 * low health or clip pulses in the caution colour. */
static f32 qiLastHealth = -1, qiLastClip = -1, qiShowUntil;
static u16 hudW, hudH;
static void quick_info(f32 cx, f32 cy, f32 hs, const WeapRec *w)
{
	if (!Text_HasFont(FONT_CROSS))
		return;
	f32 lh1 = Text_LineHeight(FONT_CROSS, 1);
	f32 sc = 28.0f * hs / (lh1 > 0 ? lh1 : 64.0f);
	f32 gw = Text_Width(FONT_CROSS, "[", 1, sc), gh = Text_LineHeight(FONT_CROSS, sc);
	f32 y0 = cy - gh * 0.5f;
	f32 hp = inv.health / 100.0f;
	f32 clip = 1.0f;
	int haveClip = w->ammo != A_NONE && has_clip(w) && w->clip > 1;
	if (haveClip)
		clip = (f32)inv.clip[inv.cur] / (f32)w->clip;
	hp = hp < 0 ? 0 : hp > 1 ? 1 : hp;
	clip = clip < 0 ? 0 : clip > 1 ? 1 : clip;
	int warnH = inv.health <= 25, warnA = haveClip && clip <= 0.25f; /* HEALTH_WARNING_THRESHOLD, CLIP_PERC_THRESHOLD */
	f32 pulse = fabsf(sinf(wclock * 8.0f));
	for (int side = 0; side < 2; side++) {
		f32 frac = side ? clip : hp;
		int warn = side ? warnA : warnH;
		u32 a = warn ? (u32)(255 * pulse) : 138;
		u32 col = (warn ? 0xFF000000u : 0xFFDC0000u) | a;
		f32 x = side ? cx + gw : cx - gw * 2.0f;
		const char *empty = side ? "}" : "{", *full = side ? "]" : "[";
		Text_Draw(FONT_CROSS, x, y0, empty, 1, col, sc);
		/* the full bracket over the bottom part */
		f32 cut = y0 + gh * (1.0f - frac);
		u32 sy = cut < 0 ? 0 : (u32)cut;
		u32 sh = (u32)(y0 + gh + 2.0f) > sy ? (u32)(y0 + gh + 2.0f) - sy : 0;
		if (frac > 0 && sh) {
			GX_SetScissor((u32)(x > 0 ? x : 0), sy, (u32)(gw + 4), sh);
			Text_Draw(FONT_CROSS, x, y0, full, 1, col, sc);
			GX_SetScissor(0, 0, hudW, hudH);
		}
	}
	(void)qiLastHealth;
	(void)qiLastClip;
	(void)qiShowUntil;
}

static void draw_history(u16 width)
{
	/* HudHistoryResource: r252, y 40, 248 x 320, history_gap 56, icon_inset 38, text_inset 26 */
	f32 x0 = width - 252.0f, y0 = 40, w = 248, h = 320;
	for (int i = 0; i < MAX_HIST; i++) {
		Hist *e = &hist[i];
		if (!e->alive)
			continue;
		f32 left = e->until - wclock;
		if (left <= 0) {
			e->alive = 0;
			continue;
		}
		f32 a = left * 80.0f;
		a = a > 255 ? 255 : a;
		u32 col = 0xFFDC0000u | (u32)a;
		if (e->kind == HIST_WEAPON && e->id >= 0 && (u32)e->id < numWeaps && weaps[e->id].ammo != A_NONE &&
		    inv.ammo[weaps[e->id].ammo] <= 0 && inv.clip[e->id] <= 0)
			col = 0xFF000000u | (u32)a; /* (no ammo for it: red) */
		f32 tall = e->kind == HIST_AMMO ? 32.0f : 64.0f; /* WeaponIconsSmall / WeaponIcons */
		f32 iw = icon_w(e->icon, tall);
		f32 ypos = y0 + h - 56.0f * (f32)(i + 1);
		if (e->kind == HIST_AMMO)
			ypos += tall * 0.5f;
		f32 xpos = x0 + w - iw - 38.0f;
		icon(xpos, ypos, e->icon, tall, col);
		if (e->icon2)
			icon(xpos - icon_w(e->icon2, tall) * 1.25f, ypos, e->icon2, tall, col);
		if (e->count)
			number(x0 + w - 26.0f, ypos, e->count, col, 0.5f);
	}
}

/* HudSquadStatus: r120, y 380, 104 x 46; an icon per member ('C', medics 'M', HudNumbers), 24 apart */
static void draw_squad(u16 width)
{
	u32 nn, ne;
	Npc *L = Npc_List(&nn);
	Entity *E = Ent_List(&ne);
	int members = 0, medics = 0;
	for (u32 i = 0; i < nn; i++) {
		if (L[i].dead || L[i].followEnt != 0 || L[i].ent <= 0 || (u32)L[i].ent >= ne)
			continue;
		const Entity *e = &E[L[i].ent];
		if (e->flags & (EF_DISABLED | EF_KILLED))
			continue;
		members++;
		if (!strcmp(e->classname, "npc_citizen") && (e->spawnflags & 131072))
			medics++;
	}
	if (!members)
		return;
	if (members > 4)
		members = 4;
	f32 x = width - 120.0f, y = 380;
	box(x, y, 104, 46, HUD_BG);
	f32 lh = Text_LineHeight(FONT_TITLE, 1), sc = 32.0f / (lh > 0 ? lh : 64.0f) * 1.22f;
	for (int k = 0; k < members; k++)
		Text_Draw(FONT_TITLE, x + 8 + k * 24.0f, y, k < medics ? "M" : "C", 1, 0xFFDC00A0u, sc);
	Text_Draw(FONT_CAPTION, x + 8, y + 34, "SQUAD FOLLOWING", -1, 0xFFDC00A0u, 0.55f);
}

/* HudPoisonDamageIndicator: 16, 346, 136 x 38, while poison is being healed */
static void draw_poison(void)
{
	if (poisonLeft <= 0)
		return;
	box(16, 346, 136, 38, HUD_BG);
	Text_Draw(FONT_CAPTION, 24, 354, "NEUROTOXIN DETECTED", -1, 0xFFAA00DCu, 0.55f);
	Text_Draw(FONT_CAPTION, 24, 368, "ADMINISTERING ANTIDOTE", -1, 0xFFAA00DCu, 0.55f);
}

static void hud_panel(f32 x, f32 y, f32 w, const char *label, int value, u32 col)
{
	box(x, y, w, 36, HUD_BG);
	Text_Draw(FONT_CAPTION, x + 8, y + 20, label, -1, col, 0.75f);
	number(x + 50, y + 2, value, col, 1.3f);
}

void Weap_DrawHud(u16 width, u16 height)
{
	hudW = width;
	hudH = height;
	if (!loaded)
		return;
	draw_damage_indicator(width, height);
	if (!inv.suit)
		return;
	f32 sx = width / 640.0f, sy = height / 480.0f;
	(void)sx;
	f32 by = 432 * sy;
	int hp = (int)ceilf(inv.health);
	hud_panel(16, by, 102, "HEALTH", hp, hp <= 20 ? HUD_LOW : HUD_FG);
	/* aux power (shown while it's below full or a device is on) */
	int devices = sprintActive + flashlightOn + breatherActive;
	if (inv.aux < 100 || devices) {
		f32 ay = by - 46 - devices * 10;
		box(16, ay, 102, 40 + devices * 10, HUD_BG);
		u32 col = inv.aux < 25 ? HUD_LOW : HUD_FG;
		Text_Draw(FONT_CAPTION, 24, ay + 4, "AUX POWER", -1, col, 0.6f);
		for (int k = 0; k < 10; k++) {
			int lit = inv.aux > k * 10;
			Text_Rect(24 + k * 8.6f, ay + 20, 24 + k * 8.6f + 6, ay + 26, lit ? col : (col & 0xFFFFFF00u) | 0x40);
		}
		f32 ly = ay + 30;
		if (breatherActive) {
			Text_Draw(FONT_CAPTION, 24, ly, "OXYGEN", -1, col, 0.55f);
			ly += 10;
		}
		if (flashlightOn) {
			Text_Draw(FONT_CAPTION, 24, ly, "FLASHLIGHT", -1, col, 0.55f);
			ly += 10;
		}
		if (sprintActive)
			Text_Draw(FONT_CAPTION, 24, ly, "SPRINT", -1, col, 0.55f);
	}
	if (inv.armor > 0)
		hud_panel(140, by, 108, "SUIT", (int)inv.armor, HUD_FG);
	draw_history(width);
	draw_squad(width);
	draw_poison();

	if (inv.cur >= 0 && deadAt == 0) {
		const WeapRec *w = &weaps[inv.cur];
		if (w->ammo != A_NONE) {
			const char *cls = wstr(w->name);
			int alt = !strcmp(cls, "weapon_smg1") ? A_SMG1_GRENADE : !strcmp(cls, "weapon_ar2") ? A_AR2_ALT : 0;
			/* HudLayout.res: HudAmmo at r150 (136 wide); with secondary ammo hudanimations slides it to r222
			 * and HudAmmoSecondary sits at r76 (60 wide) */
			f32 x = width - (alt ? 222.0f : 150.0f);
			box(x, by, 136, 36, HUD_BG);
			Text_Draw(FONT_CAPTION, x + 8, by + 20, "AMMO", -1, HUD_FG, 0.75f);
			if (has_clip(w)) {
				number(x + 44, by + 2, inv.clip[inv.cur], inv.clip[inv.cur] ? HUD_FG : HUD_LOW, 1.3f);
				number(x + 98, by + 16, inv.ammo[w->ammo], HUD_FG, 0.7f);
			} else {
				number(x + 44, by + 2, inv.ammo[w->ammo], inv.ammo[w->ammo] ? HUD_FG : HUD_LOW, 1.3f);
			}
			if (alt) {
				f32 x2 = width - 76.0f;
				box(x2, by, 60, 36, HUD_BG);
				Text_Draw(FONT_CAPTION, x2 + 8, by + 22, "ALT", -1, HUD_FG, 0.75f);
				number(x2 + 36, by + 2, inv.ammo[alt], inv.ammo[alt] ? HUD_FG : HUD_LOW, 1.3f);
			}
		}
		/* CHudCrosshair: the weapon's crosshair glyph (its script's "crosshair": Crosshairs font, halflife2.ttf
		 * 40 tall), centred, in the HUD colour */
		f32 cx = width * 0.5f, cy = height * 0.5f;
		f32 hs = height / 480.0f;
		u8 xh = inv.cur >= 0 && inv.cur < 32 ? wIcon[inv.cur][3] : 0;
		if (xh && Text_HasFont(FONT_ICONS)) {
			f32 tall = 40.0f * hs;
			icon(cx - icon_w(xh, tall) * 0.5f, cy - icon_line(tall) * 0.5f, xh, tall, 0xFFDC00FFu);
		} else {
			Text_Rect(cx - 1, cy - 1, cx + 1, cy + 1, 0xFFDC00C8u);
		}
		quick_info(cx, cy, hs, w);
	}

	/* in a vehicle with its gun: HL2's vehicle crosshair, and the airboat gun's heat where the ammo goes */
	{
		f32 heat;
		if (Veh_GunState(&heat)) {
			f32 cx = width * 0.5f, cy = height * 0.5f;
			u32 cc = 0xFFDC00C8u;
			Text_Rect(cx - 1, cy - 1, cx + 1, cy + 1, cc);
			Text_Rect(cx - 12, cy - 1, cx - 6, cy + 1, cc);
			Text_Rect(cx + 6, cy - 1, cx + 12, cy + 1, cc);
			Text_Rect(cx - 1, cy + 6, cx + 1, cy + 12, cc);
			if (heat > 0.01f) {
				f32 x = width - 150.0f;
				box(x, by, 136, 36, HUD_BG);
				Text_Draw(FONT_CAPTION, x + 8, by + 20, "AMMO", -1, HUD_FG, 0.75f);
				u32 col = heat > 0.8f ? HUD_LOW : HUD_FG;
				Text_Rect(x + 44, by + 14, x + 44 + 80 * (1.0f - (heat > 1 ? 1 : heat)), by + 22, col);
			}
		}
	}
	/* weapon selection: the slot row along the top, the chosen weapon named */
	if (wclock < selShowUntil && (inv.cur >= 0 || switchTo >= 0)) {
		s32 sel = switchTo >= 0 ? switchTo : inv.cur;
		f32 a = selShowUntil - wclock < 0.3f ? (selShowUntil - wclock) / 0.3f : 1;
		/* HudWeaponSelection: 32x32 boxes per slot, the chosen one 112x80, 8 apart, 16 from the top,
		 * the weapon's name at y 64 inside the large box */
		f32 total = 5 * 32.0f + 112.0f + 5 * 8.0f;
		f32 x = width * 0.5f - total * 0.5f, y = 16;
		for (int slot = 0; slot < 6; slot++) {
			s32 inSlot = -1;
			for (u32 i = 0; i < numWeaps; i++)
				if (weaps[i].slot == slot && inv.have[i] && (inSlot < 0 || (s32)i == sel))
					inSlot = (s32)i;
			int chosen = inSlot >= 0 && inSlot == sel;
			f32 w = chosen ? 112.0f : 32.0f, h = chosen ? 80.0f : 32.0f;
			box(x, y, w, h, (u32)(a * (inSlot >= 0 ? 76 : 40)));
			char num[4];
			snprintf(num, sizeof(num), "%d", slot + 1);
			Text_Draw(FONT_CAPTION, x + 4, y + 4, num, -1, 0xFFDC0000u | (u32)(a * 255), 0.7f);
			if (chosen) {
				/* its icon (WeaponIconsSelected's glow under WeaponIcons), centred above the name */
				u8 ic = inSlot < 32 ? wIcon[inSlot][0] : 0;
				if (ic) {
					f32 iw = icon_w(ic, 64.0f);
					f32 ix = x + (w - iw) * 0.5f, iy = y + 6;
					icon(ix - 1, iy - 1, ic, 64.0f, 0xFFDC0000u | (u32)(a * 60));
					icon(ix + 1, iy + 1, ic, 64.0f, 0xFFDC0000u | (u32)(a * 60));
					icon(ix, iy, ic, 64.0f, 0xFFDC0000u | (u32)(a * 255));
				}
				const char *t = weapon_title(inSlot);
				f32 tw = Text_Width(FONT_CAPTION, t, -1, 0.6f);
				Text_Draw(FONT_CAPTION, x + (w - tw) * 0.5f, y + 64, t, -1, 0xFFDC0000u | (u32)(a * 255), 0.6f);
			}
			x += w + 8;
		}
	}
}
#endif /* HOST_TEST */
