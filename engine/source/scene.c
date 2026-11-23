#include "scene.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "caption.h"
#include "ent.h"
#include "face.h"
#include "npc.h"

#define MAX_RUNS 12
/* captions only for lines the player can actually hear (about 3000 units in the open, less through walls) */
#define CAPTION_GAIN 0.12f
#define MAX_ACTORS 8

enum { ST_PENDING, ST_RUNNING, ST_DONE };

typedef struct {
	s32 ent;                  /* scene entity, -1 = slot free */
	s32 activator;
	const PackScene *sc;
	f32 t, autoResume;
	u8 paused, autoCancel;
	u8 waitActors;            /* a "noaction" section: continue once the actors stop moving and talking */
	u8 waitStart;             /* busyactor 1: playback starts once none of its actors is talking */
	f32 pausedAt;
	u8 *state;
	s16 *loops;
	s8 *slot;                 /* gesture events: the actor's gesture slot, -1 none */
	s32 waitEnt;              /* NPC entity whose move-to the scene is waiting on */
	f32 waitLeft;
	u8 *pre;                  /* speak events whose line has been asked for from the disc */
	f32 held;                 /* how long the clock has waited on a line still coming off the disc */
	f32 heldTotal;            /* (tracing) */
	u32 actorKey[MAX_ACTORS];
	s32 actorEnt[MAX_ACTORS];
	u32 numActors;
} Run;

static const Pack *P;
static Player *player;
static Run runs[MAX_RUNS];
static f32 now;
u32 sceneGestures, sceneLooks; /* counters for the host tests */

static const char *pstr(u32 off) { return P->entStrings + off; }

void Scene_Init(const Pack *p, Player *pl)
{
	Scene_Free();
	P = p;
	player = pl;
	now = 0;
	for (int i = 0; i < MAX_RUNS; i++)
		runs[i].ent = -1;
}

static void prefetch_ahead(Run *r);
static void release(Run *r)
{
	free(r->state);
	free(r->loops);
	free(r->slot);
	free(r->pre);
	memset(r, 0, sizeof(*r));
	r->ent = -1;
}

void Scene_Free(void)
{
	for (int i = 0; i < MAX_RUNS; i++)
		if (runs[i].ent >= 0)
			release(&runs[i]);
}

static Run *find_run(s32 ent)
{
	for (int i = 0; i < MAX_RUNS; i++)
		if (runs[i].ent == ent)
			return &runs[i];
	return NULL;
}

int Scene_IsPlaying(s32 ent) { return find_run(ent) != NULL; }

/* ------------------------------------------------------------------ actors */

/* actor: who the event belongs to ("!self" in Source's scenes is the actor itself, not the scene) */
static s32 resolve_for(Run *r, const char *name, int depth, s32 actor)
{
	if (!name || !*name || depth > 3)
		return -1;
	if (name[0] == '!') {
		if (!strcasecmp(name, "!player"))
			return 0;
		if (!strcasecmp(name, "!self"))
			return actor >= 0 ? actor : r->ent;
		if (!strcasecmp(name, "!caller"))
			return r->ent;
		if (!strcasecmp(name, "!activator"))
			return r->activator;
		if (!strncasecmp(name, "!target", 7)) {
			char key[16];
			snprintf(key, sizeof(key), "target%s", name + 7);
			return resolve_for(r, Ent_KV(r->ent, key), depth + 1, actor);
		}
		return -1;
	}
	return Ent_FindActor(name);
}

static s32 resolve(Run *r, const char *name, int depth) { return resolve_for(r, name, depth, -1); }

static s32 actor_of(Run *r, u32 key)
{
	for (u32 i = 0; i < r->numActors; i++)
		if (r->actorKey[i] == key)
			return r->actorEnt[i];
	s32 e = resolve(r, pstr(key), 0);
	if (r->numActors < MAX_ACTORS) {
		r->actorKey[r->numActors] = key;
		r->actorEnt[r->numActors] = e;
		r->numActors++;
	}
	return e;
}

static void ent_pos(s32 e, f32 out[3]) { Npc_EntPos(e, out); }

/* ------------------------------------------------------------------ events */

static void start_event(Run *r, u32 i)
{
	const PackSceneEvent *ev = &P->sceneEvents[r->sc->firstEvent + i];
	s32 actor = ev->actor ? actor_of(r, ev->actor) : -1;
	Npc *n = actor > 0 ? Npc_OfEntity(actor) : NULL;
	switch (ev->type) {
	case SEV_SPEAK: {
		f32 len = ev->end - ev->start;
		/* speech comes from the speaker; with no speaker in the map, from the scene entity */
		f32 pos[3];
		ent_pos(actor > 0 ? actor : r->ent, pos);
		if (actor > 0)
			pos[2] += 60;
		f32 gain = Audio_GainAt(pos, SNDLVL_TALKING);
		f32 relays[3][3];
		int nr = Ent_SpeakerRelays(pos, relays, 3);
		for (int k = 0; k < nr; k++) {
			f32 g = Audio_GainAt(relays[k], SNDLVL_TALKING);
			if (g > gain)
				gain = g;
		}
		if (ev->sound >= 0 && (u32)ev->sound < P->numSounds) {
			const PackSound *s = &P->sounds[ev->sound];
			u32 h = Audio_PlaySpeech(s, pos, 1.0f, SNDLVL_DEFAULT);
			for (int k = 0; k < nr; k++)
				Audio_PlaySpeech(s, relays[k], 0.9f, SNDLVL_DEFAULT); /* PA speakers / monitors */
			if (n) {
				n->voice = h;
				n->speakUntil = now + Audio_Duration(s);
				n->speakSound = ev->sound;
				n->speakStart = now;
			}
			if (Audio_Duration(s) > len)
				len = Audio_Duration(s);
		}
		/* like PC: captions only for lines the player can hear */
		if (ev->caption && gain > CAPTION_GAIN)
			Caption_Add(pstr(ev->caption), len + 0.6f);
		break;
	}
	case SEV_SEQUENCE:
		if (n)
			Npc_PlaySequence(n, pstr(ev->p1), 0);
		break;
	case SEV_MOVETO:
		if (n) {
			s32 target = resolve_for(r, pstr(ev->p1), 0, actor);
			if (target >= 0) {
				f32 g[3];
				ent_pos(target, g);
				const char *how = pstr(ev->p2);
				f32 stop = ev->sound > 0 ? (f32)ev->sound : (target == 0 ? 64.0f : 4.0f);
				if (n->locked)
					Npc_ResetSequence(n);
				Npc_MoveTo(n, g, how && !strncasecmp(how, "run", 3), stop);
				if (ev->flags & SEVF_RESUMECONDITION) {
					r->waitEnt = actor;
					r->waitLeft = 10.0f;
				}
			}
		}
		break;
	case SEV_FACE:
		if (n) {
			s32 target = resolve_for(r, pstr(ev->p1), 0, actor);
			if (target >= 0)
				n->faceEnt = target;
		}
		break;
	case SEV_GESTURE:
		/* the gesture plays as a layer over whatever the actor is doing; tick_run times it */
		if (n && r->slot && ev->end > ev->start) {
			s16 seq = Npc_FindSeq(n, pstr(ev->p1));
			if (seq >= 0) {
				r->slot[i] = (s8)Npc_SetGesture(n, -1, seq, 0, 0, 0);
				sceneGestures++;
			}
		}
		break;
	case SEV_LOOKAT:
		if (n) {
			s32 target = resolve_for(r, pstr(ev->p1), 0, actor);
			if (target >= 0 && target != actor) {
				n->lookEnt = target;
				n->sceneLook = 1;
				sceneLooks++;
			}
		}
		break;
	case SEV_FIRETRIGGER: {
		char out[24];
		snprintf(out, sizeof(out), "ontrigger%d", atoi(pstr(ev->p1)));
		Ent_FireOutput(r->ent, out, actor >= 0 ? actor : r->ent);
		break;
	}
	case SEV_SECTION: {
		/* pause; "automate Resume|Cancel <delay>" continues by itself, otherwise wait for Resume */
		r->paused = 1;
		r->autoResume = 0;
		r->autoCancel = 0;
		r->waitActors = 0;
		char verb[16] = "", act[16] = "";
		float delay = 0;
		if (sscanf(pstr(ev->p1), "%15s %15s %f", verb, act, &delay) >= 2 && !strcasecmp(verb, "automate")) {
			r->autoResume = now + delay;
			r->autoCancel = !strcasecmp(act, "cancel");
		} else {
			/* Most sections (114 of the 129 scenes that have them) are never resumed by the map: they sit
			 * right after move-to events ("pause_mossyEntry", "Wait for movement") and hold the scene
			 * until the actors have arrived. Continue once nobody in the scene is walking or talking;
			 * an explicit Resume input still works, and a stuck actor can't stall the scene for good. */
			r->waitActors = 1;
			r->pausedAt = now;
			r->autoResume = now + 30.0f;
		}
		break;
	}
	default:
		break;
	}
}

/* a gesture or look-at event stops: its layer goes away */
static void stop_layer(Run *r, u32 i, Npc *n)
{
	const PackSceneEvent *ev = &P->sceneEvents[r->sc->firstEvent + i];
	if (!n)
		return;
	if (ev->type == SEV_GESTURE && r->slot && r->slot[i] >= 0) {
		n->gest[(int)r->slot[i]].seq = -1;
		r->slot[i] = -1;
	} else if (ev->type == SEV_LOOKAT && n->sceneLook) {
		n->lookEnt = -1;
		n->sceneLook = 0;
	}
}

static void drop_layers(Run *r)
{
	if (!r->sc || !r->state)
		return;
	for (u32 i = 0; i < r->sc->numEvents; i++) {
		if (r->state[i] != ST_RUNNING)
			continue;
		const PackSceneEvent *ev = &P->sceneEvents[r->sc->firstEvent + i];
		s32 actor = ev->actor ? actor_of(r, ev->actor) : -1;
		stop_layer(r, i, actor > 0 ? Npc_OfEntity(actor) : NULL);
	}
}

static void end_event(Run *r, u32 i)
{
	const PackSceneEvent *ev = &P->sceneEvents[r->sc->firstEvent + i];
	s32 actor = ev->actor ? actor_of(r, ev->actor) : -1;
	Npc *n = actor > 0 ? Npc_OfEntity(actor) : NULL;
	if (!n)
		return;
	stop_layer(r, i, n);
	switch (ev->type) {
	case SEV_SEQUENCE:
		if (n->locked && n->script < 0)
			Npc_ResetSequence(n);
		break;
	case SEV_FACE:
		break;
	default:
		break;
	}
}

static void finish(Run *r, int canceled)
{
#ifdef ENT_TRACE
	printf("[%7.2f] SCENE %s: %s (scene time %.1f of %.1f, held for lines %.1f s)\n", now, P->entStrings + r->sc->name,
	       canceled ? "canceled" : "done", r->t, r->sc->duration, r->heldTotal);
#endif
	drop_layers(r);
	for (u32 i = 0; i < r->numActors; i++) {
		Npc *n = r->actorEnt[i] > 0 ? Npc_OfEntity(r->actorEnt[i]) : NULL;
		if (n && n->locked && n->script < 0)
			Npc_ResetSequence(n);
		if (n && canceled && n->voice) {
			Audio_Stop(n->voice);
			n->voice = 0;
		}
	}
	s32 ent = r->ent, act = r->activator;
	release(r);
	Ent_FireOutput(ent, canceled ? "oncanceled" : "oncompletion", act);
}

/* any of the scene's actors (every event's) talking right now */
static int actors_talking(Run *r)
{
	const PackScene *sc = r->sc;
	for (u32 i = 0; i < sc->numEvents; i++) {
		const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
		s32 a = ev->actor ? actor_of(r, ev->actor) : -1;
		const Npc *n = a > 0 ? Npc_OfEntity(a) : NULL;
		if (n && !n->dead && n->speakUntil > now)
			return 1;
	}
	return 0;
}

void Scene_Start(s32 ent, u32 index, s32 activator)
{
	if (!P || index >= P->numScenes)
		return;
	Run *r = find_run(ent);
	if (r) {
		drop_layers(r);
		release(r); /* restart */
	}
	r = find_run(-1);
	if (!r)
		return;
	const PackScene *sc = &P->scenes[index];
	r->ent = ent;
	r->activator = activator;
	r->sc = sc;
	r->t = 0;
	r->waitEnt = -1;
	r->state = calloc(sc->numEvents ? sc->numEvents : 1, 1);
	r->loops = calloc(sc->numEvents ? sc->numEvents : 1, sizeof(s16));
	r->pre = calloc(sc->numEvents ? sc->numEvents : 1, 1);
	r->held = 0;
	r->slot = malloc(sc->numEvents ? sc->numEvents : 1);
	if (r->slot)
		memset(r->slot, 0xFF, sc->numEvents ? sc->numEvents : 1);
	if (!r->state || !r->loops || !r->slot) {
		release(r);
		return;
	}
	for (u32 i = 0; i < sc->numEvents; i++) {
		const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
		r->loops[i] = ev->sound; /* loop count for loop events */
	}
	prefetch_ahead(r);
	/* logic_choreographed_scene "busyactor" 1 (CSceneEntity::CheckActors): if one of its actors is talking
	 * already, it tries again every think until they're quiet; most HL2 scenes are set so */
	const char *ba = Ent_KV(ent, "busyactor");
	if (ba && atoi(ba) == 1 && actors_talking(r)) {
		r->waitStart = 1;
		return;
	}
	Ent_FireOutput(ent, "onstart", activator);
}

void Scene_Cancel(s32 ent)
{
	Run *r = find_run(ent);
	if (r)
		finish(r, 1);
}

void Scene_Pause(s32 ent)
{
	Run *r = find_run(ent);
	if (r) {
		r->paused = 1;
		r->autoResume = 0;
		r->waitActors = 0;
	}
}

void Scene_Resume(s32 ent)
{
	Run *r = find_run(ent);
	if (r) {
		r->paused = 0;
		r->waitActors = 0;
	}
}

/* lines come off the disc in the background a few seconds before they're due (Source compensates for
 * its sound latency the same way). Only a few seconds ahead: the cache is 1 MB, and asking for a
 * long scene's every line at once made the later ones push the first out again before they played. */
#define PREFETCH_AHEAD 6.0f
static void prefetch_ahead(Run *r)
{
	const PackScene *sc = r->sc;
	for (u32 i = 0; i < sc->numEvents; i++) {
		const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
		if (ev->start > r->t + PREFETCH_AHEAD)
			break;
		if (r->pre[i] || r->state[i] != ST_PENDING || ev->type != SEV_SPEAK || ev->sound < 0 || (u32)ev->sound >= P->numSounds)
			continue;
		r->pre[i] = 1;
		Audio_Prefetch(&P->sounds[ev->sound]);
	}
}

static void tick_run(Run *r, f32 dt)
{
	const PackScene *sc = r->sc;
	if (r->waitStart) {
		if (actors_talking(r))
			return;
		r->waitStart = 0;
		Ent_FireOutput(r->ent, "onstart", r->activator);
	}
	if (r->paused && r->waitActors) {
		/* CSceneEntity::PauseThink: a section without "automate" resumes once the events flagged as resume
		 * conditions that are under way have finished (CChoreoScene::CheckEventCompletion): a move-to's
		 * actor arriving, a line being said. With none under way it goes straight on. */
		int busy = 0;
		for (u32 i = 0; i < sc->numEvents && !busy; i++) {
			const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
			if (!(ev->flags & SEVF_RESUMECONDITION) || r->state[i] == ST_PENDING)
				continue;
			s32 a = ev->actor ? actor_of(r, ev->actor) : -1;
			const Npc *n = a > 0 ? Npc_OfEntity(a) : NULL;
			if (!n || n->dead)
				continue;
			if (ev->type == SEV_MOVETO && n->moving)
				busy = 1;
			else if (ev->type == SEV_SPEAK && n->speakUntil > now)
				busy = 1;
		}
		if (!busy) {
#ifdef ENT_TRACE
			if (now - r->pausedAt > 2.0f)
				printf("[%7.2f] SCENE %s: section waited %.1f s for its actors\n", now, P->entStrings + r->sc->name, now - r->pausedAt);
#endif
			r->paused = 0;
			r->waitActors = 0;
		}
	}
	if (r->paused) {
		if (r->autoResume > 0 && now >= r->autoResume) {
#ifdef ENT_TRACE
			if (r->waitActors)
				printf("[%7.2f] SCENE %s: section gave up waiting for its actors (30 s)\n", now, P->entStrings + r->sc->name);
#endif
			if (r->autoCancel) {
				finish(r, 1);
				return;
			}
			r->paused = 0;
		} else {
			return;
		}
	}
	if (r->waitEnt >= 0) {
		Npc *n = Npc_OfEntity(r->waitEnt);
		r->waitLeft -= dt;
		if (!n || !n->moving || r->waitLeft <= 0)
			r->waitEnt = -1;
		else
			return;
	}
	/* a line that's due but still coming off the disc holds the scene's clock (briefly), so
	 * the speech, the mouth and the gestures stay together instead of the voice trailing them */
	int hold = 0;
	for (u32 i = 0; i < sc->numEvents; i++) {
		const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
		if (ev->start > r->t + dt)
			break;
		if (r->state[i] == ST_PENDING && ev->type == SEV_SPEAK && ev->sound >= 0 && (u32)ev->sound < P->numSounds &&
		    !Audio_Ready(&P->sounds[ev->sound])) {
			if (!r->pre[i]) {
				r->pre[i] = 1;
				Audio_Prefetch(&P->sounds[ev->sound]);
			}
			hold = 1;
			break;
		}
	}
	/* (at most a little under half a second: Source's scene clock never waits, and a second at every line
	 * the drive was slow with added up to long silences between lines) */
	if (hold && r->held < 0.4f) {
		r->held += dt;
#ifdef ENT_TRACE
		r->heldTotal += dt;
#endif
		return;
	}
	r->held = 0;
	r->t += dt;
	prefetch_ahead(r);
	for (u32 i = 0; i < sc->numEvents; i++) {
		if (r->state[i] != ST_PENDING)
			continue;
		const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
		if (ev->start > r->t)
			break; /* events are sorted by start time */
		if (ev->type == SEV_LOOP) {
			r->state[i] = ST_DONE;
			if (r->loops[i] != 0) {
				if (r->loops[i] > 0)
					r->loops[i]--;
				f32 to = (f32)atof(pstr(ev->p1));
				r->t = to;
				for (u32 k = 0; k < sc->numEvents; k++) {
					const PackSceneEvent *e2 = &P->sceneEvents[sc->firstEvent + k];
					if (k != i && e2->start >= to && r->state[k] == ST_DONE && e2->type != SEV_LOOP)
						r->state[k] = ST_PENDING;
				}
				return;
			}
			continue;
		}
		r->state[i] = (ev->end > ev->start) ? ST_RUNNING : ST_DONE;
		start_event(r, i);
		if (r->ent < 0)
			return; /* finished from inside an event */
		if (r->paused || r->waitEnt >= 0)
			return;
	}
	/* faces: flex tracks of the running events */
	if (P->eventFlex)
		for (u32 i = 0; i < sc->numEvents; i++) {
			if (r->state[i] != ST_RUNNING)
				continue;
			const PackEventFlex *ef = &P->eventFlex[sc->firstEvent + i];
			if (!ef->numTracks)
				continue;
			const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
			s32 actor = ev->actor ? actor_of(r, ev->actor) : -1;
			Npc *n = actor > 0 ? Npc_OfEntity(actor) : NULL;
			if (!n)
				continue;
			f32 t = r->t - ev->start;
			for (u32 k = 0; k < ef->numTracks; k++) {
				const PackFlexTrack *tr = &P->flexTracks[ef->firstTrack + k];
				if (!tr->numSamples)
					continue;
				const PackFlexSample *sm = &P->flexSamples[tr->firstSample];
				f32 v = sm[0].v;
				if (t >= sm[tr->numSamples - 1].t)
					v = sm[tr->numSamples - 1].v;
				else
					for (u32 j = 0; j + 1 < tr->numSamples; j++)
						if (t >= sm[j].t && t < sm[j + 1].t) {
							f32 span = sm[j + 1].t - sm[j].t;
							f32 a = span > 0 ? (t - sm[j].t) / span : 0;
							v = sm[j].v + (sm[j + 1].v - sm[j].v) * a;
							break;
						}
				Face_SetController(n, pstr(tr->name), tr->min + v * (tr->max - tr->min));
			}
		}
	/* gestures: CChoreoEvent's playback -> original tag mapping gives the sequence's cycle; the layer
	 * fades in and out at the event's ends */
	for (u32 i = 0; i < sc->numEvents; i++) {
		if (r->state[i] != ST_RUNNING || !r->slot || r->slot[i] < 0)
			continue;
		const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
		s32 actor = ev->actor ? actor_of(r, ev->actor) : -1;
		Npc *n = actor > 0 ? Npc_OfEntity(actor) : NULL;
		if (!n)
			continue;
		f32 dur = ev->end - ev->start, el = r->t - ev->start;
		f32 frac = dur > 0 ? el / dur : 1;
		frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
		f32 cyc = frac;
		const PackEventTags *tg = P->eventTags ? &P->eventTags[sc->firstEvent + i] : NULL;
		if (tg && tg->numTags) {
			f32 p0 = 0, o0 = 0;
			cyc = -1;
			for (u32 k = 0; k <= tg->numTags && k <= 4; k++) {
				f32 p1 = k < tg->numTags ? tg->play[k] / 4096.0f : 1, o1 = k < tg->numTags ? tg->orig[k] / 4096.0f : 1;
				if (frac <= p1) {
					cyc = p1 > p0 ? o0 + (o1 - o0) * (frac - p0) / (p1 - p0) : o1;
					break;
				}
				p0 = p1;
				o0 = o1;
			}
			if (cyc < 0)
				cyc = 1;
		}
		f32 fade = dur * 0.25f < 0.2f ? dur * 0.25f : 0.2f, w = 1;
		if (fade > 0) {
			if (el < fade)
				w = el / fade;
			if (ev->end - r->t < fade)
				w = (ev->end - r->t) / fade;
		}
		Npc_SetGesture(n, r->slot[i], n->gest[(int)r->slot[i]].seq, cyc, w < 0 ? 0 : w, el);
	}
	int running = 0;
	for (u32 i = 0; i < sc->numEvents; i++) {
		if (r->state[i] == ST_RUNNING) {
			const PackSceneEvent *ev = &P->sceneEvents[sc->firstEvent + i];
			if (r->t >= ev->end) {
				r->state[i] = ST_DONE;
				end_event(r, i);
			} else {
				running = 1;
			}
		} else if (r->state[i] == ST_PENDING) {
			running = 1;
		}
	}
	if (!running && r->t >= sc->duration)
		finish(r, 0);
}

void Scene_Tick(f32 dt)
{
	now += dt;
	for (int i = 0; i < MAX_RUNS; i++)
		if (runs[i].ent >= 0)
			tick_run(&runs[i], dt);
}

void Scene_NpcSay(Npc *n, s16 sound)
{
	if (!n || sound < 0 || (u32)sound >= P->numSounds)
		return;
	f32 pos[3];
	ent_pos(n->ent, pos);
	pos[2] += 60;
	const PackSound *s = &P->sounds[sound];
	if (n->voice)
		Audio_Stop(n->voice);
	n->voice = Audio_PlaySpeech(s, pos, 1.0f, SNDLVL_DEFAULT);
	n->speakUntil = now + Audio_Duration(s);
	n->speakSound = sound;
	n->speakStart = now;
	u32 cap = P->soundCaptions ? P->soundCaptions[sound] : 0;
	if (cap && Audio_GainAt(pos, SNDLVL_TALKING) > CAPTION_GAIN)
		Caption_Add(pstr(cap), Audio_Duration(s) + 0.6f);
}

int Scene_NpcBusy(const Npc *n)
{
	for (int i = 0; i < MAX_RUNS; i++) {
		const Run *r = &runs[i];
		if (r->ent < 0 || !r->sc)
			continue;
		for (u32 k = 0; k < r->numActors; k++)
			if (r->actorEnt[k] == n->ent)
				return 1;
	}
	return n->speakUntil > now;
}
