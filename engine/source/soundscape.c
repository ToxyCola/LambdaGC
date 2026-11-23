/*
 * Soundscapes, like Source's env_soundscape + C_SoundscapeSystem: the level's ambience (looping beds,
 * distant random sounds, sounds from fixed spots) and its room reverb, chosen by where the player is.
 *
 * An env_soundscape takes over when the player is inside its radius and it can see him (a clear line
 * from it to his ears), and it's closer than the current one; the current one stays until then, as in
 * Source. env_soundscape_triggerable ones take over when a trigger_soundscape is touched (the
 * entity's "Enable"/"ToggleEnabled" inputs and the trigger call Scape_Force). Loops cross-fade over a
 * second when the soundscape changes; random sounds play at their interval, at a random spot around the
 * player or at one of the soundscape's position0-7 targets.
 */
#include "soundscape.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "collide.h"

#define MAX_LOOPS 16
#define MAX_RANDOM 24
#define FADE_TIME 1.0f

static const Pack *P;
static Entity *ents;
static u32 numEnts;

static s32 current = -1;          /* active env_soundscape entity */
static s32 forced = -1;           /* a trigger_soundscape's choice */
static f32 checkTimer, clock_;

typedef struct {
	s16 snd;         /* sound index, -1 free */
	u32 handle;
	f32 vol, target; /* current and wanted volume (fades) */
	f32 pos[3];
	u8 positional, level;
} Loop;
static Loop loops[MAX_LOOPS];

typedef struct {
	f32 next;        /* when the next random sound plays */
} RandomState;
static RandomState rnd[MAX_RANDOM];

static f32 frand(void) { return (f32)(rand() & 0x7FFF) / 32767.0f; }
static f32 range(f32 a, f32 b) { return a + (b - a) * frand(); }

static const char *kv(const Entity *e, const char *key) { return Ent_KV((s32)(e - ents), key); }

static int scape_of(const Entity *e)
{
	const char *v = kv(e, "_scape");
	if (!v)
		return -1;
	int k = atoi(v);
	return (k >= 0 && (u32)k < P->numScapes) ? k : -1;
}

/* the env_soundscape's position<N> target's origin */
static int position(const Entity *e, int n, f32 out[3])
{
	char key[16] = "position0";
	key[8] = (char)('0' + n);
	const char *name = kv(e, key);
	if (!name || !*name)
		return 0;
	for (u32 i = 1; i < numEnts; i++)
		if (ents[i].name && !strcasecmp(ents[i].name, name)) {
			memcpy(out, ents[i].origin, 12);
			return 1;
		}
	return 0;
}

void Scape_Init(const Pack *p)
{
	P = p;
	ents = Ent_List(&numEnts);
	current = forced = -1;
	checkTimer = 0;
	clock_ = 0;
	memset(loops, 0, sizeof(loops));
	for (int i = 0; i < MAX_LOOPS; i++)
		loops[i].snd = -1;
	memset(rnd, 0, sizeof(rnd));
	Audio_SetDsp(1);
}

void Scape_Force(s32 ent)
{
	if (ent > 0 && (u32)ent < numEnts && scape_of(&ents[ent]) >= 0)
		forced = ent;
}

void Scape_Stop(void)
{
	for (int i = 0; i < MAX_LOOPS; i++) {
		if (loops[i].handle)
			Audio_Stop(loops[i].handle);
		loops[i].handle = 0;
		loops[i].snd = -1;
	}
	current = forced = -1;
}

/* can the soundscape at e hear the player (Source: within radius, and a clear line to his ears)? */
static int in_range(const Entity *e, const f32 ear[3], f32 *dist)
{
	f32 d[3] = {ear[0] - e->origin[0], ear[1] - e->origin[1], ear[2] - e->origin[2]};
	*dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	const char *r = kv(e, "radius");
	f32 radius = r ? (f32)atof(r) : 128.0f;
	if (radius >= 0 && *dist > radius)
		return 0;
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_SetSkipBoxes(1);
	CM_BoxTrace(&tr, e->origin, ear, z, z, CONTENTS_SOLID);
	CM_SetSkipBoxes(0);
	return tr.fraction >= 0.99f || (!tr.startsolid && *dist * (1 - tr.fraction) < 8);
}

static void choose(const f32 ear[3])
{
	if (forced > 0) {
		current = forced;
		return;
	}
	f32 best = 1e30f;
	s32 pick = -1;
	f32 curDist = 1e30f;
	for (u32 i = 1; i < numEnts; i++) {
		const Entity *e = &ents[i];
		if ((e->flags & (EF_KILLED | EF_DISABLED)) || !strcmp(e->classname, "env_soundscape_triggerable") ||
		    scape_of(e) < 0)
			continue;
		f32 d;
		if (!in_range(e, ear, &d))
			continue;
		if ((s32)i == current)
			curDist = d;
		if (d < best) {
			best = d;
			pick = (s32)i;
		}
	}
	/* the current one stays until another one hears the player and is closer */
	if (pick > 0 && (current < 0 || best < curDist))
		current = pick;
}

/* loops: what the current soundscape wants playing, faded in; the rest faded out */
static void update_loops(f32 dt, const Entity *e, const PackScape *sc)
{
	for (int i = 0; i < MAX_LOOPS; i++)
		loops[i].target = 0;
	if (sc) {
		for (u32 k = 0; k < sc->numElems; k++) {
			const PackScapeElem *el = &P->scapeElems[sc->firstElem + k];
			if (el->type != 0 || !el->numWaves || el->firstWave >= P->numScapeWaves)
				continue;
			s16 snd = P->scapeWaves[el->firstWave];
			f32 pos[3];
			int positional = el->pos < 8 && position(e, el->pos, pos);
			int slot = -1, freeSlot = -1;
			for (int i = 0; i < MAX_LOOPS; i++) {
				if (loops[i].snd == snd && (!positional || !memcmp(loops[i].pos, pos, 12)) && loops[i].target == 0)
					slot = i;
				if (loops[i].snd < 0 && freeSlot < 0)
					freeSlot = i;
			}
			if (slot < 0) {
				if (freeSlot < 0)
					continue;
				slot = freeSlot;
				Loop *l = &loops[slot];
				l->snd = snd;
				l->vol = 0;
				l->handle = 0;
				l->positional = (u8)positional;
				l->level = positional ? el->level : 0;
				if (positional)
					memcpy(l->pos, pos, 12);
			}
			loops[slot].target = (el->volMin + el->volMax) * 0.5f;
		}
	}
	f32 step = dt / FADE_TIME;
	for (int i = 0; i < MAX_LOOPS; i++) {
		Loop *l = &loops[i];
		if (l->snd < 0)
			continue;
		if (l->vol < l->target)
			l->vol = l->vol + step > l->target ? l->target : l->vol + step;
		else if (l->vol > l->target)
			l->vol = l->vol - step < l->target ? l->target : l->vol - step;
		if (l->target <= 0 && l->vol <= 0) {
			if (l->handle)
				Audio_Stop(l->handle);
			l->handle = 0;
			l->snd = -1;
			continue;
		}
		if ((u32)l->snd >= P->numSounds)
			continue;
		/* (re)start it: a loop without loop points in its wav restarts when it ends, as Source loops them */
		if (!l->handle || !Audio_IsPlaying(l->handle))
			l->handle = Audio_Play(&P->sounds[l->snd], l->positional ? l->pos : NULL, l->vol > 0.01f ? l->vol : 0.01f,
			                       l->positional ? (f32)l->level : 0);
		else
			Audio_SetChannelVolume(l->handle, l->vol);
	}
}

static void update_random(const f32 ear[3], const Entity *e, const PackScape *sc)
{
	if (!sc)
		return;
	u32 r = 0;
	for (u32 k = 0; k < sc->numElems && r < MAX_RANDOM; k++) {
		const PackScapeElem *el = &P->scapeElems[sc->firstElem + k];
		if (el->type != 1)
			continue;
		RandomState *st = &rnd[r++];
		if (st->next <= 0) {
			st->next = clock_ + range(el->timeMin, el->timeMax); /* (not all at once on entering) */
			continue;
		}
		if (clock_ < st->next)
			continue;
		st->next = clock_ + range(el->timeMin > 0.1f ? el->timeMin : 0.1f, el->timeMax > el->timeMin ? el->timeMax : el->timeMin + 0.1f);
		if (!el->numWaves)
			continue;
		u32 w = el->firstWave + (u32)(rand() % el->numWaves);
		if (w >= P->numScapeWaves)
			continue;
		s16 snd = P->scapeWaves[w];
		if (snd < 0 || (u32)snd >= P->numSounds)
			continue;
		f32 vol = range(el->volMin, el->volMax);
		f32 pos[3];
		if (el->pos == 254) {
			/* somewhere around the player (C_SoundscapeSystem's random position) */
			f32 a = frand() * 6.2831853f, d = range(256.0f, 768.0f);
			pos[0] = ear[0] + cosf(a) * d;
			pos[1] = ear[1] + sinf(a) * d;
			pos[2] = ear[2] + range(-64.0f, 256.0f);
			Audio_Play(&P->sounds[snd], pos, vol, el->level ? (f32)el->level : 0);
		} else if (el->pos < 8 && position(e, el->pos, pos)) {
			Audio_Play(&P->sounds[snd], pos, vol, el->level ? (f32)el->level : 0);
		} else {
			Audio_Play(&P->sounds[snd], NULL, vol, 0);
		}
	}
}

/* trigger_soundscape: while the player is inside, its env_soundscape_triggerable is the one */
static void triggers(const f32 feet[3])
{
	for (u32 i = 1; i < numEnts; i++) {
		const Entity *t = &ents[i];
		if (strcmp(t->classname, "trigger_soundscape") || (t->flags & (EF_KILLED | EF_DISABLED)) || t->bmodel <= 0 ||
		    (u32)t->bmodel >= P->numBModels)
			continue;
		const PackBModel *b = &P->bmodels[t->bmodel];
		int in = 1;
		for (int k = 0; k < 3; k++)
			if (feet[k] < b->mins[k] + t->origin[k] - 16 || feet[k] > b->maxs[k] + t->origin[k] + 16)
				in = 0;
		if (!in)
			continue;
		const char *name = kv(t, "soundscape");
		if (!name)
			continue;
		for (u32 j = 1; j < numEnts; j++)
			if (ents[j].name && !strcasecmp(ents[j].name, name) && scape_of(&ents[j]) >= 0) {
				forced = (s32)j;
				return;
			}
	}
}

void Scape_Tick(f32 dt, const f32 ear[3], const f32 feet[3])
{
	if (!P || !P->numScapes)
		return;
	clock_ += dt;
	checkTimer -= dt;
	s32 before = current;
	if (checkTimer <= 0) {
		checkTimer = 0.25f;
		triggers(feet);
		choose(ear);
		/* a triggered one holds until a plain env_soundscape takes over again */
		if (forced > 0 && current != forced)
			forced = -1;
	}
	if (current != before) {
		memset(rnd, 0, sizeof(rnd));
		const Entity *ce = current > 0 ? &ents[current] : NULL;
		int k = ce ? scape_of(ce) : -1;
		Audio_SetDsp(k >= 0 ? P->scapes[k].dsp : 1);
	}
	const Entity *e = current > 0 ? &ents[current] : NULL;
	int k = e ? scape_of(e) : -1;
	const PackScape *sc = k >= 0 ? &P->scapes[k] : NULL;
	update_loops(dt, e, sc);
	update_random(ear, e, sc);
}

s32 Scape_Current(void) { return current; }
