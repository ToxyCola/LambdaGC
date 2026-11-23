/*
 * hl2gc: manhacks and city scanners (npc_manhack.cpp, npc_scanner.cpp / npc_basescanner.cpp). They fly by
 * their own rules when no script is moving them: manhacks hunt what they hate and slice it, scanners patrol,
 * come to look at the player and photograph him.
 */
#include "flyers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "collide.h"
#include "ent.h"
#include "npc.h"
#include "relations.h"
#include "weapons.h"

enum { FK_MANHACK = 1, FK_SCANNER, FK_TRACK };
typedef struct {
	s32 ent;
	u8 kind, boom, alive;
	/* FK_TRACK (CAI_TrackPather: npc_helicopter, npc_combinegunship, npc_combinedropship): the path_track it
	 * flies at (cur), the one it passed (last), where it's going (dest); forward along the path; forced (told
	 * where to go), patrolling, farthest (ChooseFarthestPathPoint) */
	s32 cur, last, dest;
	u8 fwd, forced, patrol, patrolBreak, farthest, started;
	f32 maxSpeed, pathMax, accel, tol, nextEnemyCheck, lastGoal[3];
	f32 vel[3], home[3];
	u32 loop;
	f32 nextHit, nextPhoto, flashUntil, speedOverride, distOverride;
	s32 follow, path, inspect;
	f32 bob;
} Flyer;

#define MAX_FLYERS 64
static Flyer fl[MAX_FLYERS];
static int numFl;
static u8 *known; /* per entity: has a Flyer */
static u32 knownN;

static f32 frand(void) { return (f32)(rand() & 0xFFFF) / 65535.0f; }
static Entity *E(s32 i) { return Ent_List(NULL) + i; }

static int kind_of(const char *cn)
{
	if (!strcmp(cn, "npc_manhack"))
		return FK_MANHACK;
	if (!strcmp(cn, "npc_cscanner") || !strcmp(cn, "npc_clawscanner"))
		return FK_SCANNER;
	if (!strcmp(cn, "npc_helicopter") || !strcmp(cn, "npc_combinegunship") || !strcmp(cn, "npc_combinedropship"))
		return FK_TRACK;
	return 0;
}

static u32 play(Flyer *f, int slot, f32 vol)
{
	const Pack *P = Ent_Pack();
	Entity *e = E(f->ent);
	if (P && e->esnd[slot] >= 0 && (u32)e->esnd[slot] < P->numSounds)
		return Audio_Play(&P->sounds[e->esnd[slot]], e->origin, vol, SNDLVL_DEFAULT);
	return 0;
}

void Flyers_Free(void)
{
	for (int i = 0; i < numFl; i++)
		if (fl[i].loop)
			Audio_Stop(fl[i].loop);
	numFl = 0;
	free(known);
	known = NULL;
	knownN = 0;
}

static void add(s32 ent)
{
	if (numFl >= MAX_FLYERS)
		return;
	Flyer *f = &fl[numFl++];
	memset(f, 0, sizeof(*f));
	f->ent = ent;
	f->kind = (u8)kind_of(E(ent)->classname);
	memcpy(f->home, E(ent)->origin, 12);
	f->follow = f->inspect = -1;
	const char *t = Ent_KV(ent, "target");
	f->path = t && *t ? Ent_FindActor(t) : -1;
	f->nextPhoto = Ent_Now() + 3 + frand() * 4;
	f->bob = frand() * 6.28f;
	f->alive = 1;
	f->cur = f->last = f->dest = -1;
	f->fwd = 1;
	if (f->kind == FK_TRACK) {
		const char *cn = E(ent)->classname;
		/* (Spawn: the chopper's "PatrolSpeed" or 60 mph x 17.6; the gunship 1056; the dropship 60 mph x 17.6;
		 * accel 500 / 500 / 300 a second; arrival within 128) */
		const char *ps = Ent_KV(ent, "patrolspeed");
		f->maxSpeed = !strcmp(cn, "npc_helicopter") && ps && atof(ps) > 0 ? (f32)atof(ps) : !strcmp(cn, "npc_combinegunship") ? 1056.0f : 60 * 17.6f;
		f->accel = !strcmp(cn, "npc_combinedropship") ? 300.0f : 500.0f;
		f->tol = 128.0f;
		const char *is = Ent_KV(ent, "initialspeed");
		if (is && atof(is) > 0)
			f->vel[0] = 0; /* (InitialSpeed: the flight's starting speed; it accelerates from there) */
	}
	known[ent] = 1;
}

void Flyers_Spawn(void)
{
	Flyers_Free();
	u32 n;
	Entity *ents = Ent_List(&n);
	known = calloc(n, 1);
	knownN = n;
	for (u32 i = 1; i < n && known; i++)
		if (kind_of(ents[i].classname) && Npc_OfEntity((s32)i))
			add((s32)i);
}

static int alive_ent(s32 t)
{
	if (t == 0)
		return 1;
	if (t < 0 || (E(t)->flags & (EF_KILLED | EF_DISABLED)))
		return 0;
	Npc *n = Npc_OfEntity(t);
	return !n || !n->dead;
}

static void center(s32 t, f32 out[3])
{
	const f32 *o = E(t)->origin;
	out[0] = o[0];
	out[1] = o[1];
	out[2] = o[2] + (t == 0 ? 40 : 32);
}

/* the nearest thing it hates and can see */
static s32 look(Flyer *f, f32 range)
{
	s32 best = -1;
	f32 bestD = range * range;
	const f32 *me = E(f->ent)->origin;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 0; i < n; i++) {
		if (i && (ents[i].npcModel < 0 || (s32)i == f->ent || !alive_ent((s32)i)))
			continue;
		if (i == 0 && Ent_NoTarget())
			continue;
		if (Rel_Disposition(f->ent, (s32)i) != REL_HATE)
			continue;
		f32 c[3];
		center((s32)i, c);
		f32 d[3] = {c[0] - me[0], c[1] - me[1], c[2] - me[2]};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd >= bestD)
			continue;
		Trace tr;
		CM_SetIgnoreEnt(f->ent);
		CM_TraceWorld(&tr, me, c, MASK_SHOT);
		CM_SetIgnoreEnt(-1);
		if (tr.fraction < 0.98f && tr.ent != (s32)i)
			continue;
		best = (s32)i;
		bestD = dd;
	}
	return best;
}

/* fly toward a point: accelerate to it, slide along what's in the way */
static void fly_to(Flyer *f, Npc *n, const f32 goal[3], f32 speed, f32 accel, f32 dt)
{
	Entity *e = E(f->ent);
	f32 d[3] = {goal[0] - e->origin[0], goal[1] - e->origin[1], goal[2] - e->origin[2]};
	f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	f32 want = l < 1 ? 0 : speed * (l < 64 ? l / 64 : 1);
	for (int k = 0; k < 3; k++) {
		f32 tv = l > 1e-3f ? d[k] / l * want : 0;
		f32 a = accel * dt;
		f32 dv = tv - f->vel[k];
		f->vel[k] += dv > a ? a : dv < -a ? -a : dv;
	}
	f32 to[3] = {e->origin[0] + f->vel[0] * dt, e->origin[1] + f->vel[1] * dt, e->origin[2] + f->vel[2] * dt};
	f32 mn[3] = {-10, -10, -10}, mx[3] = {10, 10, 10};
	Trace tr;
	CM_SetIgnoreEnt(f->ent);
	CM_SetSkipBoxes(1);
	CM_BoxTrace(&tr, e->origin, to, mn, mx, MASK_PLAYERSOLID);
	CM_SetSkipBoxes(0);
	CM_SetIgnoreEnt(-1);
	if (!tr.startsolid)
		memcpy(e->origin, tr.endpos, 12);
	if (tr.fraction < 1) {
		/* bounce off the wall (manhacks scrape along it) */
		f32 vn = f->vel[0] * tr.normal[0] + f->vel[1] * tr.normal[1] + f->vel[2] * tr.normal[2];
		for (int k = 0; k < 3; k++)
			f->vel[k] -= tr.normal[k] * vn * 1.5f;
	}
	f32 hv = sqrtf(f->vel[0] * f->vel[0] + f->vel[1] * f->vel[1]);
	if (hv > 20)
		n->idealYaw = n->yaw = atan2f(f->vel[1], f->vel[0]) * 180.0f / (f32)M_PI;
	e->angles[1] = n->yaw;
	Ent_AngleMatrix(e->angles, e->R);
}

static void hit(Flyer *f, s32 t, const f32 at[3])
{
	/* the blades: sk_manhack_melee_dmg (20 on Normal), then it bounces off */
	if (t == 0)
		Ent_HurtPlayer(20, DMG_SLASH, at);
	else {
		f32 d[3] = {f->vel[0], f->vel[1], f->vel[2]};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
		for (int k = 0; k < 3; k++)
			d[k] /= l;
		Ent_Damage(t, 20, d, at);
	}
	play(f, 1, 1.0f);
	Weap_Sparks(at, NULL, 1, 1);
	for (int k = 0; k < 3; k++)
		f->vel[k] = -f->vel[k] * 0.6f + (frand() - 0.5f) * 120;
	f->nextHit = Ent_Now() + 0.6f;
}

static void manhack(Flyer *f, Npc *n, f32 dt, f32 now)
{
	s32 t = look(f, 1500);
	Entity *e = E(f->ent);
	if (t < 0) {
		/* nothing to hunt: hang about where it is, bobbing */
		f->bob += dt * 2;
		f32 g[3] = {f->home[0] + cosf(f->bob * 0.7f) * 24, f->home[1] + sinf(f->bob * 0.5f) * 24, f->home[2] + sinf(f->bob) * 8};
		fly_to(f, n, g, 120, 300, dt);
		return;
	}
	f32 c[3];
	center(t, c);
	if (now < f->nextHit) {
		/* backing off after a hit, then it comes again */
		fly_to(f, n, c, 0, 200, dt);
		return;
	}
	f32 sp = f->speedOverride > 0 ? f->speedOverride : 350;
	fly_to(f, n, c, sp, 900, dt);
	f32 d[3] = {c[0] - e->origin[0], c[1] - e->origin[1], c[2] - e->origin[2]};
	if (d[0] * d[0] + d[1] * d[1] < 26 * 26 && fabsf(d[2]) < 40)
		hit(f, t, e->origin);
}

static void scanner(Flyer *f, Npc *n, f32 dt, f32 now)
{
	Entity *e = E(f->ent);
	f32 sp = f->speedOverride > 0 ? f->speedOverride : 150; /* SCANNER_SCOUT_MAX_SPEED */
	s32 t = f->inspect >= 0 ? f->inspect : f->follow >= 0 ? f->follow : -1;
	if (t < 0 && !Ent_NoTarget()) {
		/* CNPC_CScanner: with nothing else to do it goes to inspect the player when it can see him, whatever
		 * it thinks of him (that's the city scanner's job) */
		f32 pc[3];
		center(0, pc);
		f32 d[3] = {pc[0] - e->origin[0], pc[1] - e->origin[1], pc[2] - e->origin[2]};
		if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1024.0f * 1024.0f) {
			Trace tr;
			CM_SetIgnoreEnt(f->ent);
			CM_TraceWorld(&tr, e->origin, pc, MASK_SHOT);
			CM_SetIgnoreEnt(-1);
			if (tr.fraction >= 0.98f)
				t = 0;
		}
	}
	if (t >= 0 && alive_ent(t)) {
		/* hover in front of them, over head height, at photographing distance */
		f32 c[3];
		center(t, c);
		f32 d[3] = {e->origin[0] - c[0], e->origin[1] - c[1], 0};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1]) + 1e-3f;
		f32 dist = f->distOverride > 0 ? f->distOverride : 128; /* SCANNER_PHOTO_FAR_DIST */
		f32 g[3] = {c[0] + d[0] / l * dist, c[1] + d[1] / l * dist, c[2] + 72 - 40 + sinf(f->bob) * 6};
		f->bob += dt;
		fly_to(f, n, g, sp, 300, dt);
		n->idealYaw = n->yaw = atan2f(c[1] - e->origin[1], c[0] - e->origin[0]) * 180.0f / (f32)M_PI;
		/* take its picture: the flash, the shutter, OnPhotographPlayer / OnPhotographNPC */
		f32 dd = sqrtf((c[0] - e->origin[0]) * (c[0] - e->origin[0]) + (c[1] - e->origin[1]) * (c[1] - e->origin[1]) +
		               (c[2] - e->origin[2]) * (c[2] - e->origin[2]));
		if (now >= f->nextPhoto && dd < 400) { /* (its flash reaches much further: SCANNER_FLASH_MAX_DIST) */
			f->nextPhoto = now + 4 + frand() * 4;
			f->flashUntil = now + 0.15f;
			play(f, 1, 1.0f);
			if (t == 0) {
				/* BlindFlashTarget: white, by how squarely he's looking at it, if nothing's in between */
				f32 eye[3], fwd[3];
				Ent_PlayerView(eye, fwd);
				f32 fd[3] = {e->origin[0] - eye[0], e->origin[1] - eye[1], e->origin[2] - eye[2]};
				f32 fl = sqrtf(fd[0] * fd[0] + fd[1] * fd[1] + fd[2] * fd[2]) + 1e-3f;
				f32 dot = (fd[0] * fwd[0] + fd[1] * fwd[1] + fd[2] * fwd[2]) / fl;
				Trace tr;
				CM_SetIgnoreEnt(f->ent);
				CM_TraceWorld(&tr, e->origin, eye, MASK_PLAYERSOLID);
				CM_SetIgnoreEnt(-1);
				if (dot > 0.5f && !tr.startsolid && tr.fraction >= 1.0f)
					Ent_FlashScreen(255, 255, 255, 240.0f * dot / 255.0f, 3.0f);
			}
			Ent_FireOutput(f->ent, t == 0 ? "onphotographplayer" : "onphotographnpc", t);
			if (f->inspect == t)
				f->inspect = -1; /* (InspectTargetPhoto: one picture) */
		}
		return;
	}
	/* patrol its path_corners, else hover where it is */
	if (f->path > 0 && !(E(f->path)->flags & EF_KILLED)) {
		f32 g[3];
		memcpy(g, E(f->path)->origin, 12);
		fly_to(f, n, g, sp, 300, dt);
		f32 d[3] = {g[0] - e->origin[0], g[1] - e->origin[1], g[2] - e->origin[2]};
		if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 48 * 48) {
			Ent_FireOutput(f->path, "onpass", f->ent);
			const char *nx = Ent_KV(f->path, "target");
			s32 nxt = nx && *nx ? Ent_FindActor(nx) : -1;
			f->path = nxt > 0 ? nxt : -1;
			if (f->path < 0)
				memcpy(f->home, e->origin, 12);
		}
		return;
	}
	f->bob += dt;
	f32 g[3] = {f->home[0], f->home[1], f->home[2] + sinf(f->bob) * 6};
	fly_to(f, n, g, 60, 150, dt);
}

/* ---- CAI_TrackPather */
static int on_same_track(s32 a, s32 b)
{
	if (a == b)
		return 1;
	for (int dir = 0; dir < 2; dir++) {
		s32 p = Ent_PathNext(a, dir);
		for (int g = 0; p > 0 && p != a && g < 512; g++) {
			if (p == b)
				return 1;
			p = Ent_PathNext(p, dir);
		}
	}
	return 0;
}

static f32 dist3(const f32 *a, const f32 *b)
{
	f32 d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
	return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/* ComputePathDistance: along the path from p to dest (forward or back), or a huge number */
static f32 path_dist(s32 p, s32 dest, int forward)
{
	f32 d = 0;
	s32 last = p;
	for (int g = 0; p > 0 && g < 512; g++) {
		d += dist3(E(last)->origin, E(p)->origin);
		if (p == dest)
			return d;
		last = p;
		p = Ent_PathNext(p, !forward);
	}
	return 1e30f;
}

static int forward_along(s32 p, s32 test) { return path_dist(p, test, 1) <= path_dist(p, test, 0); }

/* BestPointOnPath: the track point on p's path nearest (or farthest) to pos, keeping avoid away from it */
static s32 best_point(s32 p, const f32 pos[3], f32 avoid, int farthest)
{
	s32 best = -1;
	f32 bd = farthest ? -1 : 1e30f;
	for (int dir = 0; dir < 2; dir++) {
		s32 t = p;
		for (int g = 0; t > 0 && g < 512; g++) {
			f32 d = dist3(E(t)->origin, pos);
			if (d >= avoid && (farthest ? d > bd : d < bd)) {
				bd = d;
				best = t;
			}
			t = Ent_PathNext(t, dir == 0);
			if (t == p)
				break;
		}
	}
	return best;
}

static void track_new_current(Flyer *f, s32 t)
{
	f->cur = t;
}

/* MoveToTrackPoint (FlyToSpecificTrackViaPath): there by way of the path */
static void track_fly_to(Flyer *f, s32 t)
{
	if (t <= 0)
		return;
	if (f->dest > 0 && on_same_track(t, f->dest) && f->cur > 0) {
		f->dest = t;
		f->fwd = (u8)forward_along(f->cur, t);
		f->forced = 1;
	} else {
		s32 c = best_point(t, E(f->ent)->origin, 0, 0);
		if (c <= 0)
			return;
		track_new_current(f, c);
		f->dest = t;
		f->fwd = (u8)forward_along(c, t);
		f->forced = 1;
	}
}

/* MoveToClosestTrackPoint (SetTrack): onto that path at its nearest point, and stay */
static void track_set(Flyer *f, s32 t)
{
	if (t <= 0 || (f->dest > 0 && on_same_track(t, f->dest)))
		return;
	s32 c = best_point(t, E(f->ent)->origin, 0, 0);
	if (c <= 0)
		return;
	track_new_current(f, c);
	f->dest = c;
	f->fwd = 1;
}

/* SelectNewDestTarget: patrolling turns round at the ends (the fixed logic) */
static void track_patrol_dest(Flyer *f)
{
	if (!f->patrol || f->dest <= 0)
		return;
	s32 old = f->dest;
	int circular = 0;
	s32 t = f->dest;
	for (int g = 0; t > 0 && g < 512; g++) {
		t = Ent_PathNext(t, !f->fwd);
		if (t == f->dest) {
			circular = 1;
			break;
		}
	}
	if (circular || Ent_PathNext(f->dest, !f->fwd) <= 0)
		f->fwd = !f->fwd;
	for (int g = 0; g < 512; g++) {
		s32 nx = Ent_PathNext(f->dest, !f->fwd);
		if (nx <= 0 || nx == old)
			break;
		f->dest = nx;
	}
}

static void track_tick(Flyer *f, Npc *n, f32 dt, f32 now)
{
	Entity *e = E(f->ent);
	if (!f->started) {
		/* (UpdateTrackNavigation: no spawn method, so the "target" key is flown to on the first update) */
		f->started = 1;
		const char *t = Ent_KV(f->ent, "target");
		if (t && *t && f->dest <= 0) {
			track_fly_to(f, Ent_FindActor(t));
			f->forced = 0;
		}
	}
	if (f->cur <= 0) {
		/* nowhere to go: hover */
		f->bob += dt;
		f32 g[3] = {f->home[0], f->home[1], f->home[2] + sinf(f->bob) * 6};
		fly_to(f, n, g, 60, 150, dt);
		return;
	}
	/* UpdateTargetPosition: chase the enemy along the path (not when told where to go, or patrolling) */
	if ((!f->forced || f->patrolBreak) && (!f->patrol || f->patrolBreak) && now >= f->nextEnemyCheck &&
	    !Ent_NoTarget() && Rel_Disposition(f->ent, 0) == REL_HATE) {
		f32 dist;
		const f32 *pp = E(0)->origin;
		if (Ent_PlayerVisible(e->origin, &dist) && dist3(pp, f->lastGoal) >= 64.0f) {
			f->patrol = 0;
			s32 d = best_point(f->cur, pp, 0, f->farthest);
			if (d > 0 && d != f->dest) {
				int fw = forward_along(f->cur, d);
				if (fw != f->fwd) {
					f->fwd = (u8)fw;
					if (d != f->cur) {
						s32 nx = Ent_PathNext(f->cur, !f->fwd);
						if (nx > 0)
							track_new_current(f, nx);
					}
				}
				f->dest = d;
			}
			memcpy(f->lastGoal, pp, 12);
			f->nextEnemyCheck = now + 1.0f;
		}
	}
	/* UpdateCurrentTarget: at the track point: InPass, then on along the path to the destination */
	if (dist3(e->origin, E(f->cur)->origin) <= f->tol) {
		if (f->cur != f->last) {
			const char *sp = Ent_KV(f->cur, "speed");
			f->pathMax = sp ? (f32)atof(sp) : 0;
			f->last = f->cur;
			Ent_FireOutput(f->cur, "onpass", f->ent);
		}
		if (f->cur == f->dest) {
			f->forced = 0;
			track_patrol_dest(f);
		}
		if (f->cur != f->dest) {
			s32 nx = Ent_PathNext(f->cur, !f->fwd);
			f->cur = nx > 0 ? nx : f->last;
		} else
			f->fwd = 1;
	}
	/* fly at it: full speed (the track's speed if it sets one) through the points, slowing for the last */
	f32 speed = f->maxSpeed;
	if (f->pathMax > 0 && f->pathMax < speed)
		speed = f->pathMax;
	const f32 *g = E(f->cur)->origin;
	f32 d[3] = {g[0] - e->origin[0], g[1] - e->origin[1], g[2] - e->origin[2]};
	f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (f->cur == f->dest) {
		f32 stop = sqrtf(2 * f->accel * (l > 0 ? l : 0)); /* (brakes to stop on it) */
		if (stop < speed)
			speed = stop;
	}
	for (int k = 0; k < 3; k++) {
		f32 tv = l > 1e-3f ? d[k] / l * speed : 0;
		f32 a = f->accel * dt, dv = tv - f->vel[k];
		f->vel[k] += dv > a ? a : dv < -a ? -a : dv;
		e->origin[k] += f->vel[k] * dt;
	}
	f32 hv = sqrtf(f->vel[0] * f->vel[0] + f->vel[1] * f->vel[1]);
	if (hv > 40) {
		/* turn toward where it's flying, at most 60 degrees a second (CBaseHelicopter::Flight's yaw rate) */
		f32 want = atan2f(f->vel[1], f->vel[0]) * 180.0f / (f32)M_PI;
		f32 dy = fmodf(want - n->yaw + 540.0f, 360.0f) - 180.0f, mx = 60.0f * dt;
		n->yaw += dy > mx ? mx : dy < -mx ? -mx : dy;
		n->idealYaw = n->yaw;
	}
	e->angles[1] = n->yaw;
	Ent_AngleMatrix(e->angles, e->R);
}

void Flyers_Tick(f32 dt)
{
	if (!known)
		return;
	u32 n;
	Entity *ents = Ent_List(&n);
	/* (ones that came out of a maker) */
	for (u32 i = 1; i < n && i < knownN; i++)
		if (!known[i] && !(ents[i].flags & EF_KILLED) && kind_of(ents[i].classname) && Npc_OfEntity((s32)i))
			add((s32)i);
	f32 now = Ent_Now();
	for (int i = 0; i < numFl; i++) {
		Flyer *f = &fl[i];
		if (!f->alive)
			continue;
		Entity *e = E(f->ent);
		Npc *np = Npc_OfEntity(f->ent);
		if (np && np->dead && !f->boom) {
			/* destroyed: CNPC_BaseScanner::Gib (the manhack likewise): sparks, a light, an explosion
			 * (ExplosionCreate, magnitude 64) covering the pieces, and it's gone (no corpse) */
			f->boom = 1;
			if (f->loop)
				Audio_Stop(f->loop);
			f->loop = 0;
			play(f, 2, 1.0f);
			for (int k = 0; k < 4; k++) {
				f32 sp[3] = {e->origin[0] + (frand() - 0.5f) * 24, e->origin[1] + (frand() - 0.5f) * 24,
				             e->origin[2] + (frand() - 0.5f) * 24};
				Weap_Sparks(sp, NULL, 1, 1);
			}
			Weap_Explosion(e->origin, 0.32f, 0);
			e->flags |= EF_KILLED;
		}
		if (!np || np->dead || (e->flags & EF_KILLED)) {
			if (f->loop)
				Audio_Stop(f->loop);
			f->loop = 0;
			f->alive = 0;
			continue;
		}
		if (e->flags & EF_DISABLED)
			continue;
		/* the engine / fly loop goes with it */
		if (f->kind != FK_TRACK) { /* (the big ones' rotor sounds are their NPC sounds) */
			if (!f->loop || !Audio_IsPlaying(f->loop))
				f->loop = play(f, 0, f->kind == FK_MANHACK ? 0.55f : 0.8f);
			else
				Audio_SetPos(f->loop, e->origin);
		}
		/* a script moving it (scripted_sequence, aiscripted_schedule): leave it to that */
		if (np->moving || np->script >= 0 || e->parent > 0) {
			memcpy(f->home, e->origin, 12);
			continue;
		}
		if (f->kind == FK_MANHACK)
			manhack(f, np, dt, now);
		else if (f->kind == FK_TRACK)
			track_tick(f, np, dt, now);
		else
			scanner(f, np, dt, now);
	}
}

int Flyers_Input(s32 ent, const char *in, const char *param)
{
	for (int i = 0; i < numFl; i++) {
		Flyer *f = &fl[i];
		if (f->ent != ent)
			continue;
		if (f->kind == FK_TRACK) {
			/* (CAI_TrackPather's and CBaseHelicopter's inputs) */
			if (!strcasecmp(in, "flytospecifictrackviapath") || !strcasecmp(in, "flytopathtrack")) {
				f->started = 1;
				f->patrol = 0;
				track_fly_to(f, Ent_FindActor(param));
			} else if (!strcasecmp(in, "settrack")) {
				f->started = 1;
				track_set(f, Ent_FindActor(param));
			} else if (!strcasecmp(in, "startpatrol") || !strcasecmp(in, "startpatrolbreakable")) {
				f->patrol = 1;
				f->patrolBreak = !strcasecmp(in, "startpatrolbreakable");
				if (f->cur == f->dest)
					track_patrol_dest(f);
			} else if (!strcasecmp(in, "stoppatrol")) {
				f->patrol = 0;
			} else if (!strcasecmp(in, "startbreakablemovement")) {
				f->patrolBreak = 1;
			} else if (!strcasecmp(in, "stopbreakablemovement")) {
				f->patrolBreak = 0;
			} else if (!strcasecmp(in, "choosefarthestpathpoint")) {
				f->farthest = 1;
			} else if (!strcasecmp(in, "choosenearestpathpoint")) {
				f->farthest = 0;
			} else if (!strcasecmp(in, "movespecifiedspeed")) {
				f->maxSpeed = (f32)atof(param);
			} else if (!strcasecmp(in, "movetopspeed")) {
				f->maxSpeed = 60 * 17.6f;
				f32 l = sqrtf(f->vel[0] * f->vel[0] + f->vel[1] * f->vel[1] + f->vel[2] * f->vel[2]);
				if (l > 1 && f->cur > 0) /* (at top speed at once) */
					for (int k = 0; k < 3; k++)
						f->vel[k] *= f->maxSpeed / l;
			} else if (!strcasecmp(in, "setangles")) {
				f32 a[3] = {0, 0, 0};
				sscanf(param, "%f %f %f", &a[0], &a[1], &a[2]);
				Npc *np = Npc_OfEntity(ent);
				if (np)
					np->yaw = np->idealYaw = a[1];
			} else if (!strcasecmp(in, "activate") || !strcasecmp(in, "gunon") || !strcasecmp(in, "gunoff") ||
			           !strcasecmp(in, "missileon") || !strcasecmp(in, "missileoff") ||
			           !strcasecmp(in, "enablerotorwash") || !strcasecmp(in, "disablerotorwash") ||
			           !strcasecmp(in, "enablerotorsound") || !strcasecmp(in, "disablerotorsound")) {
				; /* (understood) */
			} else
				return 0;
			return 1;
		}
		if (!strcasecmp(in, "setfollowtarget")) {
			f->follow = param && *param ? Ent_FindActor(param) : -1;
		} else if (!strcasecmp(in, "clearfollowtarget")) {
			f->follow = -1;
		} else if (!strcasecmp(in, "inspecttargetphoto") || !strcasecmp(in, "inspecttargetspotlight")) {
			f->inspect = param && *param ? Ent_FindActor(param) : -1;
			f->nextPhoto = Ent_Now() + 1.5f;
		} else if (!strcasecmp(in, "setflightspeed")) {
			f->speedOverride = (f32)atof(param);
		} else if (!strcasecmp(in, "setdistanceoverride")) {
			f->distOverride = (f32)atof(param);
		} else if (!strcasecmp(in, "disablespotlight") || !strcasecmp(in, "inputshouldinspect") ||
		           !strcasecmp(in, "disableswarm") || !strcasecmp(in, "unpack") || !strcasecmp(in, "equipmine") ||
		           !strcasecmp(in, "deploymine")) {
			; /* (understood; nothing more to do here) */
		} else {
			return 0;
		}
		return 1;
	}
	return 0;
}

int Flyers_Count(void) { return numFl; }

int Flyers_Glow(int i, f32 pos[3], u32 *rgba, f32 *size)
{
	if (i < 0 || i >= numFl || !fl[i].alive || fl[i].boom)
		return 0;
	Flyer *f = &fl[i];
	Entity *e = E(f->ent);
	if (e->flags & (EF_KILLED | EF_DISABLED))
		return 0;
	memcpy(pos, e->origin, 12);
	if (f->kind == FK_SCANNER && f->flashUntil > Ent_Now()) {
		*rgba = 0xFFFFFFFFu; /* the photo's flash */
		*size = 40;
		return 1;
	}
	/* the manhack's eye (sprites/glow1, red), the scanner's light */
	*rgba = f->kind == FK_MANHACK ? 0xFF2010C0u : 0xFFFFFF60u;
	*size = f->kind == FK_MANHACK ? 4 : 6;
	return 1;
}

int Flyers_Probe(s32 ent, f32 vel[3])
{
	for (int i = 0; i < numFl; i++)
		if (fl[i].ent == ent) {
			memcpy(vel, fl[i].vel, 12);
			return fl[i].kind;
		}
	return 0;
}
