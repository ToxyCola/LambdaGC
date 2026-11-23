#include "ent.h"
#include "envents.h"
#include "relations.h"
#include "aients.h"
#include "sentries.h"
#include "flyers.h"
#include "mines.h"
#include "tanks.h"
#include "objects.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "collide.h"
#include "npc.h"
#include "physics.h"
#include "face.h"
#include "text.h"
#include "scene.h"
#include "weapons.h"
#include "gibs.h"
#include "vehicle.h"
#include "light.h"

#define MAX_EVENTS 512
#define USE_RANGE 80.0f /* Source: PLAYER_USE_RADIUS */

typedef struct {
	f32 time;
	s32 activator, caller;
	char target[64];
	char input[40];
	char param[64];
} Event;

static const Pack *P;
static Player *player;
static EntCallbacks cbs;
static Entity *ents;
static u32 numEnts;
static u16 *kvFired; /* how many times each output (by key/value index) has fired */
static Event *events;
static u32 numEvents;
static f32 now;
static CMEnt *solids;
static u32 numSolids;

/* ------------------------------------------------------------------ helpers */

static const char *str(u32 off) { return P->entStrings + off; }

static const char *kv(const Entity *e, const char *key)
{
	for (u32 i = 0; i < e->numKV; i++) {
		const PackKV *k = &P->kvs[e->firstKV + i];
		if (strcmp(str(k->key), key) == 0)
			return str(k->val);
	}
	return NULL;
}

static f32 kvf(const Entity *e, const char *key, f32 def)
{
	const char *v = kv(e, key);
	return v && *v ? (f32)atof(v) : def;
}

static void kvvec(const Entity *e, const char *key, f32 out[3])
{
	out[0] = out[1] = out[2] = 0;
	const char *v = kv(e, key);
	if (v)
		sscanf(v, "%f %f %f", &out[0], &out[1], &out[2]);
}

void Ent_AngleMatrix(const f32 a[3], f32 R[3][3])
{
	const f32 d = (f32)M_PI / 180.0f;
	f32 sp = sinf(a[0] * d), cp = cosf(a[0] * d);
	f32 sy = sinf(a[1] * d), cy = cosf(a[1] * d);
	f32 sr = sinf(a[2] * d), cr = cosf(a[2] * d);
	R[0][0] = cp * cy;
	R[1][0] = cp * sy;
	R[2][0] = -sp;
	R[0][1] = sr * sp * cy - cr * sy;
	R[1][1] = sr * sp * sy + cr * cy;
	R[2][1] = sr * cp;
	R[0][2] = cr * sp * cy + sr * sy;
	R[1][2] = cr * sp * sy - sr * cy;
	R[2][2] = cr * cp;
}

static void update_matrix(Entity *e) { Ent_AngleMatrix(e->angles, e->R); }

/* how far riding moved the player this tick (main.c keeps the view's interpolation relative to the ride) */
static f32 playerCarry[3];

/* ---- parenting */

static int slides(const Entity *e)
{
	return e->cls == EC_FUNC_DOOR || e->cls == EC_FUNC_MOVELINEAR || e->cls == EC_FUNC_BUTTON;
}

static void set_parent(Entity *e, Entity *parent)
{
	if (!parent || parent == e) {
		e->parent = -1;
		return;
	}
	e->parent = (s32)(parent - ents);
	/* remember where it sits in the parent's frame (a sliding door: where it is when closed, and its travel) */
	const f32 *at = slides(e) ? e->pos1 : e->origin;
	f32 d[3] = {at[0] - parent->origin[0], at[1] - parent->origin[1], at[2] - parent->origin[2]};
	f32 mv[3] = {e->pos2[0] - e->pos1[0], e->pos2[1] - e->pos1[1], e->pos2[2] - e->pos1[2]};
	for (int k = 0; k < 3; k++) {
		e->localOrigin[k] = parent->R[0][k] * d[0] + parent->R[1][k] * d[1] + parent->R[2][k] * d[2];
		e->localMove[k] = slides(e) ? parent->R[0][k] * mv[0] + parent->R[1][k] * mv[1] + parent->R[2][k] * mv[2] : 0;
	}
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			e->localR[r][c] = parent->R[0][r] * e->R[0][c] + parent->R[1][r] * e->R[1][c] + parent->R[2][r] * e->R[2][c];
}

static void angles_from_R(const f32 R[3][3], f32 a[3])
{
	f32 xy = sqrtf(R[0][0] * R[0][0] + R[1][0] * R[1][0]);
	const f32 k = 180.0f / (f32)M_PI;
	if (xy > 0.001f) {
		a[1] = atan2f(R[1][0], R[0][0]) * k;
		a[0] = atan2f(-R[2][0], xy) * k;
		a[2] = atan2f(R[2][1], R[2][2]) * k;
	} else {
		a[1] = atan2f(-R[0][1], R[1][1]) * k;
		a[0] = atan2f(-R[2][0], xy) * k;
		a[2] = 0;
	}
}

/* func_rotating: spins about z (yaw), or x/y with spawnflags 4/8, at maxspeed degrees a second */
/* ---- momentary_rot_button (CMomentaryRotButton): a wheel that turns while use is held, between "start"
 * (position 0) and "end" (1), firing Position as it goes; SetPosition turns it to a position (a door
 * driven by the wheel's Position output). ang1/ang2: start/end angles; frac: where it is; value: where
 * SetPosition sends it; minv: direction; interval: speed, maxv: return speed (degrees/s); moveLen: its
 * distance; phase: 0 still, 1 being turned, 2 going to a position, 3 turning back; latch bit 0: used
 * this press; compare: the last Position sent. */
enum { MR_IDLE, MR_USED, MR_SETPOS, MR_RETURN };
static void fire_output_value(Entity *e, const char *output, s32 activator, const char *value);
static void fire_output(Entity *e, const char *output, s32 activator);

static void momentary_setup(Entity *e)
{
	int axis = (e->spawnflags & 64) ? 2 : (e->spawnflags & 128) ? 0 : 1;
	f32 dist = kvf(e, "distance", 0), sign = 1;
	if (dist < 0) {
		sign = -1;
		dist = -dist;
	}
	f32 sp = kvf(e, "startposition", 0);
	sp = sp < 0 ? 0 : sp > 1 ? 1 : sp;
	memcpy(e->ang1, e->angles, 12);
	memcpy(e->ang2, e->angles, 12);
	e->ang1[axis] = e->angles[axis] - sign * dist * sp;
	e->ang2[axis] = e->angles[axis] + sign * dist * (1 - sp);
	e->moveLen = dist > 0.01f ? dist : 0.01f;
	e->interval = kvf(e, "speed", 0);
	if (e->interval == 0)
		e->interval = 100;
	e->maxv = kvf(e, "returnspeed", 0);
	f32 d = kvf(e, "startdirection", 1);
	e->minv = (d == 1 || d == -1) ? d : 1;
	if (sp == 0)
		e->minv = -1;
	else if (sp == 1)
		e->minv = 1;
	e->frac = sp;
	e->value = sp;
	e->compare = sp;
	e->phase = MR_IDLE;
	e->latch = 0;
	e->locked = (e->spawnflags & 2048) != 0;
	e->enabled = 1;
}

static void momentary_apply(Entity *e)
{
	for (int k = 0; k < 3; k++)
		e->angles[k] = e->ang1[k] + (e->ang2[k] - e->ang1[k]) * e->frac;
	update_matrix(e);
}

/* (UpdateTarget: Position, when it changed) */
static void momentary_position(Entity *e, s32 activator)
{
	if (e->compare == e->frac)
		return;
	e->compare = e->frac;
	char v[24];
	snprintf(v, sizeof(v), "%g", e->frac);
	fire_output_value(e, "position", activator, v);
}

void Ent_UseHeld(s32 ent, s32 activator)
{
	if (ent <= 0 || (u32)ent >= numEnts)
		return;
	Entity *e = &ents[ent];
	if (e->cls != EC_MOMENTARY || !e->enabled || (e->flags & EF_KILLED))
		return;
	if (e->locked) {
		if (!(e->latch & 2)) /* (once a press) */
			fire_output(e, "onuselocked", activator);
		e->latch |= 2;
		e->nextThink = now + 0.1f;
		return;
	}
	if (!(e->latch & 1)) {
		e->minv = -e->minv;
		fire_output(e, "onpressed", activator);
		if (e->esnd[0] >= 0 && (u32)e->esnd[0] < P->numSounds)
			Audio_Play(&P->sounds[e->esnd[0]], e->origin, 1.0f, SNDLVL_DEFAULT);
	}
	e->latch |= 1;
	/* (UpdateSelf: at an end it stops there and says so) */
	e->nextThink = now + 0.1f;
	if (e->minv > 0 && e->frac >= 1) {
		e->frac = 1;
		momentary_apply(e);
		fire_output(e, "onfullyclosed", ent);
	} else if (e->minv < 0 && e->frac <= 0) {
		e->frac = 0;
		momentary_apply(e);
		fire_output(e, "onfullyopen", ent);
	}
	e->phase = MR_USED;
}

static void momentary_tick(Entity *e, f32 dt)
{
	s32 self = (s32)(e - ents);
	switch (e->phase) {
	case MR_USED:
		e->frac += e->minv * e->interval / e->moveLen * dt;
		e->frac = e->frac < 0 ? 0 : e->frac > 1 ? 1 : e->frac;
		momentary_apply(e);
		momentary_position(e, 0);
		if (now >= e->nextThink) { /* (UseMoveDone: let go) */
			fire_output(e, "onunpressed", 0);
			e->latch = 0;
			if (!(e->spawnflags & 32) && e->maxv > 0) { /* (not "toggle": it turns back) */
				e->minv = -1;
				e->phase = MR_RETURN;
			} else
				e->phase = MR_IDLE;
		}
		break;
	case MR_SETPOS: {
		f32 step = e->interval / e->moveLen * dt, d = e->value - e->frac;
		if (fabsf(d) <= step) {
			e->frac = e->value;
			e->phase = MR_IDLE;
			momentary_apply(e);
			momentary_position(e, self);
			/* (OutputMovementComplete) */
			if (e->value == 1)
				fire_output(e, "onfullyclosed", self);
			else if (e->value == 0)
				fire_output(e, "onfullyopen", self);
			fire_output(e, "onreachedposition", self);
		} else {
			e->frac += d > 0 ? step : -step;
			momentary_apply(e);
			momentary_position(e, 0);
		}
		break;
	}
	case MR_RETURN:
		e->frac -= e->maxv / e->moveLen * dt;
		if (e->frac <= 0) {
			e->frac = 0;
			e->phase = MR_IDLE;
		}
		momentary_apply(e);
		momentary_position(e, 0);
		break;
	default:
		if (e->latch & 2 && now >= e->nextThink)
			e->latch &= ~2;
		break;
	}
}

/* CFuncRotating: speed is signed (the sign is the direction, "reverse" flag 2 flips the axis); value is
 * the target speed; with flag 16 (SF_BRUSH_ACCDCC) it spins up by 0.2*max*friction and down by
 * 0.1*max*friction every 0.1 s (SpinUpMove/SpinDownMove), reversing goes through zero; without it the
 * speed changes at once. latch bit 1: StopAtStartPos (stops at its start angles); interval: the spin
 * think clock; ang1: the start angles. */
static int rotating_axis(const Entity *e)
{
	return (e->spawnflags & 4) ? 2 : (e->spawnflags & 8) ? 0 : 1;
}

static f32 rotating_max(Entity *e)
{
	f32 m = fabsf(kvf(e, "maxspeed", 100));
	return m == 0 ? 100 : m; /* (Spawn: a forgotten maxspeed is 100) */
}

/* turn a parented rotator's local orientation by deg about its axis (its local angles turning) */
static void rotating_local_turn(Entity *e, int axis, f32 deg)
{
	f32 d = deg * (f32)M_PI / 180.0f, c = cosf(d), sn = sinf(d), L[3][3];
	memcpy(L, e->localR, sizeof(L));
	if (axis == 1) { /* yaw: about the parent-frame up */
		for (int col = 0; col < 3; col++) {
			e->localR[0][col] = c * L[0][col] - sn * L[1][col];
			e->localR[1][col] = sn * L[0][col] + c * L[1][col];
		}
	} else {
		/* roll about its own forward (column 0), pitch about its own left (column 1) */
		int a0 = axis == 2 ? 1 : 2, a1 = axis == 2 ? 2 : 0;
		for (int r = 0; r < 3; r++) {
			f32 u = L[r][a0], v = L[r][a1];
			e->localR[r][a0] = c * u + sn * v;
			e->localR[r][a1] = -sn * u + c * v;
		}
	}
}

/* how far it has turned from its start angles, -180..180 (parented: frac counts its local turn) */
static f32 rotating_from_start(const Entity *e, int ax)
{
	f32 d = fmodf(e->parent > 0 ? e->frac : e->angles[ax] - e->ang1[ax], 360.0f);
	if (d < 0)
		d += 360;
	if (d > 180)
		d -= 360;
	return d;
}

static void rotating_home(Entity *e, int ax)
{
	if (e->parent > 0) {
		rotating_local_turn(e, ax, -e->frac);
		e->frac = 0;
	} else {
		memcpy(e->angles, e->ang1, sizeof(e->angles));
		update_matrix(e);
	}
}

static void rotating_update_speed(Entity *e, f32 v)
{
	f32 mx = rotating_max(e), old = e->speed;
	e->speed = v > mx ? mx : v < -mx ? -mx : v;
	if (e->latch & 2) {
		/* (UpdateSpeed with m_bStopAtStartPos: slow ones snap home near the start) */
		int ax = rotating_axis(e);
		f32 d = rotating_from_start(e, ax);
		if (fabsf(v) < 100) {
			if (fabsf(v) <= 25 && fabsf(d) < 1.0f) {
				e->value = 0;
				e->latch &= ~2;
				e->speed = 0;
				rotating_home(e, ax);
			} else if (fabsf(d) > 90.0f)
				e->speed = old;
			else {
				f32 m = fabsf(d) < 20 ? 20 : fabsf(d); /* (eases in over the last quarter turn) */
				e->speed = old > 0 ? m : -m;
			}
		}
	}
	e->enabled = e->speed != 0 || e->value != 0;
}

static void rotating_target(Entity *e, f32 v)
{
	v = fabsf(v);
	if (e->latch & 1)
		v = -v;
	e->value = v;
	if (!(e->spawnflags & 16))
		rotating_update_speed(e, v);
	e->enabled = e->speed != 0 || e->value != 0;
	e->interval = 0;
}

static void rotating_tick(Entity *e, f32 dt)
{
	if (!e->enabled)
		return;
	if (e->speed != e->value && (e->spawnflags & 16)) {
		e->interval -= dt;
		if (e->interval <= 0) {
			e->interval = (e->latch & 2) ? 0.015f : 0.1f; /* (GetNextMoveInterval) */
			f32 mx = rotating_max(e), fr = kvf(e, "fanfriction", 0) / 100.0f;
			if (fr == 0)
				fr = 1;
			int reversing = (e->speed > 0 && e->value < 0) || (e->speed < 0 && e->value > 0);
			if (!reversing && fabsf(e->speed) < fabsf(e->value)) {
				f32 n = fabsf(e->speed) + 0.2f * mx * fr; /* SpinUpMove */
				if (n >= fabsf(e->value))
					n = e->value;
				else if (e->value < 0)
					n = -n;
				rotating_update_speed(e, n);
			} else {
				f32 tgt = reversing ? 0 : e->value; /* SpinDown (ReverseMove: to zero first) */
				f32 n = fabsf(e->speed) - 0.1f * mx * fr;
				if (n < 0)
					n = 0;
				if (n <= fabsf(tgt))
					n = tgt;
				else if (e->speed < 0)
					n = -n;
				rotating_update_speed(e, n);
			}
		}
	}
	if (e->speed == 0) {
		e->enabled = e->value != 0;
		return;
	}
	int axis = rotating_axis(e);
	f32 sgn = (e->spawnflags & 2) ? -1.0f : 1.0f;
	f32 turn = sgn * e->speed * dt;
	f32 before = rotating_from_start(e, axis);
	if (e->parent > 0) {
		/* (parented: its local angles turn, so it spins on whatever carries it - the citadel's cutters on
		 * their train, the mini teleporter's rings in their housing) */
		rotating_local_turn(e, axis, turn);
		e->frac = fmodf(e->frac + turn, 360.0f);
	} else
		e->angles[axis] = fmodf(e->angles[axis] + turn, 360.0f);
	if ((e->latch & 2) && !(e->spawnflags & 16) && fabsf(before) < fabsf(turn)) {
		/* (RotateMove with m_bStopAtStartPos: stops when the next step would pass the start) */
		e->latch &= ~2;
		e->value = 0;
		e->speed = 0;
		e->enabled = 0;
		rotating_home(e, axis);
		return;
	}
	if (e->parent > 0)
		return;
	update_matrix(e);
}

/* rendercolor "r g b", renderamt */
static void sprite_color(Entity *e, const char *rgb)
{
	int r = 255, g = 255, b = 255;
	if (rgb && *rgb)
		sscanf(rgb, "%d %d %d", &r, &g, &b);
	e->color[0] = (u8)r;
	e->color[1] = (u8)g;
	e->color[2] = (u8)b;
}

static void sprite_setup(Entity *e)
{
	sprite_color(e, kv(e, "rendercolor"));
	const char *amt = kv(e, "renderamt");
	e->color[3] = amt && *amt ? (u8)atoi(amt) : 255;
	e->rendermode = (u8)kvf(e, "rendermode", e->cls == EC_LIGHTGLOW ? 9 : 0);
	e->tex = (s16)kvf(e, "_tex", -1);
	e->tex2 = (s16)kvf(e, "_tex2", -1);
	e->scale = kvf(e, "scale", 0);
	if (e->scale <= 0)
		e->scale = 1;
	e->seen = 0;
	if (e->cls == EC_LIGHTGLOW) {
		e->glowSize[0] = kvf(e, "horizontalglowsize", 30);
		e->glowSize[1] = kvf(e, "verticalglowsize", 30);
		e->glowDist[0] = kvf(e, "mindist", 500);
		e->glowDist[1] = kvf(e, "maxdist", 2000);
		e->glowDist[2] = kvf(e, "outermaxdist", 0);
		e->enabled = 1; /* env_lightglow is always on (flag 1 = only visible from the front) */
	} else if (!strcmp(e->classname, "point_spotlight")) {
		/* CPointSpotlight: flag 1 = start on; beam length/width from the keys, halo in world glow mode */
		e->enabled = (e->spawnflags & 1) != 0;
		e->beamLen = kvf(e, "spotlightlength", 500);
		e->beamWidth = kvf(e, "spotlightwidth", 50);
		e->rendermode = 9;
		e->scale = e->beamWidth / 64.0f;
	} else {
		/* CSprite::Spawn: on at start when flagged, or when nothing can turn it on (no name) */
		e->enabled = (e->spawnflags & 1) || !e->name || !e->name[0];
	}
}

static void sprite_input(Entity *e, const char *in, const char *param)
{
	if (!strcasecmp(in, "showsprite") || !strcasecmp(in, "turnon") || !strcasecmp(in, "enable") ||
	    !strcasecmp(in, "lighton"))
		e->enabled = 1;
	else if (!strcasecmp(in, "hidesprite") || !strcasecmp(in, "turnoff") || !strcasecmp(in, "disable") ||
	         !strcasecmp(in, "lightoff"))
		e->enabled = 0;
	else if (!strcasecmp(in, "togglesprite") || !strcasecmp(in, "toggle"))
		e->enabled = !e->enabled;
	else if (!strcasecmp(in, "color"))
		sprite_color(e, param);
	else if (!strcasecmp(in, "alpha"))
		e->color[3] = (u8)atoi(param);
	else if (!strcasecmp(in, "setscale"))
		e->scale = (f32)atof(param);
}

static void rotating_input(Entity *e, const char *in, const char *param)
{
	f32 max = rotating_max(e);
	if (!strcasecmp(in, "start")) {
		e->latch &= ~2;
		rotating_target(e, max);
	} else if (!strcasecmp(in, "startforward")) {
		e->latch &= ~1; /* (InputStartForward leaves StopAtStartPos as it was) */
		rotating_target(e, max);
	} else if (!strcasecmp(in, "startbackward")) {
		e->latch = (e->latch & ~2) | 1;
		rotating_target(e, max);
	} else if (!strcasecmp(in, "stop")) {
		e->latch &= ~2;
		rotating_target(e, 0);
	} else if (!strcasecmp(in, "stopatstartpos")) {
		e->latch |= 2;
		if (e->speed == 0)
			e->latch &= ~2;
		else
			rotating_target(e, 0); /* (UpdateSpeed keeps it turning until it nears the start) */
	} else if (!strcasecmp(in, "toggle")) {
		rotating_target(e, e->speed > 0 ? 0 : max); /* (InputToggle: m_flSpeed > 0) */
	} else if (!strcasecmp(in, "reverse")) {
		e->latch = (e->latch ^ 1) & ~2;
		rotating_target(e, e->speed);
	} else if (!strcasecmp(in, "setspeed")) {
		e->latch &= ~2;
		f32 v = (f32)atof(param);
		e->latch = v < 0 ? (e->latch | 1) : (e->latch & ~1);
		v = fabsf(v);
		rotating_target(e, (v > 1 ? 1 : v) * max);
	}
}

/* things riding a character's attachment (Barney's helmet on his head): placed after the NPCs and
 * scenes have animated this tick, so they don't trail the head by a tick and show the face through */
static void follow_attachments(void)
{
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (e->parent <= 0 || !e->parentAttach[0] || (e->flags & EF_KILLED) || (ents[e->parent].flags & EF_KILLED))
			continue;
		Npc *pn = Npc_OfEntity(e->parent);
		f32 ap[3], aR[3][3];
		if (!pn || !Npc_Attachment(pn, e->parentAttach, ap, aR))
			continue;
		for (int k = 0; k < 3; k++)
			e->origin[k] = ap[k] + aR[k][0] * e->localOrigin[0] + aR[k][1] * e->localOrigin[1] + aR[k][2] * e->localOrigin[2];
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				e->R[r][c] = aR[r][0] * e->localR[0][c] + aR[r][1] * e->localR[1][c] + aR[r][2] * e->localR[2][c];
		angles_from_R(e->R, e->angles);
	}
}

static void mover_apply(Entity *e);
static void follow_parents(void)
{
	/* twice, so a child of a child follows within the same tick */
	for (int pass = 0; pass < 2; pass++)
		for (u32 i = 1; i < numEnts; i++) {
			Entity *e = &ents[i];
			if (e->parent <= 0 || (e->flags & EF_KILLED))
				continue;
			Entity *pa = &ents[e->parent];
			if (pa->flags & EF_KILLED) {
				e->parent = -1;
				continue;
			}
			if (e->parentAttach[0]) {
				Npc *pn = Npc_OfEntity(e->parent);
				f32 ap[3], aR[3][3];
				if (pn)
					continue; /* (follow_attachments places it, after the NPCs animate) */
				if (pn && Npc_Attachment(pn, e->parentAttach, ap, aR)) {
					for (int k = 0; k < 3; k++)
						e->origin[k] = ap[k] + aR[k][0] * e->localOrigin[0] + aR[k][1] * e->localOrigin[1] +
						               aR[k][2] * e->localOrigin[2];
					for (int r = 0; r < 3; r++)
						for (int c = 0; c < 3; c++)
							e->R[r][c] = aR[r][0] * e->localR[0][c] + aR[r][1] * e->localR[1][c] + aR[r][2] * e->localR[2][c];
					angles_from_R(e->R, e->angles);
					continue;
				}
			}
			for (int k = 0; k < 3; k++)
				e->origin[k] = pa->origin[k] + pa->R[k][0] * e->localOrigin[0] + pa->R[k][1] * e->localOrigin[1] +
				               pa->R[k][2] * e->localOrigin[2];
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					e->R[r][c] = pa->R[r][0] * e->localR[0][c] + pa->R[r][1] * e->localR[1][c] + pa->R[r][2] * e->localR[2][c];
			angles_from_R(e->R, e->angles);
			if (slides(e)) {
				/* a sliding door on a moving thing (the train's doors): its closed and open positions ride
				 * along, and it keeps sliding between them */
				memcpy(e->pos1, e->origin, 12);
				for (int k = 0; k < 3; k++)
					e->pos2[k] = e->pos1[k] + pa->R[k][0] * e->localMove[0] + pa->R[k][1] * e->localMove[1] +
					             pa->R[k][2] * e->localMove[2];
				/* only the position: the orientation is the parent's (mover_apply would reset the angles to
				 * the door's own, leaving it crooked whenever the train turned) */
				for (int k = 0; k < 3; k++)
					e->origin[k] = e->pos1[k] + (e->pos2[k] - e->pos1[k]) * e->frac;
			} else if (e->cls == EC_PROP_DOOR || e->cls == EC_FUNC_DOOR_ROTATING) {
				memcpy(e->pos1, e->origin, 12);
				memcpy(e->pos2, e->origin, 12);
			}
		}
}

static int has_rotation(const Entity *e) { return e->angles[0] != 0 || e->angles[1] != 0 || e->angles[2] != 0; }

/* world-space bounds of the entity's brush model or studio model, as currently placed */
static void world_bounds(const Entity *e, f32 mins[3], f32 maxs[3])
{
	const f32 *lo, *hi;
	if (e->bmodel > 0 && (u32)e->bmodel < P->numBModels) {
		lo = P->bmodels[e->bmodel].mins;
		hi = P->bmodels[e->bmodel].maxs;
	} else if (e->mdl >= 0 && (u32)e->mdl < P->numModels) {
		lo = P->models[e->mdl].mins;
		hi = P->models[e->mdl].maxs;
	} else {
		for (int k = 0; k < 3; k++)
			mins[k] = maxs[k] = e->origin[k];
		return;
	}
	if (!has_rotation(e)) {
		for (int k = 0; k < 3; k++) {
			mins[k] = lo[k] + e->origin[k];
			maxs[k] = hi[k] + e->origin[k];
		}
		return;
	}
	for (int k = 0; k < 3; k++) {
		mins[k] = 1e30f;
		maxs[k] = -1e30f;
	}
	for (int c = 0; c < 8; c++) {
		f32 v[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]};
		for (int r = 0; r < 3; r++) {
			f32 w = e->R[r][0] * v[0] + e->R[r][1] * v[1] + e->R[r][2] * v[2] + e->origin[r];
			if (w < mins[r])
				mins[r] = w;
			if (w > maxs[r])
				maxs[r] = w;
		}
	}
}

/* inside the trigger's own brushes, not just their bounding box: a trigger made of two strips (the can
 * cop's "escape" trigger, one at each way out) mustn't count the room between them */
static int in_brushes(const Entity *e, const f32 pos[3], const f32 mn[3], const f32 mx[3])
{
	if (e->bmodel <= 0 || (u32)e->bmodel >= P->numBModels)
		return 1;
	return CM_BoxInBModel((u32)e->bmodel, e->origin, has_rotation(e) ? (const f32(*)[3])e->R : NULL, pos, mn, mx);
}

static int player_touches(const Entity *e)
{
	f32 mn[3], mx[3], pmn[3], pmx[3];
	world_bounds(e, mn, mx);
	Player_Bounds(player, pmn, pmx);
	for (int k = 0; k < 3; k++)
		if (player->origin[k] + pmx[k] < mn[k] || player->origin[k] + pmn[k] > mx[k])
			return 0;
	return in_brushes(e, player->origin, pmn, pmx);
}

static int alive(const Entity *e) { return !(e->flags & (EF_KILLED | EF_DISABLED)); }
static int name_matches(const Entity *e, const char *target);
static Entity *find_named(const char *name);

/* ------------------------------------------------------------------ trigger filters (CBaseTrigger) */

#define TRIG_CLIENTS 1
#define TRIG_NPCS 2
#define TRIG_ALLY_NPCS 16
#define TRIG_EVERYTHING 64

static int negated(const Entity *f)
{
	const char *n = kv(f, "negated");
	if (!n)
		return 0;
	if (n[0] >= '0' && n[0] <= '9')
		return atoi(n) != 0;
	return strstr(n, "Filter out") != NULL || strstr(n, "filter out") != NULL;
}

/* filter_activator_class / _name / _multi: does entity `who` pass the filter called `name`? */
static int filter_pass_depth(const char *name, s32 who, int depth)
{
	if (!name || !*name || depth > 4)
		return 1;
	Entity *f = find_named(name);
	if (!f || !f->classname)
		return 1;
	const Entity *w = &ents[who];
	int match = 1;
	if (!strcasecmp(f->classname, "filter_activator_class")) {
		const char *c = kv(f, "filterclass");
		match = c && w->classname && !strcasecmp(c, w->classname);
	} else if (!strcasecmp(f->classname, "filter_activator_name")) {
		const char *c = kv(f, "filtername");
		match = c && (who == 0 ? !strcasecmp(c, "!player") : name_matches(w, c));
	} else if (!strcasecmp(f->classname, "filter_multi")) {
		int any = kvf(f, "filtertype", 0) != 0; /* 0: all must pass, 1: any */
		match = !any;
		for (int k = 1; k <= 10; k++) { /* (CFilterMulti: Filter01..Filter10) */
			char key[16];
			snprintf(key, sizeof(key), "filter%02d", k);
			const char *sub = kv(f, key);
			if (!sub || !*sub)
				continue;
			int r = filter_pass_depth(sub, who, depth + 1);
			if (any && r)
				match = 1;
			if (!any && !r)
				match = 0;
		}
	} else
		return 1; /* (team / damage type filters: nothing to check here) */
	return negated(f) ? !match : match;
}

static int is_ally_class(const char *c)
{
	static const char *const allies[] = {"npc_citizen", "npc_barney", "npc_alyx", "npc_vortigaunt", "npc_dog",
	                                     "npc_monk", "npc_eli", "npc_kleiner", "npc_mossman", "npc_magnusson", "npc_fisherman"};
	for (u32 i = 0; c && i < sizeof(allies) / sizeof(allies[0]); i++)
		if (!strcasecmp(c, allies[i]))
			return 1;
	return 0;
}

/* the NPCs that are about, gathered once a tick for the triggers */
#define MAX_LIVE_NPCS 128
static u16 liveNpcs[MAX_LIVE_NPCS];
static u32 numLiveNpcs;

#define MAX_TRIG_BODIES 256
static s32 trigBodies[MAX_TRIG_BODIES];
static f32 trigBodyBox[MAX_TRIG_BODIES][6];
static int numTrigBodies; /* -1: not gathered yet this tick (only when a trigger wants physics objects) */

static void gather_bodies(void)
{
	numTrigBodies = Phys_BodyEnts(trigBodies, MAX_TRIG_BODIES);
	for (int j = 0; j < numTrigBodies; j++)
		world_bounds(&ents[trigBodies[j]], &trigBodyBox[j][0], &trigBodyBox[j][3]);
}

static void gather_npcs(void)
{
	numTrigBodies = -1;
	numLiveNpcs = 0;
	for (u32 i = 1; i < numEnts && numLiveNpcs < MAX_LIVE_NPCS; i++)
		if (ents[i].cls == EC_NPC && ents[i].npcModel >= 0 && alive(&ents[i]))
			liveNpcs[numLiveNpcs++] = (u16)i;
}

static int npc_touches(const Entity *t, const Entity *n)
{
	f32 mn[3], mx[3];
	world_bounds(t, mn, mx);
	static const f32 lo[3] = {-13, -13, 0}, hi[3] = {13, 13, 64};
	for (int k = 0; k < 3; k++)
		if (n->origin[k] + hi[k] < mn[k] || n->origin[k] + lo[k] > mx[k])
			return 0;
	return in_brushes(t, n->origin, lo, hi);
}

/* who's in a trigger (Source's PassesTriggerFilters): the player or an NPC, by the trigger's flags and
 * filter. Returns the toucher's entity index (0 = the player), or -1 */
static s32 trigger_toucher(const Entity *e)
{
	u32 sf = e->spawnflags;
	const char *filt = kv(e, "filtername");
	int wantPlayer = (sf & (TRIG_CLIENTS | TRIG_EVERYTHING)) || !(sf & (TRIG_NPCS | TRIG_ALLY_NPCS | 4 | 8 | 1024 | 2048));
	int wantNpc = (sf & (TRIG_NPCS | TRIG_ALLY_NPCS | TRIG_EVERYTHING)) != 0;
	if (wantPlayer && player_touches(e) && filter_pass_depth(filt, 0, 0))
		return 0;
	/* physics objects (SF_TRIGGER_ALLOW_PHYSICS 8): the can in the bin, the plug in its socket */
	if (sf & (8 | TRIG_EVERYTHING)) {
		f32 mn[3], mx[3];
		world_bounds(e, mn, mx);
		if (numTrigBodies < 0)
			gather_bodies();
		for (int j = 0; j < numTrigBodies; j++) {
			s32 bi = trigBodies[j];
			if ((u32)bi >= numEnts || !alive(&ents[bi]) || bi == (s32)(e - ents))
				continue;
			const f32 *bmn = &trigBodyBox[j][0], *bmx = &trigBodyBox[j][3];
			int over = 1;
			for (int k = 0; k < 3; k++)
				if (bmx[k] < mn[k] || bmn[k] > mx[k])
					over = 0;
			if (!over)
				continue;
			f32 c[3], lo[3], hi[3];
			for (int k = 0; k < 3; k++) {
				c[k] = (bmn[k] + bmx[k]) * 0.5f;
				lo[k] = bmn[k] - c[k];
				hi[k] = bmx[k] - c[k];
			}
			if (in_brushes(e, c, lo, hi) && filter_pass_depth(filt, bi, 0))
				return bi;
		}
	}
	if (!wantNpc)
		return -1;
	for (u32 j = 0; j < numLiveNpcs; j++) {
		u32 i = liveNpcs[j];
		const Entity *n = &ents[i];
		if ((sf & TRIG_ALLY_NPCS) && !(sf & (TRIG_NPCS | TRIG_EVERYTHING)) && !is_ally_class(n->classname))
			continue;
		if (npc_touches(e, n) && filter_pass_depth(filt, (s32)i, 0))
			return (s32)i;
	}
	return -1;
}

static void door_sound(Entity *e, int which);
static void door_arrived(Entity *e);
static void npc_say(Entity *e, const char *key);
static void break_effects(Entity *e, const f32 dir[3]);

/* ------------------------------------------------------------------ outputs and events */

static void queue(const char *target, const char *input, const char *param, f32 delay, s32 activator, s32 caller)
{
	if (numEvents >= MAX_EVENTS)
		return;
	Event *ev = &events[numEvents++];
	ev->time = now + delay;
	ev->activator = activator;
	ev->caller = caller;
	snprintf(ev->target, sizeof(ev->target), "%s", target);
	snprintf(ev->input, sizeof(ev->input), "%s", input);
	snprintf(ev->param, sizeof(ev->param), "%s", param ? param : "");
}

/* Output values look like "target,Input,param,delay,times" (separator ',' or 0x1B). */
static void fire_output_value(Entity *e, const char *output, s32 activator, const char *value);
static void fire_output(Entity *e, const char *output, s32 activator) { fire_output_value(e, output, activator, NULL); }

/* (an output with a value, OutValue and the like: an empty parameter in the map passes the value on) */
static void fire_output_value(Entity *e, const char *output, s32 activator, const char *value)
{
	s32 self = (s32)(e - ents);
	for (u32 i = 0; i < e->numKV; i++) {
		u32 ki = e->firstKV + i;
		const PackKV *k = &P->kvs[ki];
		if (strcmp(str(k->key), output) != 0)
			continue;
		char buf[192], *f[5] = {0};
		snprintf(buf, sizeof(buf), "%s", str(k->val));
		char sep = strchr(buf, 0x1B) ? 0x1B : ',';
		int nf = 0;
		char *p = buf;
		f[nf++] = p;
		while (*p && nf < 5) {
			if (*p == sep) {
				*p = 0;
				f[nf++] = p + 1;
			}
			p++;
		}
		if (nf < 2 || !f[0][0])
			continue;
		int times = nf >= 5 && f[4][0] ? atoi(f[4]) : -1;
		if (times > 0 && kvFired[ki] >= times)
			continue;
		kvFired[ki]++;
		const char *prm = nf >= 3 ? f[2] : "";
		if (value && !prm[0])
			prm = value;
		queue(f[0], f[1], prm, nf >= 4 && f[3][0] ? (f32)atof(f[3]) : 0, activator, self);
	}
}

void Ent_FireOutputValue(s32 ent, const char *output, s32 activator, const char *value)
{
	if (ent > 0 && (u32)ent < numEnts)
		fire_output_value(&ents[ent], output, activator, value);
}

static void input(Entity *e, const char *in, const char *param, s32 activator, s32 caller);

static int name_matches(const Entity *e, const char *target)
{
	size_t n = strlen(target);
	if (n && target[n - 1] == '*')
		return e->name && strncasecmp(e->name, target, n - 1) == 0;
	return e->name && strcasecmp(e->name, target) == 0;
}

static void dispatch(const Event *ev)
{
	const char *t = ev->target;
#ifdef ENT_TRACE
	{
		const Entity *c = (ev->caller >= 0 && (u32)ev->caller < numEnts) ? &ents[ev->caller] : NULL;
		printf("[%7.2f] %-28s -> %-28s %s(%s)\n", now, c && c->name ? c->name : (c ? c->classname : "?"), t,
		       ev->input, ev->param);
	}
#endif
	if (t[0] == '!') {
		s32 idx = -1;
		if (strcasecmp(t, "!player") == 0)
			idx = 0;
		else if (strcasecmp(t, "!activator") == 0)
			idx = ev->activator;
		else if (strcasecmp(t, "!self") == 0 || strcasecmp(t, "!caller") == 0)
			idx = ev->caller;
		if (idx >= 0 && (u32)idx < numEnts)
			input(&ents[idx], ev->input, ev->param, ev->activator, ev->caller);
		return;
	}
	int hit = 0;
	for (u32 i = 1; i < numEnts; i++)
		if (!(ents[i].flags & EF_KILLED) && name_matches(&ents[i], t)) {
			input(&ents[i], ev->input, ev->param, ev->activator, ev->caller);
			hit = 1;
		}
	if (!hit) /* Source also lets outputs target a classname */
		for (u32 i = 1; i < numEnts; i++)
			if (!(ents[i].flags & EF_KILLED) && ents[i].classname && strcasecmp(ents[i].classname, t) == 0)
				input(&ents[i], ev->input, ev->param, ev->activator, ev->caller);
}

static void run_events(void)
{
	/* fire everything that is due, in the order it was queued; inputs may queue more */
	for (int pass = 0; pass < 8; pass++) {
		int any = 0;
		for (u32 i = 0; i < numEvents;) {
			if (events[i].time <= now) {
				Event ev = events[i];
				memmove(&events[i], &events[i + 1], (numEvents - i - 1) * sizeof(Event));
				numEvents--;
				dispatch(&ev);
				any = 1;
			} else {
				i++;
			}
		}
		if (!any)
			break;
	}
}

/* ------------------------------------------------------------------ movers */

enum { MV_CLOSED, MV_OPENING, MV_OPEN, MV_CLOSING, MV_STOPPED /* func_movelinear between its ends */ };

static int slides(const Entity *e);
static void mover_apply(Entity *e)
{
	if (e->parent > 0 && slides(e)) {
		/* riding a parent: the orientation follows the parent (follow_parents), only the slide is ours */
		for (int k = 0; k < 3; k++)
			e->origin[k] = e->pos1[k] + (e->pos2[k] - e->pos1[k]) * e->frac;
		return;
	}
	for (int k = 0; k < 3; k++) {
		e->origin[k] = e->pos1[k] + (e->pos2[k] - e->pos1[k]) * e->frac;
		e->angles[k] = e->ang1[k] + (e->ang2[k] - e->ang1[k]) * e->frac;
	}
	update_matrix(e);
}

static void mover_open(Entity *e, s32 activator)
{
	if (e->locked)
		return;
	if (e->latch & 4) { /* (a SetPosition move: Open sends it on to the end) */
		e->latch &= ~4;
		if (e->moveState == MV_OPENING)
			return;
	}
	if (e->moveState == MV_OPEN || e->moveState == MV_OPENING)
		return;
	e->moveState = MV_OPENING;
	e->toucher = activator; /* (m_hActivator: who set it going) */
	door_sound(e, 0);
	fire_output(e, "onopen", activator);
}

static void mover_close(Entity *e, s32 activator)
{
	if (e->latch & 4) {
		e->latch &= ~4;
		if (e->moveState == MV_CLOSING)
			return;
	}
	if (e->moveState == MV_CLOSED || e->moveState == MV_CLOSING)
		return;
	e->moveState = MV_CLOSING;
	e->toucher = activator;
	door_sound(e, 0);
	fire_output(e, "onclose", activator);
}

/* would this mover, where it is now, be inside the player? (keeps its collision entry current too) */
static int blocks_player(Entity *e)
{
	if (!(e->flags & EF_SOLID) || player->noclip)
		return 0;
	CMEnt *c = NULL;
	for (u32 k = 0; k < numSolids; k++)
		if (solids[k].ent == (s32)(e - ents)) {
			c = &solids[k];
			break;
		}
	if (!c)
		return 0;
	memcpy(c->origin, e->origin, sizeof(c->origin));
	memcpy(c->R, e->R, sizeof(c->R));
	c->rotated = (u8)has_rotation(e);
	world_bounds(e, c->mins, c->maxs);
	f32 mn[3], mx[3];
	Player_Bounds(player, mn, mx);
	Trace tr;
	CM_BoxTrace(&tr, player->origin, player->origin, mn, mx, MASK_PLAYERSOLID);
	return tr.startsolid && tr.ent == (s32)(e - ents);
}

static void mover_tick(Entity *e, f32 dt)
{
	f32 step = e->moveLen > 0.01f ? dt * e->speed / e->moveLen : 1.0f;
	int door = e->cls == EC_PROP_DOOR || e->cls == EC_FUNC_DOOR || e->cls == EC_FUNC_DOOR_ROTATING;
	if (door && (e->moveState == MV_OPENING || e->moveState == MV_CLOSING)) {
		/* a door moving into the player stops there (prop doors) or goes back (brush doors), like Source */
		f32 was = e->frac;
		e->frac += e->moveState == MV_OPENING ? step : -step;
		e->frac = e->frac < 0 ? 0 : e->frac > 1 ? 1 : e->frac;
		mover_apply(e);
		if (blocks_player(e)) {
			e->frac = was;
			mover_apply(e);
			blocks_player(e);
			if (!e->blocked) {
				fire_output(e, e->moveState == MV_CLOSING ? "onblockedclosing" : "onblockedopening", 0);
				e->blocked = 1;
			}
			if (e->cls != EC_PROP_DOOR && e->moveState == MV_CLOSING)
				e->moveState = MV_OPENING;
			return;
		}
		if (e->blocked) {
			e->blocked = 0;
			fire_output(e, e->moveState == MV_CLOSING ? "onunblockedclosing" : "onunblockedopening", 0);
		}
		e->frac = was; /* the normal path below advances it */
	}
	if ((e->latch & 4) && (e->moveState == MV_OPENING || e->moveState == MV_CLOSING)) {
		/* (CFuncMoveLinear::SetPosition: MoveTo the position at m_flSpeed; MoveDone fires the end outputs
		 * only at an end) */
		f32 t = e->value;
		if (e->moveState == MV_OPENING ? e->frac + step >= t : e->frac - step <= t) {
			e->frac = t;
			e->latch &= ~4;
			e->moveState = t >= 1 ? MV_OPEN : t <= 0 ? MV_CLOSED : MV_STOPPED;
			if (t >= 1)
				fire_output(e, "onfullyopen", (s32)(e - ents));
			else if (t <= 0)
				fire_output(e, "onfullyclosed", (s32)(e - ents));
		} else
			e->frac += e->moveState == MV_OPENING ? step : -step;
		mover_apply(e);
		return;
	}
	if (e->moveState == MV_OPENING) {
		e->frac += step;
		if (e->frac >= 1) {
			e->frac = 1;
			e->moveState = MV_OPEN;
			door_arrived(e);
			e->waitUntil = now + e->wait;
			fire_output(e, "onfullyopen", (s32)(e - ents)); /* (DoorHitTop: the door is the activator) */
			if (e->cls == EC_FUNC_BUTTON)
				fire_output(e, "onin", e->toucher);
		}
		mover_apply(e);
	} else if (e->moveState == MV_CLOSING) {
		e->frac -= step;
		if (e->frac <= 0) {
			e->frac = 0;
			e->moveState = MV_CLOSED;
			door_arrived(e);
			door_sound(e, 1);
			fire_output(e, "onfullyclosed", e->toucher); /* (DoorHitBottom: m_hActivator) */
			if (e->cls == EC_FUNC_BUTTON)
				fire_output(e, "onout", e->toucher);
		}
		mover_apply(e);
	} else if (e->moveState == MV_OPEN && e->wait >= 0 && now >= e->waitUntil && e->cls != EC_FUNC_MOVELINEAR &&
	           !((e->cls == EC_FUNC_DOOR || e->cls == EC_FUNC_DOOR_ROTATING || e->cls == EC_FUNC_BUTTON) &&
	             (e->spawnflags & 32))) {
		/* (a toggle button stays pressed in until it's used again: TriggerAndWait) */
		/* (func_door's "Toggle" flag: it stays open until told to close) */
		mover_close(e, 0);
	}
}


/* turnstiles: the player walking into one turns it about its hinge (Source simulates the physbox on its
 * phys_hinge; here the push is the player's intended movement, and the bars stop where they'd hit him) */
static Entity *find_named(const char *name);
static void hinge_tick(Entity *e, f32 dt)
{
	f32 w[2];
	Phys_GetWish(w);
	f32 rx = player->origin[0] - e->hingePivot[0], ry = player->origin[1] - e->hingePivot[1];
	f32 r = sqrtf(rx * rx + ry * ry);
	f32 mn[3], mx[3];
	world_bounds(e, mn, mx);
	f32 reach = 0;
	for (int k = 0; k < 2; k++) {
		f32 a = fabsf(mn[k] - e->hingePivot[k]), b = fabsf(mx[k] - e->hingePivot[k]);
		reach = a > reach ? a : reach;
		reach = b > reach ? b : reach;
	}
	f32 ws = sqrtf(w[0] * w[0] + w[1] * w[1]);
	int pushing = r < reach + 20.0f && r > 4.0f && ws > 20.0f && player->origin[2] < mx[2] && player->origin[2] + 72 > mn[2];
	if (pushing) {
		/* the bar in front of him moves along with his walk: angular speed = tangential speed / radius */
		f32 tang = (rx * w[1] - ry * w[0]) / r;
		f32 want = tang / (r > 16 ? r : 16) * 57.29578f; /* torque r x push: the bar in front moves his way */
		e->hingeVel += (want - e->hingeVel) * (dt * 6 < 1 ? dt * 6 : 1);
	} else {
		e->hingeVel *= expf(-dt * 2.5f); /* hinge friction */
	}
	if (fabsf(e->hingeVel) < 0.5f)
		return;
	f32 da = e->hingeVel * dt;
#ifdef HINGE_DEBUG
	if (fabsf(da) > 20 || fabsf(e->angles[1]) > 1e5f)
		printf("hinge vel %f da %f ang %f r %f w %f %f\n", e->hingeVel, da, e->angles[1], r, w[0], w[1]);
#endif
	f32 oldA = e->angles[1], oldO[3];
	memcpy(oldO, e->origin, 12);
	f32 c = cosf(da * (f32)M_PI / 180), s = sinf(da * (f32)M_PI / 180);
	f32 ox = e->origin[0] - e->hingePivot[0], oy = e->origin[1] - e->hingePivot[1];
	e->origin[0] = e->hingePivot[0] + ox * c - oy * s;
	e->origin[1] = e->hingePivot[1] + ox * s + oy * c;
	e->angles[1] += da;
	update_matrix(e);
	if (blocks_player(e)) {
		/* turning the other way into him: stop */
		e->angles[1] = oldA;
		memcpy(e->origin, oldO, 12);
		update_matrix(e);
		blocks_player(e);
		e->hingeVel = 0;
	}
}

/* an NPC walking into a turnstile turns it like the player does (Source's NPCs push physics objects
 * through their shadow controller); returns 1 if ent is one, so the NPC keeps walking through it */
int Ent_HingePush(s32 idx, const f32 pos[3], f32 vx, f32 vy)
{
	if (idx <= 0 || (u32)idx >= numEnts)
		return 0;
	Entity *e = &ents[idx];
	if (!e->hinged || (e->flags & (EF_DISABLED | EF_KILLED)))
		return 0;
	f32 rx = pos[0] - e->hingePivot[0], ry = pos[1] - e->hingePivot[1];
	f32 r = sqrtf(rx * rx + ry * ry);
	if (r < 4.0f)
		return 1;
	f32 tang = (rx * vy - ry * vx) / r;
	f32 want = tang / (r > 16 ? r : 16) * 57.29578f;
	e->hingeVel += (want - e->hingeVel) * 0.2f;
	return 1;
}

static void setup_hinges(void)
{
	for (u32 i = 1; i < numEnts; i++) {
		Entity *h = &ents[i];
		if (strcasecmp(h->classname, "phys_hinge"))
			continue;
		const char *a2 = kv(h, "attach2");
		if (a2 && *a2)
			continue; /* hinged to another object, not the world */
		Entity *b = find_named(kv(h, "attach1"));
		if (!b || b->bmodel <= 0)
			continue;
		f32 ax[3];
		kvvec(h, "hingeaxis", ax);
		f32 d[3] = {ax[0] - h->origin[0], ax[1] - h->origin[1], ax[2] - h->origin[2]};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (l < 0.01f || fabsf(d[2]) / l < 0.9f)
			continue; /* only upright hinges: turnstiles, carousels */
		b->hinged = 1;
		b->hingeVel = 0;
		memcpy(b->hingePivot, h->origin, 12);
	}
}

static void prop_door_direction_from(Entity *e, const f32 from[3], int byPlayer);
static void prop_door_angles(const Entity *e, f32 *fwdA, f32 *backA);
static void setup_door(Entity *e)
{
	memcpy(e->pos1, e->origin, sizeof(e->pos1));
	memcpy(e->pos2, e->origin, sizeof(e->pos2));
	memcpy(e->ang1, e->angles, sizeof(e->ang1));
	memcpy(e->ang2, e->angles, sizeof(e->ang2));
	e->speed = kvf(e, "speed", 100);
	if (e->cls == EC_FUNC_DOOR || e->cls == EC_FUNC_MOVELINEAR || e->cls == EC_FUNC_BUTTON) {
		f32 md[3], dir[3];
		kvvec(e, "movedir", md);
		const f32 d = (f32)M_PI / 180.0f;
		dir[0] = cosf(md[0] * d) * cosf(md[1] * d);
		dir[1] = cosf(md[0] * d) * sinf(md[1] * d);
		dir[2] = -sinf(md[0] * d);
		if (e->cls == EC_FUNC_MOVELINEAR) {
			e->moveLen = kvf(e, "movedistance", 100);
		} else {
			const PackBModel *bm = (e->bmodel > 0) ? &P->bmodels[e->bmodel] : NULL;
			/* CBaseDoor::Spawn: m_vecPosition2 = pos1 + movedir * (|movedir . (size - 2)| - lip). A lip bigger
			 * than the door moves it the other way (Kleiner's teleport cage gates rise into the ceiling) */
			f32 size = 0;
			if (bm)
				size = fabsf(dir[0] * (bm->maxs[0] - bm->mins[0] - 2)) + fabsf(dir[1] * (bm->maxs[1] - bm->mins[1] - 2)) +
				       fabsf(dir[2] * (bm->maxs[2] - bm->mins[2] - 2));
			f32 len = size - kvf(e, "lip", e->cls == EC_FUNC_BUTTON ? 4 : 0);
			e->moveLen = fabsf(len);
			if (len < 0)
				for (int k = 0; k < 3; k++)
					dir[k] = -dir[k];
		}
		for (int k = 0; k < 3; k++)
			e->pos2[k] = e->pos1[k] + dir[k] * e->moveLen;
		e->wait = kvf(e, "wait", e->cls == EC_FUNC_BUTTON ? 1 : 4);
	} else { /* rotating: func_door_rotating, prop_door_rotating */
		f32 dist = kvf(e, "distance", 90);
		if ((e->spawnflags & 2) && e->cls != EC_PROP_DOOR)
			dist = -dist; /* reverse direction */
		if (e->cls == EC_PROP_DOOR)
			dist = -fabsf(dist ? dist : 90); /* (forward; prop_door_direction picks the side when it opens) */
		int axis = 1;      /* yaw */
		if (e->cls == EC_FUNC_DOOR_ROTATING) {
			if (e->spawnflags & 64)
				axis = 2; /* roll */
			else if (e->spawnflags & 128)
				axis = 0; /* pitch */
		}
		e->ang2[axis] += dist;
		e->moveLen = fabsf(dist);
		e->wait = e->cls == EC_PROP_DOOR ? kvf(e, "returndelay", -1) : kvf(e, "wait", 4);
		if (e->cls == EC_PROP_DOOR) {
			/* CPropDoorRotating::DoorTeleportToSpawnPosition: the "Starts Open" flag (1) or spawnpos 1:
			 * open forward; 2: open back; 3: ajar at "ajarangles"; else closed */
			int sp = (e->spawnflags & 1) ? 1 : (int)kvf(e, "spawnpos", 0);
			f32 fwdA, backA;
			prop_door_angles(e, &fwdA, &backA);
			e->ang2[1] = e->ang1[1] + fwdA;
			if (sp == 2) {
				e->ang2[1] = e->ang1[1] + backA;
			} else if (sp == 3) {
				f32 aj[3] = {0, 0, 0};
				kvvec(e, "ajarangles", aj);
				f32 da = fmodf(aj[1] - e->ang1[1] + 540.0f, 360.0f) - 180.0f;
				if (fabsf(da) > 0.5f) {
					e->ang2[1] = e->ang1[1] + (da > 0 ? fabsf(fwdA) : -fabsf(fwdA));
					e->ajar = fabsf(da) / fabsf(fwdA) > 1 ? 1 : fabsf(da) / fabsf(fwdA);
				}
			}
			e->spawnOpen = sp == 1 || sp == 2;
		}
	}
	if (e->cls == EC_FUNC_BUTTON && ((e->spawnflags & 1) || e->moveLen < 1)) {
		/* "don't move" (or under a unit of travel): pressed in at once (LinearMove to where it is) */
		memcpy(e->pos2, e->pos1, sizeof(e->pos2));
		e->moveLen = 0;
	}
	e->locked = (e->spawnflags & 2048) ? 1 : 0;
	if (e->cls == EC_FUNC_DOOR_ROTATING && (e->spawnflags & 1)) {
		/* CRotDoor::Spawn, "Starts Open": the two angles swap. It sits at what was its open angle and
		 * counts as closed; opening takes it to where it was built. */
		f32 t[3];
		memcpy(t, e->ang1, 12);
		memcpy(e->ang1, e->ang2, 12);
		memcpy(e->ang2, t, 12);
		e->frac = 0;
		e->moveState = MV_CLOSED;
	} else if ((e->cls == EC_FUNC_DOOR && ((e->spawnflags & 1) || kvf(e, "spawnpos", 0) == 1)) ||
	           (e->cls == EC_FUNC_DOOR_ROTATING && kvf(e, "spawnpos", 0) == 1) || (e->cls == EC_PROP_DOOR && e->spawnOpen) ||
	           (e->cls == EC_FUNC_MOVELINEAR && kvf(e, "startposition", 0) >= 1)) {
		/* CBaseDoor::Spawn: a sliding door that starts open sits at its open position and IS open
		 * (nothing swaps): Close brings it to where it was built */
		e->frac = 1;
		e->moveState = MV_OPEN;
		e->waitUntil = 1e30f;
	} else if (e->cls == EC_PROP_DOOR && e->ajar > 0) {
		e->frac = e->ajar; /* ajar: shut as far as the door's logic goes, standing partly open */
		e->moveState = MV_CLOSED;
	}
	mover_apply(e);
}

/* trains, func_movelinear, func_rotating and buttons: the start sound as they set off, their move sound
 * looping (following them) while they move, the stop sound when they stop (CFuncTrackTrain's
 * MoveSound/StartSound/StopSound, CFuncMoveLinear's StartSound/StopSound, func_rotating's message,
 * func_button's sounds) */
static void motion_sounds(void)
{
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (e->esnd[0] < 0 && e->esnd[1] < 0 && e->esnd[2] < 0)
			continue;
		int moving = 0;
		switch (e->cls) {
		case EC_TRACKTRAIN:
			moving = e->trainSpeed != 0 && e->pathNode > 0;
			break;
		case EC_FUNC_MOVELINEAR:
		case EC_FUNC_BUTTON:
			moving = e->moveState == MV_OPENING || e->moveState == MV_CLOSING;
			break;
		case EC_ROTATING:
			moving = e->enabled && e->speed != 0;
			break;
		default:
			break;
		}
		if (e->flags & (EF_KILLED | EF_DISABLED))
			moving = 0;
		f32 c[3], mn[3], mx[3];
		world_bounds(e, mn, mx);
		for (int k = 0; k < 3; k++)
			c[k] = (mn[k] + mx[k]) * 0.5f;
		if (moving && !e->sndMoving) {
			if (e->esnd[0] >= 0 && (u32)e->esnd[0] < P->numSounds)
				e->sndStart = Audio_Play(&P->sounds[e->esnd[0]], c, 1.0f, SNDLVL_DEFAULT);
		} else if (!moving && e->sndMoving) {
			if (e->sndLoop)
				Audio_Stop(e->sndLoop);
			e->sndLoop = 0;
			/* (CBaseDoor::DoorHitTop: StopSound(m_NoiseMoving); a looping move sound would play forever) */
			if (e->sndStart && e->esnd[0] >= 0 && Sound_Loops(&P->sounds[e->esnd[0]]))
				Audio_Stop(e->sndStart);
			e->sndStart = 0;
			if (e->esnd[2] >= 0 && (u32)e->esnd[2] < P->numSounds && e->cls != EC_FUNC_BUTTON)
				Audio_Play(&P->sounds[e->esnd[2]], c, 1.0f, SNDLVL_DEFAULT);
		}
		if (moving && e->esnd[1] >= 0 && (u32)e->esnd[1] < P->numSounds) {
			f32 vol = 1.0f;
			if (e->cls == EC_ROTATING) {
				f32 mx = kvf(e, "maxspeed", 100);
				vol = mx > 0 ? fabsf(e->speed) / mx : 1.0f;
				vol = vol > 1 ? 1 : vol;
			}
			if (!e->sndLoop || !Audio_IsPlaying(e->sndLoop))
				e->sndLoop = Audio_Play(&P->sounds[e->esnd[1]], c, vol, SNDLVL_DEFAULT);
			else {
				Audio_SetPos(e->sndLoop, c);
				Audio_SetChannelVolume(e->sndLoop, vol);
			}
		}
		e->sndMoving = (u8)moving;
	}
}

static void door_sound(Entity *e, int which)
{
	s16 s = e->doorSnd[which];
	if (s < 0 || (u32)s >= P->numSounds)
		return;
	f32 c[3];
	f32 mn[3], mx[3];
	world_bounds(e, mn, mx);
	for (int k = 0; k < 3; k++)
		c[k] = (mn[k] + mx[k]) * 0.5f;
	if (which == 0 && e->sndStart)
		Audio_Stop(e->sndStart);
	u32 h = Audio_Play(&P->sounds[s], c, 1.0f, SNDLVL_DEFAULT);
	if (which == 0)
		e->sndStart = Sound_Loops(&P->sounds[s]) ? h : 0; /* a looping move sound stops when it gets there */
}

static void door_arrived(Entity *e)
{
	if (e->sndStart)
		Audio_Stop(e->sndStart);
	e->sndStart = 0;
}

/* prop doors swing away from whoever opens them (CPropDoorRotating): "forward" is the closed yaw minus
 * the distance and "back" plus it, swapped when the hinge is on the door's left (the farthest corner of
 * its bounds lies to its right); opendir 1 / 2 always open forward / back, 0 opens away from the opener:
 * back when they stand on its forward side */
static int hinge_on_left(const Entity *e)
{
	if (e->mdl < 0 || (u32)e->mdl >= P->numModels)
		return 0;
	const PackModel *m = &P->models[e->mdl];
	f32 mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
	for (int c = 0; c < 8; c++) {
		f32 l[3] = {(c & 1) ? m->maxs[0] : m->mins[0], (c & 2) ? m->maxs[1] : m->mins[1], (c & 4) ? m->maxs[2] : m->mins[2]};
		for (int k = 0; k < 3; k++) {
			/* (in the closed orientation) */
			f32 w = e->R[k][0] * l[0] + e->R[k][1] * l[1] + e->R[k][2] * l[2];
			if (w < mn[k])
				mn[k] = w;
			if (w > mx[k])
				mx[k] = w;
		}
	}
	const f32 *pt = (mn[0] * mn[0] + mn[1] * mn[1] > mx[0] * mx[0] + mx[1] * mx[1]) ? mn : mx;
	f32 yaw = e->ang1[1] * (f32)M_PI / 180.0f;
	f32 right[2] = {sinf(yaw), -cosf(yaw)};
	return pt[0] * right[0] + pt[1] * right[1] > 0;
}

static void prop_door_direction_from(Entity *e, const f32 from[3], int byPlayer);
static void prop_door_direction(Entity *e);
/* does the door's swing from shut to shut+delta run into the world? (a line along the leaf at
 * mid-height, sampled through the arc; the leaf is whichever way the model is longer, +Y for
 * Source's doors) */
static int door_swing_blocked(const Entity *e, f32 delta)
{
	if (e->mdl < 0 || (u32)e->mdl >= P->numModels)
		return 0;
	const PackModel *m = &P->models[e->mdl];
	f32 lx = fabsf(m->mins[0]) > fabsf(m->maxs[0]) ? m->mins[0] : m->maxs[0];
	f32 ly = fabsf(m->mins[1]) > fabsf(m->maxs[1]) ? m->mins[1] : m->maxs[1];
	if (fabsf(lx) > fabsf(ly))
		ly = 0;
	else
		lx = 0;
	f32 z = e->pos1[2] + (m->mins[2] + m->maxs[2]) * 0.5f;
	for (int k = 2; k <= 10; k += 2) {
		f32 y = (e->ang1[1] + delta * k * 0.1f) * (f32)M_PI / 180.0f, c = cosf(y), sn = sinf(y);
		f32 dx = c * lx - sn * ly, dy = sn * lx + c * ly;
		f32 a[3] = {e->pos1[0] + dx * 0.35f, e->pos1[1] + dy * 0.35f, z};
		f32 b[3] = {e->pos1[0] + dx * 0.9f, e->pos1[1] + dy * 0.9f, z};
		Trace tr;
		CM_TraceWorld(&tr, a, b, CONTENTS_SOLID);
		if (tr.fraction < 0.95f || tr.startsolid)
			return 1;
	}
	return 0;
}

/* CPropDoorRotating: open forward = closed yaw - distance, open back = + distance, the two swapped for
 * a leaf hinged on its left (CalcOpenAngles, IsHingeOnLeft). */
static void prop_door_angles(const Entity *e, f32 *fwdA, f32 *backA)
{
	f32 dist = fabsf(kvf(e, "distance", 90));
	if (dist == 0)
		dist = 90;
	*fwdA = -dist;
	*backA = dist;
	if (hinge_on_left(e)) {
		*fwdA = dist;
		*backA = -dist;
	}
}

/* CPropDoorRotating::BeginOpening: "opendir" 1 opens forward, 2 back; either way (0, what the maps
 * use): forward, unless whoever it opens away from is in front of the door (along its forward
 * axis), then back; and when the player opens it and that way is blocked, the other way.
 * from NULL: opened by the map (the Open input), which is forward. */
static void prop_door_direction_from(Entity *e, const f32 from[3], int byPlayer)
{
	f32 fwdA, backA;
	prop_door_angles(e, &fwdA, &backA);
	int opendir = (int)kvf(e, "opendir", 0);
	f32 delta = fwdA;
	if (opendir == 2)
		delta = backA;
	else if (opendir == 0) {
		if (from) {
			f32 yaw = e->ang1[1] * (f32)M_PI / 180.0f;
			f32 fx = cosf(yaw), fy = sinf(yaw);
			if (fx * from[0] + fy * from[1] > fx * e->pos1[0] + fy * e->pos1[1])
				delta = backA;
		}
		if (byPlayer && from && door_swing_blocked(e, delta) && !door_swing_blocked(e, -delta))
			delta = -delta;
	}
	if (e->moveState == MV_CLOSED) {
		/* a door standing ajar keeps where it stands: as a fraction of the new swing (negative when it
		 * opens the other way, so it swings back through shut and on, like Source's) */
		f32 cur = e->ang1[1] + (e->ang2[1] - e->ang1[1]) * e->frac;
		e->ang2[1] = e->ang1[1] + delta;
		e->frac = delta != 0 ? (cur - e->ang1[1]) / delta : 0;
	}
}
static void prop_door_direction(Entity *e) { prop_door_direction_from(e, player->origin, 1); }

/* ------------------------------------------------------------------ inputs */

static void set_enabled(Entity *e, int on)
{
	e->enabled = (u8)on;
	if (e->cls == EC_FUNC_BRUSH || e->cls == EC_FUNC_WALL || e->cls == EC_PROP || e->cls == EC_FUNC_ILLUSIONARY) {
		if (on)
			e->flags &= ~EF_DISABLED;
		else
			e->flags |= EF_DISABLED;
	}
}

/* CMathCounter::UpdateOutValue: with a range (min or max not 0) OnHitMax / OnHitMin fire on reaching or
 * passing the end (once, until the value leaves it again) and the value is clamped; OutValue every time */
static void counter_update(Entity *e, f32 v, s32 activator)
{
	if (e->minv != 0 || e->maxv != 0) {
		if (v >= e->maxv) {
			if (!(e->latch & 1)) {
				e->latch |= 1;
				fire_output(e, "onhitmax", activator);
			}
		} else
			e->latch &= ~1;
		if (v <= e->minv) {
			if (!(e->latch & 2)) {
				e->latch |= 2;
				fire_output(e, "onhitmin", activator);
			}
		} else
			e->latch &= ~2;
		v = v < e->minv ? e->minv : v > e->maxv ? e->maxv : v;
	}
	e->value = v;
	char b[24];
	snprintf(b, sizeof(b), "%g", v);
	fire_output_value(e, "outvalue", activator, b);
}

static void compare_fire(Entity *e, s32 activator)
{
	if (e->value == e->compare)
		fire_output(e, "onequalto", activator);
	else
		fire_output(e, "onnotequalto", activator);
	if (e->value < e->compare)
		fire_output(e, "onlessthan", activator);
	if (e->value > e->compare)
		fire_output(e, "ongreaterthan", activator);
}

/* CBaseButton: how is 0 for use or touch (ButtonUse / ButtonResponseToTouch), 1 the Press input, 2 PressIn,
 * 3 PressOut (CBaseButton::Press). A toggle button (flag 32) pressed in comes back out when used again. */
enum { BTN_USE, BTN_PRESS, BTN_PRESSIN, BTN_PRESSOUT };
static void press_button(Entity *e, s32 activator, int how)
{
	int moving = e->moveState == MV_OPENING || e->moveState == MV_CLOSING;
	int top = e->moveState == MV_OPEN, bottom = e->moveState == MV_CLOSED;
	if ((how == BTN_USE || how == BTN_PRESS) && moving)
		return;
	if (how == BTN_PRESSIN && (e->moveState == MV_OPENING || top))
		return;
	if (how == BTN_PRESSOUT && (e->moveState == MV_CLOSING || bottom))
		return;
	if (e->locked) {
		/* (OnUseLocked: once per half second, and only for use) */
		if (how == BTN_USE && now > e->phaseUntil) {
			fire_output(e, "onuselocked", activator);
			e->phaseUntil = now + 0.5f;
		}
		return;
	}
	if (how == BTN_USE && top && !(e->spawnflags & 32))
		return; /* (pressed in, waiting to come out on its own or staying in) */
	e->toucher = activator;
	if ((how == BTN_USE && top) || (how == BTN_PRESS && top) || (how == BTN_PRESSOUT)) {
		fire_output(e, "onpressed", activator); /* ButtonReturn */
		e->moveState = MV_CLOSING;
		return;
	}
	fire_output(e, "onpressed", activator); /* ButtonActivate */
	e->moveState = MV_OPENING;
}

static void teleport_player_to(const Entity *dest)
{
	memcpy(player->origin, dest->origin, sizeof(player->origin));
	player->pitch = dest->angles[0];
	player->yaw = dest->angles[1];
	memset(player->velocity, 0, sizeof(player->velocity));
}

static Entity *find_named(const char *name)
{
	if (!name || !name[0])
		return NULL;
	for (u32 i = 1; i < numEnts; i++)
		if (!(ents[i].flags & EF_KILLED) && name_matches(&ents[i], name))
			return &ents[i];
	return NULL;
}

const char *Ent_KV(s32 ent, const char *key)
{
	if (ent < 0 || (u32)ent >= numEnts)
		return NULL;
	return kv(&ents[ent], key);
}

void Ent_Input(s32 ent, const char *in, const char *param, s32 activator)
{
	if (ent > 0 && (u32)ent < numEnts)
		input(&ents[ent], in, param ? param : "", activator, 0);
}

void Ent_FireOutput(s32 ent, const char *output, s32 activator)
{
	if (ent > 0 && (u32)ent < numEnts)
		fire_output(&ents[ent], output, activator);
}

f32 Ent_Now(void) { return now; }

const f32 *Ent_Origin(s32 ent)
{
	if (ent < 0 || (u32)ent >= numEnts)
		return NULL;
	return ent == 0 ? player->origin : ents[ent].origin;
}

int Ent_SpeakerRelays(const f32 src[3], f32 (*out)[3], int max)
{
	int n = 0;
	f32 dist[16];
	for (u32 i = 1; i < numEnts; i++) {
		Entity *m = &ents[i];
		if (m->cls != EC_MICROPHONE || !alive(m) || !m->enabled)
			continue;
		const f32 *at = m->origin;
		const char *t = kv(m, "target");
		Entity *te = t && t[0] ? find_named(t) : NULL;
		if (te)
			at = te->origin; /* "measure target": listen there instead of at the microphone */
		f32 range = kvf(m, "maxrange", 240);
		f32 d0 = 0;
		for (int k = 0; k < 3; k++)
			d0 += (src[k] - at[k]) * (src[k] - at[k]);
		if (d0 > range * range)
			continue;
		const char *sp = kv(m, "speakername");
		if (!sp || !sp[0])
			continue;
		for (u32 j = 1; j < numEnts; j++) {
			if (!name_matches(&ents[j], sp))
				continue;
			f32 d = 0;
			for (int k = 0; k < 3; k++)
				d += (ents[j].origin[k] - player->origin[k]) * (ents[j].origin[k] - player->origin[k]);
			/* keep the nearest max speakers */
			int slot = n < max ? n++ : -1;
			if (slot < 0) {
				int worst = 0;
				for (int k = 1; k < max; k++)
					if (dist[k] > dist[worst])
						worst = k;
				if (d >= dist[worst])
					continue;
				slot = worst;
			}
			memcpy(out[slot], ents[j].origin, 12);
			dist[slot] = d;
		}
	}
	return n;
}

int Ent_ActiveCamera(f32 origin[3], f32 angles[3], f32 *fov)
{
	/* Source has a single camera render target: show the camera of the nearest live monitor */
	Entity *best = NULL;
	f32 bestD = 2500.0f * 2500.0f;
	for (u32 i = 1; i < numEnts; i++) {
		Entity *m = &ents[i];
		if (m->cls != EC_FUNC_MONITOR || !alive(m) || m->actor <= 0)
			continue;
		Entity *c = &ents[m->actor];
		if (!alive(c) || !c->enabled)
			continue;
		f32 mn[3], mx[3], d = 0;
		world_bounds(m, mn, mx);
		for (int k = 0; k < 3; k++) {
			f32 v = (mn[k] + mx[k]) * 0.5f - player->origin[k];
			d += v * v;
		}
		if (d < bestD) {
			bestD = d;
			best = c;
		}
	}
	if (!best)
		return 0;
	memcpy(origin, best->origin, 12);
	memcpy(angles, best->angles, 12);
	*fov = best->value > 1 ? best->value : 90;
	return 1;
}

s32 Ent_FindActor(const char *name)
{
	if (!name || !*name)
		return -1;
	if (!strcasecmp(name, "!player"))
		return 0;
	for (u32 i = 1; i < numEnts; i++)
		if (!(ents[i].flags & (EF_KILLED | EF_TEMPLATE)) && ents[i].npcModel >= 0 && name_matches(&ents[i], name))
			return (s32)i;
	for (u32 i = 1; i < numEnts; i++)
		if (!(ents[i].flags & (EF_KILLED | EF_TEMPLATE)) && name_matches(&ents[i], name))
			return (s32)i;
	char cls[64];
	snprintf(cls, sizeof(cls), "npc_%s", name);
	for (u32 i = 1; i < numEnts; i++)
		if (!(ents[i].flags & (EF_KILLED | EF_TEMPLATE)) && ents[i].npcModel >= 0 &&
		    (!strcasecmp(ents[i].classname, cls) || !strcasecmp(ents[i].classname, name)))
			return (s32)i;
	return -1;
}


/* ------------------------------------------------------------------ NPCs through a level change */
/* Source carries the NPCs standing in the landmark's trigger_transition volume into the next map (Alyx walking
 * into Kleiner's lab from Point Insertion). Here they're found again by name in the next map: an NPC already
 * there is left alone; one the map only makes from a template (for a new game) is made, where they arrived. */
#define MAX_CARRY 8
static struct {
	char name[48], cls[32];
	f32 rel[3], yaw;
} npcCarry[MAX_CARRY];
static int numNpcCarry;

void Ent_PrepareCarry(const char *landmark, const f32 from[3])
{
	numNpcCarry = 0;
	if (!landmark || !*landmark)
		return;
	for (u32 v = 1; v < numEnts; v++) {
		Entity *t = &ents[v];
		if (strcmp(t->classname, "trigger_transition") || !t->name || strcasecmp(t->name, landmark))
			continue;
		f32 mn[3], mx[3];
		world_bounds(t, mn, mx);
		for (u32 i = 1; i < numEnts && numNpcCarry < MAX_CARRY; i++) {
			Entity *e = &ents[i];
			Npc *n = e->npcModel >= 0 ? Npc_OfEntity((s32)i) : NULL;
			if (!n || n->dead || n->prop || !e->name || (e->flags & (EF_KILLED | EF_DISABLED | EF_TEMPLATE)))
				continue;
			if (e->origin[0] < mn[0] || e->origin[0] > mx[0] || e->origin[1] < mn[1] || e->origin[1] > mx[1] ||
			    e->origin[2] < mn[2] - 64 || e->origin[2] > mx[2])
				continue;
			snprintf(npcCarry[numNpcCarry].name, sizeof(npcCarry[0].name), "%s", e->name);
			snprintf(npcCarry[numNpcCarry].cls, sizeof(npcCarry[0].cls), "%s", e->classname);
			for (int k = 0; k < 3; k++)
				npcCarry[numNpcCarry].rel[k] = e->origin[k] - from[k];
			npcCarry[numNpcCarry].yaw = n->yaw;
			numNpcCarry++;
		}
	}
}

void Ent_FinishCarry(const f32 to[3])
{
	for (int c = 0; c < numNpcCarry; c++) {
		s32 have = Ent_FindActor(npcCarry[c].name);
		if (have > 0 && !(ents[have].flags & EF_DISABLED))
			continue; /* (the map has them already) */
		/* the maker that makes them from a template */
		s32 maker = -1;
		for (u32 i = 1; i < numEnts && maker < 0; i++) {
			const char *tn = kv(&ents[i], "templatename");
			if (tn && !strcasecmp(tn, npcCarry[c].name) &&
			    (!strcmp(ents[i].classname, "npc_template_maker") || !strcmp(ents[i].classname, "npc_maker")))
				maker = (s32)i;
		}
		if (maker > 0) {
			Ent_Input(maker, "Spawn", "", 0);
			have = Ent_FindActor(npcCarry[c].name);
		}
		if (have <= 0)
			continue;
		ents[have].flags &= ~EF_DISABLED;
		Npc *n = Npc_OfEntity(have);
		f32 p[3] = {to[0] + npcCarry[c].rel[0], to[1] + npcCarry[c].rel[1], to[2] + npcCarry[c].rel[2]};
		if (n)
			Npc_Teleport(n, p, npcCarry[c].yaw);
	}
	numNpcCarry = 0;
}

/* ------------------------------------------------------------------ scripted_sequence */

/* ---- scripted_sequence (CAI_ScriptedSequence and the NPC's SCHED_AISCRIPT tasks). A script grabs its NPC
 * (StartScript), or queues behind the script the NPC is in (m_hNextCine, one deep); the NPC walks / runs /
 * teleports to the mark and faces it, then waits there in the pre-action idle until BeginSequence (if it
 * was only told MoveToPosition) and until every script of the same name is ready too (DelayStart); then
 * OnBeginSequence, the entry, the action (looping while m_bLoopActionSequence), OnEndSequence, the
 * post-action idle for one cycle (or for good with flag 256), OnPostIdleEndSequence; the NPC is let go and
 * the queued script (or m_iszNextScript) takes it. Without flag 4 (repeatable) a script runs once. */
enum { SS_NONE, SS_MOVING, SS_IDLE, SS_ENTRY, SS_PLAY, SS_POST, SS_QUEUED };

static Npc *script_npc(Entity *e)
{
	if (e->actor > 0) {
		Npc *n = Npc_OfEntity(e->actor);
		if (n && !(ents[e->actor].flags & EF_KILLED))
			return n;
	}
	s32 a = Ent_FindActor(kv(e, "m_iszentity"));
	e->actor = a;
	return a > 0 ? Npc_OfEntity(a) : NULL;
}

static int has_anim(const Entity *e, const char *key)
{
	const char *v = kv(e, key);
	return v && v[0];
}

static int is_script(const Entity *e) { return e->cls == EC_SCRIPTED_SEQUENCE && !(e->flags & EF_KILLED); }

/* DelayStart: not ready (1) adds the size of the same-named group to my count; ready (0) takes one off
 * everyone's in the group. A script starts once its count is back to 0. */
static void script_delay(Entity *e, int delay)
{
	if (!e->name || !e->name[0]) {
		e->cineDelay = (s16)delay;
		return;
	}
	for (u32 i = 1; i < numEnts; i++) {
		Entity *o = &ents[i];
		if (!is_script(o) || !o->name || strcasecmp(o->name, e->name))
			continue;
		if (delay)
			e->cineDelay++;
		else
			o->cineDelay--;
	}
}

static void script_play_seq(Entity *e, Npc *n, const char *key, int loop)
{
	f32 d = n && has_anim(e, key) ? Npc_PlaySequence(n, kv(e, key), loop) : -1;
	e->scriptCycle = -1.0f;
	e->phaseUntil = now + (d > 0 ? d : 0.1f);
}

static void script_grab(Entity *e, Npc *n);

/* PostIdleDone: lets the NPC go (CineCleanup), runs the queued script, removes a one-shot script */
static void script_post_done(Entity *e)
{
	Npc *n = script_npc(e);
	if (n && has_anim(e, "m_iszpostidle") && (e->spawnflags & 256) && e->nextCine <= 0) {
		e->phase = SS_POST;
		script_play_seq(e, n, "m_iszpostidle", 0);
		fire_output(e, "onpostidleendsequence", e->actor);
		return;
	}
	s32 next = e->nextCine;
	e->phase = SS_NONE;
	e->nextCine = -1;
	e->cineReady = e->cineStarted = 0;
	e->cineDelay = 0;
	if (!(e->spawnflags & 4))
		e->flags |= EF_KILLED; /* (SUB_Remove: not repeatable) */
	if (n && n->script == (s32)(e - ents)) {
		n->script = -1;
		Npc_ResetSequence(n);
	}
	if (next > 0 && is_script(&ents[next]) && n && (next != (s32)(e - ents) || (e->spawnflags & 4))) {
		Entity *x = &ents[next];
		x->actor = e->actor;
		script_grab(x, n);
	}
	fire_output(e, "onpostidleendsequence", e->actor);
}

/* SequenceDone: the post-action idle when there's one and nothing queued, else PostIdleDone */
static void script_end(Entity *e)
{
	Npc *n = script_npc(e);
	if (n && has_anim(e, "m_iszpostidle") && e->nextCine <= 0) {
		e->phase = SS_POST;
		script_play_seq(e, n, "m_iszpostidle", 0);
		fire_output(e, "onendsequence", e->actor);
		return;
	}
	fire_output(e, "onendsequence", e->actor);
	if (e->phase != SS_NONE) /* (an output may have cancelled it) */
		script_post_done(e);
}

/* TASK_WAIT_FOR_SCRIPT done: OnBeginSequence and the entry or the action */
static void script_action(Entity *e)
{
	Npc *n = script_npc(e);
	e->cineStarted = 1;
	fire_output(e, "onbeginsequence", e->actor);
	if (e->phase == SS_NONE)
		return;
	if (n && has_anim(e, "m_iszentry")) {
		e->phase = SS_ENTRY;
		script_play_seq(e, n, "m_iszentry", 0);
		return;
	}
	if (!n || !has_anim(e, "m_iszplay")) {
		/* (StartSequence with nothing to play: SequenceDone at once) */
		e->phase = SS_PLAY;
		e->phaseUntil = now;
		return;
	}
	e->phase = SS_PLAY;
	script_play_seq(e, n, "m_iszplay", 0);
}

/* at the mark: face it (walk/run/custom/face), plant, then wait in the pre-action idle */
static void script_arrived(Entity *e)
{
	Npc *n = script_npc(e);
	int moveto = (int)kvf(e, "m_fmoveto", 0);
	if (n && e->phase == SS_MOVING && (moveto == 1 || moveto == 2 || moveto == 3)) {
		/* (TASK_PLANT_ON_SCRIPT: right on the mark) */
		f32 y = n->yaw;
		Npc_Teleport(n, e->origin, y);
	}
	if (n && (moveto == 1 || moveto == 2 || moveto == 3 || moveto == 5))
		n->idealYaw = e->angles[1]; /* (TASK_FACE_SCRIPT) */
	if (n && has_anim(e, "m_iszidle"))
		Npc_PlaySequence(n, kv(e, "m_iszidle"), 1);
	e->phase = SS_IDLE;
	e->cineReady = 0;
}

/* StartScript with the NPC in hand */
static void script_grab(Entity *e, Npc *n)
{
	s32 me = (s32)(e - ents);
	if (n->script >= 0 && n->script != me && (u32)n->script < numEnts && is_script(&ents[n->script]) &&
	    ents[n->script].phase != SS_NONE && ents[n->script].phase != SS_QUEUED) {
		/* in another script: queue behind it (kicking out whoever was queued) */
		Entity *cur = &ents[n->script];
		if (cur->nextCine > 0 && cur->nextCine != n->script && cur->nextCine != me) {
			ents[cur->nextCine].phase = SS_NONE;
			ents[cur->nextCine].actor = -1;
		}
		cur->nextCine = me;
		e->phase = SS_QUEUED;
		return;
	}
	if (!has_anim(e, "m_isznextscript"))
		e->nextCine = -1;
	else {
		Entity *nx = find_named(kv(e, "m_isznextscript"));
		e->nextCine = nx && nx->cls == EC_SCRIPTED_SEQUENCE ? (s32)(nx - ents) : -1;
	}
	n->script = me;
	n->followEnt = -1;
	e->cineReady = e->cineStarted = 0;
	int moveto = (int)kvf(e, "m_fmoveto", 0);
	script_delay(e, 1);
	f32 dx = e->origin[0] - ents[e->actor].origin[0], dy = e->origin[1] - ents[e->actor].origin[1];
	if ((moveto == 1 || moveto == 2 || moveto == 3) && dx * dx + dy * dy > 2 * 2) {
		Npc_ResetSequence(n);
		Npc_MoveTo(n, e->origin, moveto == 2, 0);
		e->phase = SS_MOVING;
	} else {
		if (moveto == 4)
			Npc_Teleport(n, e->origin, e->angles[1]);
		else
			Npc_Stop(n);
		script_arrived(e);
	}
}

/* MoveToPosition (begin 0) / BeginSequence (begin 1) / starting at spawn */
static void script_start(Entity *e, int begin)
{
	if (e->flags & EF_KILLED)
		return;
	Npc *n = script_npc(e);
	if (begin)
		e->cineWait = 0;
	if (n && n->script == (s32)(e - ents) && e->phase != SS_NONE && e->phase != SS_QUEUED) {
		/* already ours: on its way or waiting at the mark (BeginSequence lets it go on) */
		if (!begin && (e->phase == SS_PLAY || e->phase == SS_POST || e->phase == SS_ENTRY))
			e->cineWait = 1;
		return;
	}
	if (!begin)
		e->cineWait = 1;
	if (e->phase == SS_QUEUED)
		return;
	if (!n) {
		/* (ScriptThink: no NPC to be found; it keeps looking) */
		e->phase = SS_MOVING;
		e->phaseUntil = now + 1.0f;
		return;
	}
	script_grab(e, n);
}

static void script_cancel(Entity *e, s32 activator)
{
	(void)activator;
	Npc *n = e->actor > 0 ? Npc_OfEntity(e->actor) : NULL;
	int was = e->phase;
	e->phase = SS_NONE;
	e->cineDelay = 0;
	e->cineReady = 0;
	if (n && n->script == (s32)(e - ents)) {
		n->script = -1;
		Npc_Stop(n);
		Npc_ResetSequence(n);
	}
	if (was != SS_NONE || n) {
		fire_output(e, "oncancelsequence", 0);
		if (!e->cineStarted)
			fire_output(e, "oncancelfailedsequence", 0);
	}
	e->cineStarted = 0;
}

static void script_events(Entity *e, Npc *n)
{
	/* animation events in the sequence fire OnScriptEvent01..08 (SCRIPT_EVENT_FIREEVENT) */
	if (!n || !P->seqEvents)
		return;
	u32 gs;
	f32 c = Npc_Cycle(n, &gs);
	for (u32 k = 0; k < P->numSeqEvents; k++) {
		const PackSeqEvent *ev = &P->seqEvents[k];
		if (ev->seq == gs && ev->cycle > e->scriptCycle && ev->cycle <= c) {
			char out[24];
			snprintf(out, sizeof(out), "onscriptevent%02u", ev->option);
			fire_output(e, out, e->actor);
		}
	}
	e->scriptCycle = c;
}

static void script_tick(Entity *e)
{
	Npc *n = (e->phase == SS_MOVING && e->actor <= 0) ? NULL : script_npc(e);
	if (e->phase != SS_QUEUED && e->phase != SS_NONE && n && n->script != (s32)(e - ents)) {
		/* someone took the NPC away (it died, a scene or the AI let it go) */
		e->phase = SS_NONE;
		return;
	}
	switch (e->phase) {
	case SS_MOVING:
		if (!n) {
			if (now >= e->phaseUntil) {
				n = script_npc(e);
				if (n)
					script_grab(e, n);
				else
					e->phaseUntil = now + 1.0f;
			}
		} else if (!n->moving)
			script_arrived(e);
		break;
	case SS_IDLE:
		/* TASK_ENABLE_SCRIPT once BeginSequence has come, then TASK_WAIT_FOR_SCRIPT for the group */
		if (!e->cineReady && !e->cineWait) {
			e->cineReady = 1;
			script_delay(e, 0);
		}
		if (e->cineReady && e->cineDelay <= 0)
			script_action(e);
		break;
	case SS_ENTRY:
		script_events(e, n);
		if (now >= e->phaseUntil) {
			e->phase = SS_PLAY;
			script_play_seq(e, n, "m_iszplay", 0);
		}
		break;
	case SS_PLAY:
		script_events(e, n);
		if (now >= e->phaseUntil) {
			if (kvf(e, "m_bloopactionsequence", 0) != 0 && n && has_anim(e, "m_iszplay"))
				script_play_seq(e, n, "m_iszplay", 0); /* (FinishedActionSequence: keep looping) */
			else
				script_end(e);
		}
		break;
	case SS_POST:
		/* TASK_PLAY_SCRIPT_POST_IDLE: one cycle, or until another script has queued for the NPC */
		if (now >= e->phaseUntil || e->nextCine > 0)
			script_post_done(e);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ followers and sounds */

static void follow_goal(Entity *e, int on)
{
	const char *who = kv(e, "actor");
	const char *goal = kv(e, "goal");
	s32 target = 0;
	if (goal && goal[0] && strcasecmp(goal, "!player")) {
		Entity *g = find_named(goal);
		target = g ? (s32)(g - ents) : 0;
	}
	for (u32 i = 1; i < numEnts; i++) {
		if (ents[i].npcModel < 0 || (ents[i].flags & EF_KILLED))
			continue;
		if (who && who[0] && !name_matches(&ents[i], who) && strcasecmp(ents[i].classname, who))
			continue;
		Npc *n = Npc_OfEntity((s32)i);
		if (n)
			Npc_Follow(n, on ? target : -1, 96.0f);
	}
}

static void ambient_play(Entity *e)
{
	if (e->sound < 0 || (u32)e->sound >= P->numSounds)
		return;
	if (e->voice)
		Audio_Stop(e->voice);
	f32 vol = kvf(e, "health", 10) / 10.0f;
	f32 radius = kvf(e, "radius", 1250);
	/* Source: ComputeSoundlevel - 40 dB at the reference distance plus the loss over the radius */
	f32 level = (radius > 0 && !(e->spawnflags & 1)) ? 40.0f + 20.0f * log10f(radius / 36.0f) : 0.0f;
	const f32 *pos = e->origin;
	const char *src = kv(e, "sourceentityname");
	if (src && src[0]) {
		Entity *s2 = find_named(src);
		if (s2)
			pos = s2->origin;
	}
	e->voice = Audio_Play(&P->sounds[e->sound], (e->spawnflags & 1) ? NULL : pos, vol, level);
}

/* ------------------------------------------------------------------ trains, cameras, fades, shakes */

static s32 activeView = -1;
static f32 viewUntil, viewSnap;
/* script_intro (CScriptIntro + C_ScriptIntro): the G-Man intro's second view, blend modes, FOV and fades */
static struct {
	s32 ent;                       /* the active script_intro, -1 none */
	s32 camera;                    /* SetCameraViewEntity, -1 none */
	s32 blendMode, nextBlendMode, queuedBlendMode, queuedNextBlendMode;
	f32 nextBlendTime, blendStartTime;
	s32 fov, nextFov, startFov;
	f32 nextFovBlendTime, fovBlendStartTime, viewFov;
	u8 alternateFov;
	f32 fadeColor[3], fadeAlpha, fadeDuration;
	f32 prevFadeAlpha, fadeStartTime, fadeStartAlpha, curFadeAlpha;
} intro;

static f32 fadeStart, fadeDur, fadeHold, fadeR, fadeG, fadeB, fadeA;
static u8 fadeIn, fadeStay, fadeOn;
static f32 shakeAmp, shakeUntil, shakeStart;
static f32 shakeOrigin[3], shakeRadius;
static f32 zoomFrom, zoomTo, zoomStart, zoomRate;
static u8 zoomOn;

f32 Ent_ZoomFov(void)
{
	if (!zoomOn)
		return 0;
	f32 t = zoomRate > 0 ? (now - zoomStart) / zoomRate : 1;
	if (t > 1)
		t = 1;
	f32 f = zoomFrom + (zoomTo - zoomFrom) * t;
	if (t >= 1 && zoomTo == 0)
		zoomOn = 0;
	return f;
}

static s32 node_named(const char *name)
{
	Entity *n = find_named(name);
	return n ? (s32)(n - ents) : -1;
}

/* CPathTrack::GetNext / GetPrevious: the alternate path ("altpath") when it's switched on (flag 0x8000;
 * with flag 4 it's the way back instead), and a disabled track (flag 1) is the end of the line
 * (ValidPath in LookAhead). The previous track is whichever links here, by its target or its altpath,
 * except my own alternate (SetPrevious). */
static s32 node_link(s32 node, int backward)
{
	Entity *n = &ents[node];
	const char *alt = n->cls == EC_PATH ? kv(n, "altpath") : NULL;
	int altOn = alt && alt[0] && (n->spawnflags & 0x8000);
	if (altOn && !backward == !(n->spawnflags & 4)) {
		s32 a = node_named(alt);
		if (a > 0)
			return a;
	}
	if (!backward)
		return node_named(kv(n, "target"));
	s32 prev = -1;
	for (u32 i = 1; i < numEnts; i++) {
		if (ents[i].cls != EC_PATH || (s32)i == node)
			continue;
		const char *t = kv(&ents[i], "target"), *ta = kv(&ents[i], "altpath");
		if ((t && name_matches(n, t)) || (ta && ta[0] && name_matches(n, ta))) {
			if (alt && alt[0] && name_matches(&ents[i], alt))
				continue;
			prev = (s32)i;
		}
	}
	return prev;
}

static s32 node_next(s32 node, int backward)
{
	if (node <= 0)
		return -1;
	s32 nx = node_link(node, backward);
	if (nx > 0 && ents[nx].cls == EC_PATH && (ents[nx].spawnflags & 1) && !strcmp(ents[nx].classname, "path_track"))
		return -1;
	return nx;
}

/* the player shouldn't end up inside a moving car's walls or seats: nudge him back into free space */
static void unstick_player(void)
{
	f32 mn[3], mx[3];
	Player_Bounds(player, mn, mx);
	Trace tr;
	CM_BoxTrace(&tr, player->origin, player->origin, mn, mx, MASK_PLAYERSOLID);
	if (!tr.startsolid || player->noclip)
		return;
	static const s8 nudge[][3] = {{0, 0, 2}, {0, 0, 6}, {0, 0, 12}, {0, 0, 18}, {8, 0, 2}, {-8, 0, 2}, {0, 8, 2}, {0, -8, 2},
	                              {16, 0, 4}, {-16, 0, 4}, {0, 16, 4}, {0, -16, 4}, {24, 24, 4}, {-24, 24, 4}, {24, -24, 4},
	                              {-24, -24, 4}, {32, 0, 8}, {-32, 0, 8}, {0, 32, 8}, {0, -32, 8}};
	for (u32 k = 0; k < sizeof(nudge) / sizeof(nudge[0]); k++) {
		f32 p[3] = {player->origin[0] + nudge[k][0], player->origin[1] + nudge[k][1], player->origin[2] + nudge[k][2]};
		CM_BoxTrace(&tr, p, p, mn, mx, MASK_PLAYERSOLID);
		if (!tr.startsolid) {
			memcpy(player->origin, p, 12);
			return;
		}
	}
}

/* ---- pushers (Source's SV_PushMove / CPhysicsPushedEntities): whatever stands on a door, a lift or a
 * train moves with it (by the ground it stands on, not by being somewhere inside its box), and what a
 * moving brush runs into is shoved out of its way by the distance it moved */
#define MAX_MOVERS 512
static u16 moverEnt[MAX_MOVERS];
static f32 moverO[MAX_MOVERS][3], moverR[MAX_MOVERS][3][3];
static u32 numMovers;
static s16 *moverSlot; /* per entity: its mover slot, -1 none */
static f32 crushNext;

static int can_move(const Entity *e)
{
	switch (e->cls) {
	case EC_FUNC_DOOR:
	case EC_FUNC_DOOR_ROTATING:
	case EC_FUNC_MOVELINEAR:
	case EC_FUNC_BUTTON:
	case EC_PROP_DOOR:
	case EC_TRACKTRAIN:
	case EC_ROTATING:
		return 1;
	default:
		return e->parent > 0 && (e->bmodel > 0 || e->mdl >= 0);
	}
}

static void movers_init(void)
{
	free(moverSlot);
	moverSlot = malloc(numEnts * sizeof(s16));
	numMovers = 0;
	for (u32 i = 0; moverSlot && i < numEnts; i++) {
		moverSlot[i] = -1;
		if (i > 0 && numMovers < MAX_MOVERS && can_move(&ents[i]) && (ents[i].bmodel > 0 || ents[i].mdl >= 0)) {
			moverSlot[i] = (s16)numMovers;
			moverEnt[numMovers++] = (u16)i;
		}
	}
}

static void movers_snapshot(void)
{
	for (u32 k = 0; k < numMovers; k++) {
		const Entity *e = &ents[moverEnt[k]];
		memcpy(moverO[k], e->origin, 12);
		memcpy(moverR[k], e->R, sizeof(e->R));
	}
}

/* (a teleport isn't a push: nothing gets shoved by it) */
static void mover_resnap(const Entity *e)
{
	s32 k = moverSlot ? moverSlot[e - ents] : -1;
	if (k >= 0) {
		memcpy(moverO[k], e->origin, 12);
		memcpy(moverR[k], e->R, sizeof(e->R));
	}
}

/* where a point riding mover slot k is now; returns 0 if it didn't move */
static int mover_xform(u32 k, const f32 p[3], f32 out[3], f32 *dyaw)
{
	const Entity *e = &ents[moverEnt[k]];
	const f32(*R0)[3] = (const f32(*)[3])moverR[k];
	int moved = 0;
	for (int a = 0; a < 3; a++) {
		if (fabsf(e->origin[a] - moverO[k][a]) > 0.0001f)
			moved = 1;
		for (int b = 0; b < 3; b++)
			if (fabsf(e->R[a][b] - R0[a][b]) > 0.00001f)
				moved = 1;
	}
	if (!moved)
		return 0;
	/* p' = O1 + R1 R0^T (p - O0) */
	f32 l[3], d[3] = {p[0] - moverO[k][0], p[1] - moverO[k][1], p[2] - moverO[k][2]};
	for (int a = 0; a < 3; a++)
		l[a] = R0[0][a] * d[0] + R0[1][a] * d[1] + R0[2][a] * d[2];
	for (int a = 0; a < 3; a++)
		out[a] = e->origin[a] + e->R[a][0] * l[0] + e->R[a][1] * l[1] + e->R[a][2] * l[2];
	if (dyaw) {
		f32 y0 = atan2f(R0[1][0], R0[0][0]), y1 = atan2f(e->R[1][0], e->R[0][0]);
		*dyaw = fmodf((y1 - y0) * 180.0f / (f32)M_PI + 540.0f, 360.0f) - 180.0f;
	}
	return 1;
}

static void crush_damage(const Entity *e)
{
	f32 dmg = kvf(e, "dmg", 0);
	if (dmg > 0 && now >= crushNext && (cbs.hurtPlayerFrom || cbs.hurtPlayer)) {
		if (cbs.hurtPlayerFrom)
			cbs.hurtPlayerFrom(dmg, 1 /* DMG_CRUSH */, e->origin);
		else
			cbs.hurtPlayer(dmg);
		crushNext = now + 0.5f;
	}
}

void Ent_PlayerCarry(f32 out[3])
{
	memcpy(out, playerCarry, 12);
}

static void push_movers(void)
{
	if (!moverSlot || !numMovers)
		return;
	/* riders: the player, by what he stands on */
	f32 np[3], dy = 0;
	s32 g = player->onGround ? player->groundEnt : -1;
	if (g > 0 && (u32)g < numEnts && moverSlot[g] >= 0 && !player->noclip &&
	    mover_xform((u32)moverSlot[g], player->origin, np, &dy)) {
		for (int a = 0; a < 3; a++)
			playerCarry[a] += np[a] - player->origin[a];
		memcpy(player->origin, np, 12);
		player->yaw += dy;
	} else {
		g = -1;
	}
	/* NPCs, by theirs */
	u32 nn;
	Npc *L = Npc_List(&nn);
	for (u32 i = 0; i < nn; i++) {
		Npc *n = &L[i];
		Entity *o = &ents[n->ent];
		if (!alive(o) || o->parent > 0 || n->groundEnt <= 0 || (u32)n->groundEnt >= numEnts || moverSlot[n->groundEnt] < 0)
			continue;
		if (mover_xform((u32)moverSlot[n->groundEnt], o->origin, np, &dy)) {
			memcpy(o->origin, np, 12);
			n->yaw += dy;
			n->idealYaw += dy;
		}
	}
	/* blockers: what a moved solid now overlaps is shoved along by the distance that part of it moved */
	f32 pm[3], px[3];
	Player_Bounds(player, pm, px);
	for (u32 k = 0; k < numMovers; k++) {
		const Entity *e = &ents[moverEnt[k]];
		if (!(e->flags & EF_SOLID) || !alive(e))
			continue;
		f32 mid[3] = {player->origin[0], player->origin[1], player->origin[2] + (pm[2] + px[2]) * 0.5f}, to[3];
		/* (not by the mover he rides: he has moved with it already, and a second shove made him jitter) */
		if (!player->noclip && moverEnt[k] != g && mover_xform(k, mid, to, NULL)) {
			Trace tr;
			CM_TraceOnly(&tr, player->origin, player->origin, pm, px, MASK_PLAYERSOLID, moverEnt[k]);
			if (tr.startsolid) {
				f32 d[3] = {to[0] - mid[0], to[1] - mid[1], to[2] - mid[2]};
				f32 len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				f32 dest[3];
				for (int a = 0; a < 3; a++) /* (and a little more, clear of the face) */
					dest[a] = player->origin[a] + d[a] + (len > 0.01f ? d[a] / len * 0.5f : 0);
				Trace t2;
				memcpy(player->origin, dest, 12);
				for (int a = 0; a < 3; a++) /* the push carries his momentum too */
					if (fabsf(d[a]) > 0.01f && TICK_INTERVAL > 0) {
						f32 v = d[a] / TICK_INTERVAL;
						if ((v > 0 && player->velocity[a] < v) || (v < 0 && player->velocity[a] > v))
							player->velocity[a] = v;
					}
				CM_BoxTrace(&t2, player->origin, player->origin, pm, px, MASK_PLAYERSOLID);
				if (t2.startsolid)
					crush_damage(e); /* (pinned against something: unstick_player sorts out where he ends up) */
			}
		}
		f32 emn[3], emx[3];
		int wb = 0;
		for (u32 i = 0; i < nn; i++) {
			Npc *n = &L[i];
			Entity *o = &ents[n->ent];
			if (!alive(o) || o->parent > 0 || o->npcModel < 0)
				continue;
			const PackNpcModel *m = &P->npcModels[o->npcModel];
			f32 h = m->height > 72 ? 72 : m->height;
			f32 nm[3] = {-13, -13, 0}, nx[3] = {13, 13, h};
			if (!wb) {
				world_bounds(e, emn, emx);
				wb = 1;
			}
			if (o->origin[0] + 13 < emn[0] || o->origin[0] - 13 > emx[0] || o->origin[1] + 13 < emn[1] ||
			    o->origin[1] - 13 > emx[1] || o->origin[2] + h < emn[2] || o->origin[2] > emx[2])
				continue;
			f32 c[3] = {o->origin[0], o->origin[1], o->origin[2] + h * 0.5f};
			if (!mover_xform(k, c, to, NULL))
				break; /* (didn't move: no NPC to push either) */
			Trace tr;
			CM_TraceOnly(&tr, o->origin, o->origin, nm, nx, MASK_NPCSOLID, moverEnt[k]);
			if (!tr.startsolid)
				continue;
			for (int a = 0; a < 3; a++)
				o->origin[a] += to[a] - c[a];
		}
	}
}

static int inside_box(const f32 p[3], const f32 mn[3], const f32 mx[3])
{
	return p[0] >= mn[0] - 4 && p[0] <= mx[0] + 4 && p[1] >= mn[1] - 4 && p[1] <= mx[1] + 4 && p[2] >= mn[2] - 4 &&
	       p[2] <= mx[2] + 4;
}

/* something standing on / riding inside a moving entity moves with it */
static void carry(const Entity *e, const f32 d[3])
{
	f32 mn[3], mx[3];
	world_bounds(e, mn, mx);
	f32 pm[3], px[3];
	Player_Bounds(player, pm, px);
	int in = 1;
	for (int k = 0; k < 2; k++)
		if (player->origin[k] < mn[k] - 4 || player->origin[k] > mx[k] + 4)
			in = 0;
	if (player->origin[2] < mn[2] - 4 || player->origin[2] > mx[2] + 4)
		in = 0;
	if (in)
		for (int k = 0; k < 3; k++) {
			player->origin[k] += d[k];
			playerCarry[k] += d[k];
		}
	for (u32 i = 1; i < numEnts; i++) {
		Entity *o = &ents[i];
		if (o->npcModel < 0 || !alive(o) || o->parent > 0)
			continue; /* (parented ones follow the train already) */
		if (o->origin[0] < mn[0] || o->origin[0] > mx[0] || o->origin[1] < mn[1] || o->origin[1] > mx[1] ||
		    o->origin[2] < mn[2] - 4 || o->origin[2] > mx[2])
			continue;
		for (int k = 0; k < 3; k++)
			o->origin[k] += d[k];
	}
}

/* arrive: Find at spawn (ArriveAtNode: OnPass, and a no-control train takes the track's speed);
 * otherwise TeleportToPathTrack (the teleport input, a teleport track): only InTeleport */
static void train_teleport(Entity *e, s32 node, int arrive)
{
	if (node <= 0)
		return;
	f32 h = kvf(e, "height", 0);
	f32 d[3];
	for (int k = 0; k < 3; k++)
		d[k] = ents[node].origin[k] + (k == 2 ? h : 0) - e->origin[k];
	carry(e, d);
	for (int k = 0; k < 3; k++)
		e->origin[k] += d[k];
	mover_resnap(e);
	e->lastNode = node;
	e->pathNode = node_next(node, e->minv < 0);
	if (!arrive) {
		fire_output(&ents[node], "onteleport", (s32)(e - ents)); /* (InTeleport) */
		return;
	}
	Entity *n = &ents[node];
	fire_output(n, "onpass", (s32)(e - ents));
	if (n->spawnflags & 8)
		e->spawnflags |= 2;
	f32 ns = kvf(n, "speed", 0);
	if ((e->spawnflags & 2) && ns != 0)
		e->trainSpeed = fabsf(ns) * (e->minv < 0 ? -1 : 1);
}

/* func_tracktrain "at path tracks" (CFuncTrackTrain::UpdateOrientationAtPathTracks): the car faces from
 * where it is to the point its "wheels" distance further along the path, so it swings round corners
 * smoothly instead of turning all at once at each path_track (which jolted whoever rode it) */
static void wheels_orient(Entity *e, f32 height)
{
	f32 L = kvf(e, "wheels", 50);
	if (L < 1)
		L = 1;
	int back = e->trainSpeed < 0;
	f32 at[3] = {e->origin[0], e->origin[1], e->origin[2]}, ahead[3];
	memcpy(ahead, at, 12);
	s32 node = e->pathNode;
	f32 left = L;
	for (int guard = 0; node > 0 && left > 0 && guard < 16; guard++) {
		f32 g[3] = {ents[node].origin[0], ents[node].origin[1], ents[node].origin[2] + height};
		f32 d[3] = {g[0] - at[0], g[1] - at[1], g[2] - at[2]};
		f32 dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dist >= left) {
			for (int k = 0; k < 3; k++)
				ahead[k] = at[k] + d[k] / dist * left;
			left = 0;
			break;
		}
		memcpy(at, g, 12);
		memcpy(ahead, g, 12);
		left -= dist;
		node = node_next(node, back);
	}
	f32 f[3] = {ahead[0] - e->origin[0], ahead[1] - e->origin[1], ahead[2] - e->origin[2]};
	f32 hz = sqrtf(f[0] * f[0] + f[1] * f[1]);
	if (hz < 1)
		return;
	e->angles[1] = atan2f(f[1], f[0]) * 180.0f / (f32)M_PI + (back ? 180 : 0);
	e->angles[0] = (e->spawnflags & 1) ? 0 : (back ? 1 : -1) * atan2f(f[2], hz) * 180.0f / (f32)M_PI;
	Ent_AngleMatrix(e->angles, e->R); /* (riders turn with it in push_movers) */
}

/* move along the path at speed; returns 0 when there's nowhere further to go */
static int path_move(Entity *e, f32 dt, f32 height, int orient)
{
	f32 left = fabsf(e->trainSpeed) * dt;
	int guard = 0;
	while (left > 0 && e->pathNode > 0 && guard++ < 8) {
		Entity *n = &ents[e->pathNode];
		f32 goal[3] = {n->origin[0], n->origin[1], n->origin[2] + height};
		f32 d[3] = {goal[0] - e->origin[0], goal[1] - e->origin[1], goal[2] - e->origin[2]};
		f32 dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (orient >= 2 && e->cls == EC_TRACKTRAIN) {
			/* linear blend (2) / ease in-out (3), like CFuncTrackTrain::UpdateOrientationBlend: from the
			 * angles the train had at the last path_track to the next one's (its own angles if it says
			 * "face this path_track's angles", else the direction of travel, pitch included unless the
			 * train has "no pitch") */
			if (e->moveLen <= 0) {
				e->moveLen = dist > 1 ? dist : 1;
				memcpy(e->ang1, e->angles, 12);
			}
			f32 want[3] = {0, 0, 0};
			if ((int)kvf(n, "orientationtype", 1) == 2) {
				memcpy(want, n->angles, 12);
			} else {
				f32 hz = sqrtf(d[0] * d[0] + d[1] * d[1]);
				if (hz > 1 || fabsf(d[2]) > 1) {
					want[1] = atan2f(d[1], d[0]) * 180.0f / (f32)M_PI + (e->trainSpeed < 0 ? 180 : 0);
					if (!(e->spawnflags & 1))
						want[0] = -atan2f(d[2], hz) * 180.0f / (f32)M_PI;
				} else {
					memcpy(want, e->angles, 12);
				}
			}
			f32 f = 1.0f - dist / e->moveLen;
			f = f < 0 ? 0 : f > 1 ? 1 : f;
			if (orient == 3)
				f = f * f * (3 - 2 * f);
			f32 old = e->angles[1];
			for (int k = 0; k < 3; k++) {
				f32 dd = fmodf(want[k] - e->ang1[k] + 540.0f, 360.0f) - 180.0f;
				e->angles[k] = e->ang1[k] + dd * f;
			}
			Ent_AngleMatrix(e->angles, e->R); /* (riders turn with it in push_movers) */
			(void)old;
		} else if (orient == 1 && e->cls == EC_TRACKTRAIN) {
			/* (at path tracks: set from the wheels below, after moving) */
		} else if (orient && (d[0] * d[0] + d[1] * d[1]) > 1) {
			f32 ny = atan2f(d[1], d[0]) * 180.0f / (f32)M_PI + (e->trainSpeed < 0 ? 180 : 0);
			f32 dy = ny - e->angles[1];
			dy = fmodf(dy + 540.0f, 360.0f) - 180.0f;
			(void)dy; /* (whoever is inside turns with the car in push_movers) */
			e->angles[1] = ny;
			Ent_AngleMatrix(e->angles, e->R);
		}
		f32 step = left < dist ? left : dist;
		f32 mv[3] = {0, 0, 0};
		if (dist > 0.001f)
			for (int k = 0; k < 3; k++)
				mv[k] = d[k] / dist * step;
		for (int k = 0; k < 3; k++)
			e->origin[k] += mv[k];
		left -= step;
		if (step >= dist - 0.01f) {
			/* reached the node */
			s32 at = e->pathNode;
			e->moveLen = 0; /* (a new segment: the orientation blend starts again from here) */
			fire_output(n, "onpass", (s32)(e - ents));
			if (e->cls == EC_TRACKTRAIN) {
				/* (ArriveAtNode: "disable train" takes the controls away; a train without player
				 * control takes the track's speed, keeping its direction) */
				e->lastNode = at;
				if (n->spawnflags & 8)
					e->spawnflags |= 2;
				f32 ns = kvf(n, "speed", 0);
				if ((e->spawnflags & 2) && ns != 0) {
					e->latch &= ~8;
					e->trainSpeed = fabsf(ns) * (e->minv < 0 ? -1 : 1);
				}
				fire_output(e, "onnextpoint", at);
			} else {
				f32 ns = kvf(n, "speed", 0);
				if (ns > 0)
					e->trainSpeed = e->trainSpeed < 0 ? -ns : ns;
			}
			if (e->pathNode == at) /* an output may have teleported us */
				e->pathNode = node_next(at, e->trainSpeed < 0);
			if (e->cls == EC_TRACKTRAIN && e->pathNode > 0 && (ents[e->pathNode].spawnflags & 16) &&
			    !strcmp(ents[e->pathNode].classname, "path_track")) {
				train_teleport(e, e->pathNode, 0); /* (the next one's a teleport: jump there) */
				return e->pathNode > 0;
			}
			if (e->pathNode <= 0)
				return 0;
		}
	}
	if (orient == 1 && e->cls == EC_TRACKTRAIN && e->pathNode > 0)
		wheels_orient(e, height);
	return e->pathNode > 0;
}

/* CFuncTrackTrain::UpdateTrainVelocity: SetSpeedDirAccel approaches its speed at the manual accel/decel
 * rates; otherwise "linear blend" (1) / "ease in-out" (2) velocity blends between the speeds of the track
 * it passed and the one it's heading for */
static void train_velocity(Entity *e, f32 dt)
{
	int vt = (int)kvf(e, "velocitytype", 0);
	f32 dir = e->minv < 0 ? -1.0f : 1.0f;
	if (vt == 0)
		return;
	if (e->latch & 8) {
		if (e->trainSpeed != e->value) {
			f32 r = fabsf(e->value) > fabsf(e->trainSpeed) ? kvf(e, "manualaccelspeed", 0) : kvf(e, "manualdecelspeed", 0);
			f32 st = r * dt, d = e->value - e->trainSpeed;
			e->trainSpeed = fabsf(d) <= st ? e->value : e->trainSpeed + (d > 0 ? st : -st);
		}
		return;
	}
	if (e->lastNode <= 0 || e->pathNode <= 0)
		return;
	Entity *pp = &ents[e->lastNode], *pn = &ents[e->pathNode];
	f32 ps = fabsf(e->trainSpeed), a = kvf(pp, "speed", 0), b = kvf(pn, "speed", 0);
	if (a != 0)
		ps = a;
	f32 ns = b != 0 ? b : ps;
	if (ps != ns) {
		f32 seg = 0, cur = 0, h = kvf(e, "height", 0);
		for (int k = 0; k < 3; k++) {
			f32 s1 = pn->origin[k] - pp->origin[k], c1 = e->origin[k] - (k == 2 ? h : 0) - pp->origin[k];
			seg += s1 * s1;
			cur += c1 * c1;
		}
		if (seg > 0) {
			f32 p = sqrtf(cur / seg);
			p = p > 1 ? 1 : p;
			if (vt == 2)
				p = p * p * (3 - 2 * p); /* (SimpleSplineRemapVal) */
			e->trainSpeed = dir * (ps * (1 - p) + ns * p);
		}
	} else
		e->trainSpeed = dir * ps;
	(void)dt;
}

static void train_tick(Entity *e, f32 dt)
{
	if (e->trainSpeed == 0 || e->pathNode <= 0)
		return;
	train_velocity(e, dt);
	if (e->trainSpeed == 0)
		return;
	if (!path_move(e, dt, kvf(e, "height", 0), (int)kvf(e, "orientationtype", 1))) {
		/* (the end of the line: DeadEnd; Resume would carry on at this speed) */
		e->maxv = e->trainSpeed;
		e->trainSpeed = 0;
		e->latch &= ~8;
		if (e->lastNode > 0)
			fire_output(&ents[e->lastNode], "onpass", (s32)(e - ents));
		fire_output(e, "onstop", 0);
	}
}

/* CFuncTrackTrain::SetSpeed: the speed in the current direction (minv: m_dir); starting fires OnStart,
 * stopping remembers the speed for Resume (maxv: m_oldSpeed) */
s32 Ent_PathNext(s32 node, int backward) { return node_next(node, backward); }

static void train_set_speed(Entity *e, f32 sp, int accel)
{
	f32 dir = e->minv < 0 ? -1.0f : 1.0f;
	if (accel) {
		e->latch |= 8;
		e->value = fabsf(sp) * dir;
		if (e->trainSpeed == 0 && e->value != 0)
			e->trainSpeed = 0.1f * dir; /* (a little push to get going) */
		if (e->pathNode <= 0 && e->lastNode > 0)
			e->pathNode = node_next(e->lastNode, dir < 0);
		fire_output(e, "onstart", (s32)(e - ents));
		return;
	}
	e->latch &= ~8;
	f32 old = e->trainSpeed;
	e->trainSpeed = fabsf(sp) * dir;
	if (e->trainSpeed == old)
		return;
	if (e->trainSpeed != 0) {
		if (e->pathNode <= 0 && e->lastNode > 0)
			e->pathNode = node_next(e->lastNode, dir < 0);
		if (old == 0)
			fire_output(e, "onstart", (s32)(e - ents));
	} else
		e->maxv = old;
}

/* SetDirForward: turning round heads back to the track it passed (or, sitting on it, the one before) */
static void train_set_dir(Entity *e, int forward)
{
	f32 nd = forward ? 1.0f : -1.0f;
	if ((e->minv < 0 ? -1.0f : 1.0f) == nd)
		return;
	e->minv = nd;
	if (e->lastNode > 0) {
		const Entity *L = &ents[e->lastNode];
		f32 h = kvf(e, "height", 0), d2 = 0;
		for (int k = 0; k < 3; k++) {
			f32 dd = e->origin[k] - (k == 2 ? h : 0) - L->origin[k];
			d2 += dd * dd;
		}
		if (d2 < 1.0f)
			e->pathNode = node_next(e->lastNode, nd < 0);
		else {
			s32 t = e->pathNode;
			e->pathNode = e->lastNode;
			e->lastNode = t;
		}
	}
	e->moveLen = 0;
}

static void view_disable(void)
{
	if (activeView > 0) {
		fire_output(&ents[activeView], "onendfollow", 0);
		Entity *v = &ents[activeView];
		v->enabled = 0;
	}
	activeView = -1;
}

static void view_tick(f32 dt)
{
	if (activeView <= 0)
		return;
	Entity *v = &ents[activeView];
	if (!(v->spawnflags & 8) && now >= viewUntil) {
		view_disable();
		return;
	}
	if (v->pathNode > 0 && v->trainSpeed > 0)
		path_move(v, dt, 0, 0);
	/* look at the target (or an attachment on it), turning slowly unless snapping */
	const char *t = kv(v, "target");
	Entity *te = t && t[0] ? find_named(t) : NULL;
	if (te) {
		f32 goal[3];
		memcpy(goal, te->origin, 12);
		const char *att = kv(v, "targetattachment");
		Npc *n = Npc_OfEntity((s32)(te - ents));
		f32 R[3][3];
		if (att && att[0] && n)
			Npc_Attachment(n, att, goal, R);
		f32 d[3] = {goal[0] - v->origin[0], goal[1] - v->origin[1], goal[2] - v->origin[2]};
		f32 yaw = atan2f(d[1], d[0]) * 180.0f / (f32)M_PI;
		f32 pitch = -atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * 180.0f / (f32)M_PI;
		if (viewSnap) {
			v->angles[0] = pitch;
			v->angles[1] = yaw;
			viewSnap = 0;
		} else {
			f32 dx = pitch - v->angles[0], dy = yaw - v->angles[1];
			while (dy > 180)
				dy -= 360;
			while (dy < -180)
				dy += 360;
			f32 k = 40.0f * dt * dt * 66.0f; /* Source: angular velocity = delta * 40 * frametime */
			if (k > 1)
				k = 1;
			v->angles[0] += dx * k;
			v->angles[1] += dy * k;
		}
	}
}

int Ent_ViewOverride(f32 origin[3], f32 angles[3])
{
	if (activeView <= 0)
		return 0;
	const Entity *v = &ents[activeView];
	memcpy(origin, v->origin, 12);
	memcpy(angles, v->angles, 12);
	return 1;
}

/* the player's eye and view direction */
void Ent_PlayerView(f32 eye[3], f32 fwd[3])
{
	if (!player) {
		memset(eye, 0, 12);
		fwd[0] = 1, fwd[1] = fwd[2] = 0;
		return;
	}
	memcpy(eye, player->origin, 12);
	eye[2] += 64.0f - player->duckAmount * 36.0f;
	f32 ang[3] = {player->pitch, player->yaw, 0}, R[3][3];
	Ent_AngleMatrix(ang, R);
	for (int k = 0; k < 3; k++)
		fwd[k] = R[k][0];
}

/* UTIL_ScreenFade(..., FFADE_IN) from game code (a scanner's photo flash): from the colour to clear */
void Ent_FlashScreen(u8 r, u8 g, u8 b, f32 alpha, f32 fadeTime)
{
	fadeOn = 1;
	fadeIn = 1;
	fadeStay = 0;
	fadeR = r;
	fadeG = g;
	fadeB = b;
	fadeA = alpha;
	fadeDur = fadeTime;
	fadeHold = 0;
	fadeStart = now;
}

u32 Ent_ScreenFade(void)
{
	if (!fadeOn)
		return 0;
	f32 t = now - fadeStart, a;
	if (fadeIn) {
		/* from the colour to clear over the duration (Source: the hold time doesn't delay a fade in) */
		if (t < fadeDur)
			a = 1 - t / (fadeDur > 0 ? fadeDur : 1);
		else {
			fadeOn = 0;
			return 0;
		}
	} else {
		if (t < fadeDur)
			a = t / (fadeDur > 0 ? fadeDur : 1);
		else if (fadeStay || t < fadeDur + fadeHold)
			a = 1;
		else {
			fadeOn = 0;
			return 0;
		}
	}
	a *= fadeA;
	return (u32)fadeR << 24 | (u32)fadeG << 16 | (u32)fadeB << 8 | (u32)(a * 255);
}

void Ent_ShakeAdd(f32 amp, f32 duration)
{
	if (now < shakeUntil && Ent_ShakeAmount() > amp)
		return;
	shakeAmp = amp;
	shakeStart = now;
	shakeUntil = now + duration;
}

f32 Ent_ShakeAmount(void)
{
	if (now >= shakeUntil)
		return 0;
	f32 amp = shakeAmp * (1 - (now - shakeStart) / (shakeUntil - shakeStart));
	if (shakeRadius > 0) {
		f32 d = 0;
		for (int k = 0; k < 3; k++)
			d += (player->origin[k] - shakeOrigin[k]) * (player->origin[k] - shakeOrigin[k]);
		d = sqrtf(d);
		if (d > shakeRadius)
			return 0;
		amp *= 1 - d / shakeRadius;
	}
	return amp;
}

static f32 intro_calc_fov(void)
{
	f32 span = intro.nextFovBlendTime - intro.fovBlendStartTime;
	if (intro.alternateFov) {
		f32 d = span != 0 ? (now - intro.fovBlendStartTime) / span : 1;
		if (d >= 1)
			return (f32)intro.nextFov;
		f32 s = d * d * (3 - 2 * d); /* SimpleSpline */
		return intro.startFov + (intro.nextFov - intro.startFov) * s;
	}
	if (span != 0) {
		f32 d = (now - intro.fovBlendStartTime) / span;
		d = d < 0 ? 0 : d > 1 ? 1 : d;
		return intro.startFov + (intro.nextFov - intro.startFov) * d;
	}
	return (f32)intro.nextFov;
}

static void script_intro_input(Entity *e, const char *in, const char *param)
{
	if (!strcasecmp(in, "activate")) {
		intro.ent = (s32)(e - ents);
	} else if (!strcasecmp(in, "deactivate")) {
		if (intro.ent == (s32)(e - ents))
			intro.ent = -1;
	} else if (!strcasecmp(in, "setcameraviewentity")) {
		Entity *c = find_named(param);
		if (c)
			intro.camera = (s32)(c - ents);
	} else if (!strcasecmp(in, "setblendmode")) {
		intro.blendMode = intro.nextBlendMode = atoi(param);
		intro.blendStartTime = intro.nextBlendTime = now;
		intro.queuedBlendMode = -1;
	} else if (!strcasecmp(in, "setnextblendmode")) {
		intro.queuedNextBlendMode = atoi(param);
	} else if (!strcasecmp(in, "setnextblendtime")) {
		intro.nextBlendTime = now + (f32)atof(param);
		intro.blendStartTime = now;
		if (intro.queuedBlendMode >= 0)
			intro.blendMode = intro.queuedBlendMode;
		if (intro.queuedNextBlendMode < 0)
			intro.queuedNextBlendMode = intro.blendMode;
		intro.nextBlendMode = intro.queuedNextBlendMode;
		intro.queuedNextBlendMode = -1;
		intro.queuedBlendMode = intro.nextBlendMode;
	} else if (!strcasecmp(in, "setnextfov")) {
		intro.nextFov = atoi(param);
	} else if (!strcasecmp(in, "setfovblendtime")) {
		if (intro.nextFovBlendTime >= now)
			intro.startFov = (s32)intro_calc_fov();
		else
			intro.startFov = intro.fov ? intro.fov : 75;
		intro.nextFovBlendTime = now + (f32)atof(param);
		intro.fovBlendStartTime = now;
	} else if (!strcasecmp(in, "setfov")) {
		intro.fov = intro.startFov = atoi(param);
		intro.viewFov = (f32)intro.fov;
	} else if (!strcasecmp(in, "fadeto")) {
		f32 a = 0, d = 0;
		if (sscanf(param, "%f %f", &a, &d) == 2) {
			intro.fadeAlpha = a;
			intro.fadeDuration = d;
		}
	} else if (!strcasecmp(in, "setfadecolor")) {
		f32 r = 0, g = 0, b = 0;
		if (sscanf(param, "%f %f %f", &r, &g, &b) == 3) {
			intro.fadeColor[0] = r;
			intro.fadeColor[1] = g;
			intro.fadeColor[2] = b;
		}
	}
}

/* once per tick: the server's blend completion and the client's fade/FOV interpolation */
static void intro_tick(void)
{
	if (intro.nextBlendTime > intro.blendStartTime && now >= intro.nextBlendTime) {
		intro.blendMode = intro.nextBlendMode; /* BlendComplete */
		intro.blendStartTime = intro.nextBlendTime = now;
		intro.queuedBlendMode = -1;
	}
	if (intro.fadeAlpha != intro.prevFadeAlpha) {
		intro.fadeStartTime = now;
		intro.fadeStartAlpha = intro.curFadeAlpha;
		intro.prevFadeAlpha = intro.fadeAlpha;
		if (intro.fadeDuration <= 0)
			intro.fadeDuration = 0.01f;
	}
	f32 k = intro.fadeDuration > 0 ? (now - intro.fadeStartTime) / intro.fadeDuration : 1;
	k = k < 0 ? 0 : k > 1 ? 1 : k;
	intro.curFadeAlpha = intro.fadeStartAlpha + (intro.fadeAlpha - intro.fadeStartAlpha) * k;
	if (intro.nextFovBlendTime >= now)
		intro.viewFov = intro_calc_fov();
}

int Ent_IntroView(IntroView *v)
{
	memset(v, 0, sizeof(*v));
	if (intro.ent < 0)
		return 0;
	v->fade[0] = intro.fadeColor[0];
	v->fade[1] = intro.fadeColor[1];
	v->fade[2] = intro.fadeColor[2];
	v->fade[3] = intro.curFadeAlpha;
	v->fov = intro.viewFov;
	if (intro.camera > 0 && !(ents[intro.camera].flags & EF_KILLED)) {
		Entity *c = &ents[intro.camera];
		v->hasCamera = 1;
		memcpy(v->camOrigin, c->origin, 12);
		memcpy(v->camAngles, c->angles, 12);
		if (c->bmodel > 0 && (u32)c->bmodel < P->numBModels) {
			/* a brush entity's "origin" is its rotation centre: Source uses GetAbsOrigin too */
		}
	}
	f32 perc = 1;
	if (intro.nextBlendTime - intro.blendStartTime != 0) {
		perc = (now - intro.blendStartTime) / (intro.nextBlendTime - intro.blendStartTime);
		perc = perc < 0 ? 0 : perc > 1 ? 1 : perc;
	}
	v->mode[0] = intro.blendMode;
	v->alpha[0] = 1;
	v->passes = 1;
	if (intro.nextBlendTime > now && perc < 1) {
		v->mode[1] = intro.nextBlendMode;
		v->alpha[0] = 1 - perc;
		v->alpha[1] = perc;
		v->passes = 2;
	}
	return 1;
}


/* ---- act-busy goals: NPCs sent to info_node_hint spots, and queues (ai_goal_actbusy_queue) where the
 * front NPC leaves for the exit node and the rest move up, unless the player stands in the way */
#define QUEUE_NODES 20
#define MAX_QUEUES 4
typedef struct {
	s32 goal;                  /* the ai_goal_actbusy_queue, -1 free */
	s32 node[QUEUE_NODES];     /* hint entities, front first */
	s32 occ[QUEUE_NODES];      /* NPC entity at (or walking to) each node, -1 none */
	u8 blocked[QUEUE_NODES];   /* the player stands at that spot */
	u32 numNodes;
	s32 exitEnt;
	s32 leaving[4];            /* NPCs on their way to the exit */
} Queue;
static Queue queues[MAX_QUEUES];

static void send_npc(s32 npcEnt, s32 to)
{
	Npc *n = npcEnt > 0 ? Npc_OfEntity(npcEnt) : NULL;
	if (!n || to <= 0)
		return;
	if (n->locked && n->script < 0)
		Npc_ResetSequence(n);
	n->aside = 0;
	Npc_MoveTo(n, ents[to].origin, 0, 4.0f);
}

static void busy_assign(s32 npc, s32 hint, s32 goal);
static void busy_release(s32 npc);
static Queue *queue_of(const Entity *e)
{
	s32 id = (s32)(e - ents);
	for (int q = 0; q < MAX_QUEUES; q++)
		if (queues[q].goal == id)
			return &queues[q];
	for (int q = 0; q < MAX_QUEUES; q++)
		if (queues[q].goal < 0) {
			Queue *Q = &queues[q];
			memset(Q, 0, sizeof(*Q));
			Q->goal = id;
			for (int k = 0; k < QUEUE_NODES; k++) {
				char key[16];
				snprintf(key, sizeof(key), "node%02d", k + 1);
				Entity *h = find_named(kv(e, key));
				Q->node[k] = h ? (s32)(h - ents) : -1;
				Q->occ[k] = -1;
				if (h)
					Q->numNodes = (u32)k + 1;
			}
			Entity *x = find_named(kv(e, "node_exit"));
			Q->exitEnt = x ? (s32)(x - ents) : -1;
			for (int k = 0; k < 4; k++)
				Q->leaving[k] = -1;
			return Q;
		}
	return NULL;
}

static int queue_slot_of(const Queue *Q, s32 npc)
{
	for (u32 k = 0; k < Q->numNodes; k++)
		if (Q->occ[k] == npc)
			return (int)k;
	return -1;
}

static void queue_join(Queue *Q, s32 npc, int at)
{
	if (npc <= 0 || !Npc_OfEntity(npc) || queue_slot_of(Q, npc) >= 0)
		return;
	if (at < 0)
		for (u32 k = 0; k < Q->numNodes && at < 0; k++)
			if (Q->node[k] > 0 && Q->occ[k] < 0)
				at = (int)k;
	if (at < 0 || (u32)at >= Q->numNodes || Q->node[at] <= 0)
		return;
	Q->occ[at] = npc;
	send_npc(npc, Q->node[at]);
	busy_assign(npc, Q->node[at], Q->goal);
}

static void queue_leave(Entity *g, Queue *Q, s32 npc)
{
	int k = queue_slot_of(Q, npc);
	if (k < 0)
		return;
	Q->occ[k] = -1;
	busy_release(npc);
	for (int j = 0; j < 4; j++)
		if (Q->leaving[j] < 0) {
			Q->leaving[j] = npc;
			break;
		}
	fire_output(g, "onnpcstartedleavingqueue", npc);
	if (Q->exitEnt > 0)
		send_npc(npc, Q->exitEnt);
}

static void queue_tick(Entity *g)
{
	Queue *Q = queue_of(g);
	if (!Q)
		return;
	/* the line closes up from the front, one step at a time, never through a spot the player blocks */
	for (u32 k = 0; k + 1 < Q->numNodes; k++) {
		if (Q->occ[k] >= 0 || Q->blocked[k] || Q->node[k] <= 0)
			continue;
		u32 j = k + 1;
		while (j < Q->numNodes && Q->occ[j] < 0)
			j++;
		if (j >= Q->numNodes)
			break;
		int clear = 1;
		for (u32 b = k; b < j; b++)
			if (Q->blocked[b])
				clear = 0;
		if (!clear)
			continue;
		const Npc *n = Npc_OfEntity(Q->occ[j]);
		if (n && n->moving)
			continue; /* let it get to its spot first */
		Q->occ[k] = Q->occ[j];
		Q->occ[j] = -1;
		busy_release(Q->occ[k]);
		send_npc(Q->occ[k], Q->node[k]);
		busy_assign(Q->occ[k], Q->node[k], Q->goal);
		fire_output(g, "onqueuemoved", Q->occ[k]);
	}
	for (int j = 0; j < 4; j++) {
		s32 npc = Q->leaving[j];
		if (npc < 0)
			continue;
		const Npc *n = Npc_OfEntity(npc);
		if (!n || !n->moving || (ents[npc].flags & EF_KILLED)) {
			Q->leaving[j] = -1;
			fire_output(g, "onnpcleftqueue", npc);
		}
	}
}


/* NPCs on their way to (or settled at) an act-busy hint: once there they take the hint's spot and facing,
 * play the act-busy's entry sequence (sitting down: its motion carries them back onto the seat), then its
 * busy sequence looping, and its exit sequence when let go, like Source's ai_goal_actbusy behaviour
 * (scripts/actbusy.txt, resolved by the converter into the hint's _busy/_busyin/_busyout) */
#define MAX_BUSY 64
enum { BUSY_GOING, BUSY_ENTERING, BUSY_IN, BUSY_LEAVING };
static struct {
	s32 npc, hint, goal;
	u8 settled;
	f32 until;
} busy[MAX_BUSY];

/* an NPC an act-busy goal has (walking to its spot, at it, or leaving it): Source's act-busy behaviour
 * schedules don't give way to the player */
int Ent_NpcActBusy(s32 npc)
{
	for (int k = 0; k < MAX_BUSY; k++)
		if (busy[k].npc == npc && npc > 0)
			return 1;
	return 0;
}

static void busy_assign(s32 npc, s32 hint, s32 goal)
{
	int slot = -1;
	for (int k = 0; k < MAX_BUSY; k++)
		if (busy[k].npc == npc || (slot < 0 && busy[k].npc <= 0))
			slot = k;
	if (slot < 0)
		return;
	busy[slot].npc = npc;
	busy[slot].hint = hint;
	busy[slot].goal = goal;
	busy[slot].settled = BUSY_GOING;
}

static const char *busy_seq(const Entity *h)
{
	const char *b = kv(h, "_busy");
	return (b && *b) ? b : kv(h, "hintactivity");
}

static void busy_release(s32 npc)
{
	for (int k = 0; k < MAX_BUSY; k++)
		if (busy[k].npc == npc) {
			Npc *n = Npc_OfEntity(npc);
			if (n && busy[k].settled != BUSY_GOING && busy[k].settled != BUSY_LEAVING && n->script < 0) {
				const char *ex = kv(&ents[busy[k].hint], "_busyout");
				f32 d = (ex && *ex) ? Npc_PlaySequence(n, ex, 0) : -1;
				if (d > 0) {
					/* stand up first; the NPC is free once that's done */
					busy[k].settled = BUSY_LEAVING;
					busy[k].until = now + d;
					busy[k].goal = -1;
					continue;
				}
				Npc_ResetSequence(n);
			}
			busy[k].npc = 0;
		}
}

static void busy_tick(void)
{
	for (int k = 0; k < MAX_BUSY; k++) {
		if (busy[k].npc <= 0)
			continue;
		Npc *n = Npc_OfEntity(busy[k].npc);
		if (!n || (ents[busy[k].npc].flags & EF_KILLED)) {
			busy[k].npc = 0;
			continue;
		}
		Entity *h = &ents[busy[k].hint];
		switch (busy[k].settled) {
		case BUSY_GOING: {
			if (n->moving || n->script >= 0)
				continue;
			f32 dx = h->origin[0] - ents[busy[k].npc].origin[0], dy = h->origin[1] - ents[busy[k].npc].origin[1];
			if (dx * dx + dy * dy > 48.0f * 48.0f)
				continue; /* (still on the way, or blocked: the move rules get it there) */
			Npc_Teleport(n, h->origin, h->angles[1]);
			const char *in = kv(h, "_busyin");
			f32 d = (in && *in) ? Npc_PlaySequence(n, in, 0) : -1;
			if (d > 0) {
				busy[k].settled = BUSY_ENTERING;
				busy[k].until = now + d;
				continue;
			}
			/* a named sequence, or an activity (ACT_BUSY_SIT_CHAIR...): the converter stores those under
			 * the activity's name, as the model's sequence for it */
			const char *act = busy_seq(h);
			if (act && *act)
				Npc_PlaySequence(n, act, 1);
			busy[k].settled = BUSY_IN;
			break;
		}
		case BUSY_ENTERING:
			if (n->script >= 0) {
				busy[k].settled = BUSY_IN;
				continue;
			}
			if (now >= busy[k].until) {
				const char *act = busy_seq(h);
				if (act && *act)
					Npc_PlaySequence(n, act, 1);
				busy[k].settled = BUSY_IN;
			}
			break;
		case BUSY_LEAVING:
			if (now >= busy[k].until) {
				if (n->script < 0)
					Npc_ResetSequence(n);
				busy[k].npc = 0;
			}
			break;
		default:
			break;
		}
	}
}

/* an NPC under an ai_goal_actbusy takes the nearest free act-busy hint in the goal's range (and in its
 * hint group, if it has one), like Source's */
static void actbusy_pick(Entity *g, s32 i)
{
	f32 range = kvf(g, "busysearchrange", 2048);
	if (i <= 0 || (u32)i >= numEnts || !Npc_OfEntity(i))
		return;
	const char *grp = kv(&ents[i], "hintgroup");
	s32 best = -1;
	f32 bd = range * range;
	for (u32 h = 1; h < numEnts; h++) {
		if (strcasecmp(ents[h].classname, "info_node_hint"))
			continue;
		const char *act = kv(&ents[h], "hintactivity");
		if (!act || !*act || !strcasecmp(act, "ACT_BUSY_QUEUE")) /* (queue spots belong to the queue goal) */
			continue;
		const char *hg = kv(&ents[h], "group");
		if (grp && *grp && (!hg || strcasecmp(hg, grp)))
			continue;
		int taken = 0;
		for (int k = 0; k < MAX_BUSY; k++)
			if (busy[k].npc > 0 && busy[k].hint == (s32)h && busy[k].npc != i)
				taken = 1;
		if (taken)
			continue;
		f32 d = 0;
		for (int c = 0; c < 3; c++)
			d += (ents[h].origin[c] - ents[i].origin[c]) * (ents[h].origin[c] - ents[i].origin[c]);
		if (d < bd) {
			bd = d;
			best = (s32)h;
		}
	}
	if (best > 0) {
		send_npc(i, best);
		busy_assign(i, best, (s32)(g - ents));
		fire_output(g, "onnpcstartedbusy", i);
	}
}

/* ai_goal_actbusy Activate: each of its actors picks a hint */
static void actbusy_activate(Entity *g)
{
	const char *pat = kv(g, "actor");
	if (!pat || !*pat)
		return;
	for (u32 i = 1; i < numEnts; i++)
		if (!(ents[i].flags & (EF_KILLED | EF_DISABLED)) && Npc_OfEntity((s32)i) && name_matches(&ents[i], pat))
			actbusy_pick(g, (s32)i);
}

static void actbusy_input(Entity *e, const char *in, const char *param, s32 activator)
{
	int isQueue = e->cls == EC_ACTBUSY_QUEUE;
	Queue *Q = isQueue ? queue_of(e) : NULL;
	if (!strcasecmp(in, "forcenpctoactbusy")) {
		/* "<npc> <hint> [teleport]" */
		char who[64] = "", hint[64] = "";
		int tele = 0;
		sscanf(param, "%63s %63s %d", who, hint, &tele);
		Entity *ne = find_named(who), *he = find_named(hint);
		if (!ne || !he)
			return;
		s32 npc = (s32)(ne - ents), h = (s32)(he - ents);
		if (Q) {
			for (u32 k = 0; k < Q->numNodes; k++)
				if (Q->node[k] == h) {
					if (Q->occ[k] >= 0 && Q->occ[k] != npc)
						break;
					queue_join(Q, npc, (int)k);
					return;
				}
		}
		Npc *n = Npc_OfEntity(npc);
		if (n && tele)
			Npc_Teleport(n, he->origin, he->angles[1]);
		else
			send_npc(npc, h);
		busy_assign(npc, h, (s32)(e - ents));
		fire_output(e, "onnpcstartedbusy", npc);
	} else if (!strcasecmp(in, "forcethisnpctoactbusy")) {
		/* (the activator: e.g. the NPC an npc_template_maker just spawned) */
		if (Q)
			queue_join(Q, activator, -1);
		else
			actbusy_pick(e, activator);
	} else if (!strcasecmp(in, "deactivate")) {
		for (int k = 0; k < MAX_BUSY; k++)
			if (busy[k].npc > 0 && busy[k].goal == (s32)(e - ents))
				busy_release(busy[k].npc);
	} else if (!strcasecmp(in, "activate")) {
		if (!Q)
			actbusy_activate(e);
		if (Q) {
			const char *pat = kv(e, "actor");
			for (u32 i = 1; i < numEnts; i++)
				if (!(ents[i].flags & (EF_KILLED | EF_DISABLED)) && Npc_OfEntity((s32)i) && pat && *pat &&
				    name_matches(&ents[i], pat))
					queue_join(Q, (s32)i, -1);
		}
	} else if (Q && !strcasecmp(in, "movequeueup")) {
		if (Q->occ[0] >= 0)
			queue_leave(e, Q, Q->occ[0]);
	} else if (Q && !strcasecmp(in, "forcethisnpctoleave")) {
		queue_leave(e, Q, activator);
	} else if (Q && (!strcasecmp(in, "playerstartedblocking") || !strcasecmp(in, "playerstoppedblocking"))) {
		int k = atoi(param) - 1;
		if (k >= 0 && k < QUEUE_NODES)
			Q->blocked[k] = !strcasecmp(in, "playerstartedblocking");
	}
}


/* ---- global states (env_global): survive level changes like Source's global entity table */
#define MAX_GLOBALS 48
static struct {
	char name[40];
	s8 state;
} globals[MAX_GLOBALS];
static int numGlobals;
static int autoNewGame; /* (this map started as a new game, not a level change) */
static int reloadRequest; /* (a player_loadsaved went off) */
int Ent_ReloadRequested(void) { return reloadRequest; }

static int global_find(const char *name)
{
	for (int i = 0; i < numGlobals; i++)
		if (!strcasecmp(globals[i].name, name))
			return i;
	return -1;
}

static void global_set(const char *name, int state, int onlyIfNew)
{
	if (!name || !*name)
		return;
	int i = global_find(name);
	if (i >= 0 && onlyIfNew)
		return;
	if (i < 0) {
		if (numGlobals >= MAX_GLOBALS)
			return;
		i = numGlobals++;
		snprintf(globals[i].name, sizeof(globals[i].name), "%s", name);
	}
	globals[i].state = (s8)state;
}

void Ent_ResetGlobals(void) { numGlobals = 0; }

int Ent_GlobalState(const char *name)
{
	int i = global_find(name);
	return i < 0 ? -1 : globals[i].state;
}

/* the title screen's background map: the player is only a camera there (Source's background maps):
 * NPCs don't see him and nothing hurts him */
static int noTarget;
void Ent_SetNoTarget(int on) { noTarget = on; }
int Ent_NoTarget(void) { return noTarget; }

void Ent_HurtPlayer(f32 damage, u32 type, const f32 from[3])
{
	if (noTarget)
		return;
	if (cbs.hurtPlayerFrom)
		cbs.hurtPlayerFrom(damage, type, from);
	else if (cbs.hurtPlayer)
		cbs.hurtPlayer(damage);
}

/* can an NPC's eyes at `from` see the player's eyes (world and solid brush entities in the way)? */
int Ent_PlayerVisible(const f32 from[3], f32 *dist)
{
	f32 to[3] = {player->origin[0], player->origin[1], player->origin[2] + Player_EyeHeight(player)};
	f32 d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
	if (dist)
		*dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (player->noclip || noTarget)
		return 0;
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_SetSkipBoxes(1);
	CM_BoxTrace(&tr, from, to, z, z, CONTENTS_SOLID);
	CM_SetSkipBoxes(0);
	return !tr.startsolid && tr.fraction >= 0.999f;
}

/* logic_timer (CTimerEntity): a random interval is drawn afresh each time (UseRandomTime); the up/down
 * kind (spawnflag 1) alternates OnTimerLow / OnTimerHigh; it is its own activator */
static f32 timer_interval(Entity *e)
{
	if (kvf(e, "userandomtime", 0) != 0) {
		f32 lo = kvf(e, "lowerrandombound", 1), hi = kvf(e, "upperrandombound", 1);
		e->interval = lo + (hi - lo) * (f32)rand() / (f32)RAND_MAX;
	}
	return e->interval;
}

static void timer_reset(Entity *e)
{
	if (e->enabled)
		e->nextThink = now + timer_interval(e);
}

static void timer_fire(Entity *e)
{
	if (!e->enabled)
		return;
	s32 self = (s32)(e - ents);
	if (e->spawnflags & 1) {
		fire_output(e, e->phase ? "ontimerhigh" : "ontimerlow", self);
		e->phase = !e->phase;
	} else {
		fire_output(e, "ontimer", self);
	}
	timer_reset(e);
}

/* logic_relay (CLogicRelay): unless flagged for fast retrigger (2) it ignores Trigger until its last output
 * has gone out (it posts itself EnableRefire after its longest delay); CancelPending drops what it queued */
static f32 outputs_max_delay(const Entity *e, const char *output)
{
	f32 m = 0;
	for (u32 i = 0; i < e->numKV; i++) {
		const PackKV *k = &P->kvs[e->firstKV + i];
		if (strcmp(str(k->key), output))
			continue;
		const char *v = str(k->val);
		char sep = strchr(v, 0x1B) ? 0x1B : ',';
		int f = 0;
		for (const char *p = v; *p; p++)
			if (*p == sep && ++f == 3) {
				f32 d = (f32)atof(p + 1);
				if (d > m)
					m = d;
				break;
			}
	}
	return m;
}

static void cancel_events_from(s32 caller)
{
	u32 w = 0;
	for (u32 i = 0; i < numEvents; i++)
		if (events[i].caller != caller)
			events[w++] = events[i];
	numEvents = w;
}

static void input(Entity *e, const char *in, const char *param, s32 activator, s32 caller)
{
	(void)caller;
	if (e->flags & EF_KILLED)
		return;

	if (!strcmp(e->classname, "logic_collision_pair")) {
		if (!strcasecmp(in, "disablecollisions"))
			e->value = 1;
		else if (!strcasecmp(in, "enablecollisions"))
			e->value = 0;
	}
	/* inputs every entity understands */
	if (!strcasecmp(in, "kill") || !strcasecmp(in, "killhierarchy")) {
		e->flags |= EF_KILLED;
		return;
	}
	if (!strcasecmp(in, "setparent")) {
		char nm[64];
		snprintf(nm, sizeof(nm), "%s", param);
		char *comma = strchr(nm, ',');
		if (comma)
			*comma = 0;
		e->parentAttach[0] = 0;
		set_parent(e, find_named(nm));
		return;
	}
	if (!strcasecmp(in, "clearparent")) {
		e->parentAttach[0] = 0;
		e->parent = -1;
		return;
	}
	if (!strcasecmp(in, "setparentattachment") || !strcasecmp(in, "setparentattachmentmaintainoffset")) {
		/* follow one of the parent character's attachments (Barney's helmet on his head, then in his hand).
		 * Without "maintain offset" Source snaps it onto the attachment; with it, it keeps where it is */
		snprintf(e->parentAttach, sizeof(e->parentAttach), "%s", param);
		Npc *pn = e->parent > 0 ? Npc_OfEntity(e->parent) : NULL;
		f32 ap[3], aR[3][3];
		if (pn && Npc_Attachment(pn, e->parentAttach, ap, aR)) {
			if (strcasecmp(in, "setparentattachmentmaintainoffset")) {
				memcpy(e->origin, ap, 12);
				memcpy(e->R, aR, sizeof(aR));
			}
			f32 d[3] = {e->origin[0] - ap[0], e->origin[1] - ap[1], e->origin[2] - ap[2]};
			for (int k = 0; k < 3; k++)
				e->localOrigin[k] = aR[0][k] * d[0] + aR[1][k] * d[1] + aR[2][k] * d[2];
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					e->localR[r][c] = aR[0][r] * e->R[0][c] + aR[1][r] * e->R[1][c] + aR[2][r] * e->R[2][c];
		} else {
			e->parentAttach[0] = 0;
		}
		return;
	}
	if (!strncasecmp(in, "fireuser", 8)) {
		char out[16];
		snprintf(out, sizeof(out), "onuser%c", in[8]);
		fire_output(e, out, activator);
		return;
	}
	/* ropes, effect entities, constraints, func_physbox (envents.c) */
	if (EnvEnts_Input((s32)(e - ents), in, param, activator))
		return;
	if (Rel_Input((s32)(e - ents), in))
		return;
	if (AiEnts_Input((s32)(e - ents), in, param, activator))
		return;
	if (Sentries_Input((s32)(e - ents), in))
		return;
	if (Flyers_Input((s32)(e - ents), in, param))
		return;
	if (Mines_Input((s32)(e - ents), in))
		return;
	if (Tanks_Input((s32)(e - ents), in, param))
		return;
	if (Objects_Input((s32)(e - ents), in, param))
		return;
	if (!strcasecmp(in, "setrelationship") && e->npcModel >= 0) {
		Rel_SetRelationship((s32)(e - ents), param);
		return;
	}

	switch (e->cls) {
	case EC_LOGIC_RELAY:
		/* (phase: waiting for refire) */
		if (!strcasecmp(in, "trigger")) {
			if (!e->enabled || e->phase)
				return;
			fire_output(e, "ontrigger", activator);
			if (e->spawnflags & 1)
				e->flags |= EF_KILLED; /* "only trigger once" */
			else if (!(e->spawnflags & 2)) {
				e->phase = 1;
				queue("!self", "enablerefire", "", outputs_max_delay(e, "ontrigger") + 0.001f,
				      (s32)(e - ents), (s32)(e - ents));
			}
		} else if (!strcasecmp(in, "enable"))
			e->enabled = 1;
		else if (!strcasecmp(in, "disable"))
			e->enabled = 0;
		else if (!strcasecmp(in, "toggle"))
			e->enabled = !e->enabled;
		else if (!strcasecmp(in, "enablerefire"))
			e->phase = 0;
		else if (!strcasecmp(in, "cancelpending")) {
			cancel_events_from((s32)(e - ents));
			e->phase = 0;
		}
		break;

	case EC_MOMENTARY:
		if (!strcasecmp(in, "setposition")) {
			f32 p = (f32)atof(param);
			e->value = p < 0 ? 0 : p > 1 ? 1 : p;
			if (e->frac < e->value)
				e->minv = 1, e->phase = MR_SETPOS;
			else if (e->frac > e->value)
				e->minv = -1, e->phase = MR_SETPOS;
		} else if (!strcasecmp(in, "setpositionimmediately")) {
			f32 p = (f32)atof(param);
			e->value = e->frac = p < 0 ? 0 : p > 1 ? 1 : p;
			momentary_apply(e);
		} else if (!strcasecmp(in, "lock")) {
			e->locked = 1;
			e->phase = MR_IDLE;
		} else if (!strcasecmp(in, "unlock")) {
			e->locked = 0;
			if (e->maxv > 0 && e->frac > 0)
				e->phase = MR_RETURN; /* (Unlock: ReturnMoveDone) */
		} else if (!strcasecmp(in, "enable"))
			e->enabled = 1;
		else if (!strcasecmp(in, "disable"))
			e->enabled = 0;
		break;

	case EC_LOGIC_TIMER:
		if (!strcasecmp(in, "enable")) {
			e->enabled = 1;
			timer_reset(e);
		} else if (!strcasecmp(in, "disable"))
			e->enabled = 0;
		else if (!strcasecmp(in, "toggle")) {
			e->enabled = !e->enabled;
			timer_reset(e);
		} else if (!strcasecmp(in, "refiretime")) {
			f32 t = (f32)atof(param);
			if (t < 0.01f)
				t = 0.01f;
			if (t != e->interval) {
				e->interval = t;
				timer_reset(e);
			}
		} else if (!strcasecmp(in, "firetimer"))
			timer_fire(e);
		else if (!strcasecmp(in, "resettimer"))
			timer_reset(e);
		else if (!strcasecmp(in, "addtotimer") && e->enabled)
			e->nextThink += (f32)atof(param);
		else if (!strcasecmp(in, "subtractfromtimer") && e->enabled)
			e->nextThink -= (f32)atof(param);
		break;

	case EC_MATH_COUNTER: {
		f32 p = (f32)atof(param);
		if (!strcasecmp(in, "enable")) {
			e->enabled = 1;
			return;
		}
		if (!strcasecmp(in, "disable")) {
			e->enabled = 0;
			return;
		}
		if (!strcasecmp(in, "getvalue")) {
			char v[24];
			snprintf(v, sizeof(v), "%g", e->value);
			fire_output_value(e, "ongetvalue", activator, v);
			return;
		}
		if (!e->enabled)
			return; /* (a disabled counter ignores the rest) */
		if (!strcasecmp(in, "add"))
			counter_update(e, e->value + p, activator);
		else if (!strcasecmp(in, "subtract"))
			counter_update(e, e->value - p, activator);
		else if (!strcasecmp(in, "multiply"))
			counter_update(e, e->value * p, activator);
		else if (!strcasecmp(in, "divide") && p != 0)
			counter_update(e, e->value / p, activator);
		else if (!strcasecmp(in, "setvalue"))
			counter_update(e, p, activator);
		else if (!strcasecmp(in, "setvaluenofire")) {
			if (e->minv != 0 || e->maxv != 0)
				p = p < e->minv ? e->minv : p > e->maxv ? e->maxv : p;
			e->value = p;
		} else if (!strcasecmp(in, "sethitmax")) {
			e->maxv = p;
			if (e->maxv < e->minv)
				e->minv = e->maxv;
			counter_update(e, e->value, activator);
		} else if (!strcasecmp(in, "sethitmin")) {
			e->minv = p;
			if (e->maxv < e->minv)
				e->maxv = e->minv;
			counter_update(e, e->value, activator);
		}
		break;
	}

	case EC_LOGIC_BRANCH:
		if (!strcasecmp(in, "setvalue"))
			e->value = (f32)atof(param);
		else if (!strcasecmp(in, "setvaluetest")) {
			e->value = (f32)atof(param);
			fire_output(e, e->value != 0 ? "ontrue" : "onfalse", activator);
		} else if (!strcasecmp(in, "toggle"))
			e->value = e->value != 0 ? 0 : 1;
		else if (!strcasecmp(in, "toggletest")) {
			e->value = e->value != 0 ? 0 : 1;
			fire_output(e, e->value != 0 ? "ontrue" : "onfalse", activator);
		} else if (!strcasecmp(in, "test"))
			fire_output(e, e->value != 0 ? "ontrue" : "onfalse", activator);
		break;

	case EC_LOGIC_COMPARE:
		if (!strcasecmp(in, "setvalue"))
			e->value = (f32)atof(param);
		else if (!strcasecmp(in, "setvaluecompare")) {
			e->value = (f32)atof(param);
			compare_fire(e, activator);
		} else if (!strcasecmp(in, "setcomparevalue"))
			e->compare = (f32)atof(param);
		else if (!strcasecmp(in, "compare"))
			compare_fire(e, activator);
		break;

	case EC_TRIGGER_ONCE:
	case EC_TRIGGER_MULTIPLE:
	case EC_TRIGGER_TELEPORT:
	case EC_TRIGGER_PUSH:
	case EC_TRIGGER_LOOK:
	case EC_TRIGGER_PLAYERMOVE:
		if (!strcasecmp(in, "enable"))
			e->enabled = 1;
		else if (!strcasecmp(in, "disable")) {
			e->enabled = 0;
			e->touching = 0;
		} else if (!strcasecmp(in, "toggle"))
			e->enabled = !e->enabled;
		break;

	case EC_TRIGGER_CHANGELEVEL:
		if (!strcasecmp(in, "changelevel") && cbs.changeLevel) {
			const char *map = kv(e, "map"), *mark = kv(e, "landmark");
			if (map)
				cbs.changeLevel(map, mark ? mark : "");
		}
		break;

	case EC_POINT_TELEPORT:
		if (!strcasecmp(in, "teleport")) {
			const char *t = kv(e, "target");
			if (t && !strcasecmp(t, "!player"))
				teleport_player_to(e);
			else {
				Entity *x = find_named(t);
				if (x) {
					memcpy(x->origin, e->origin, sizeof(x->origin));
					memcpy(x->angles, e->angles, sizeof(x->angles));
					update_matrix(x);
				}
			}
		}
		break;

	case EC_FUNC_DOOR:
	case EC_FUNC_DOOR_ROTATING:
	case EC_PROP_DOOR:
	case EC_FUNC_MOVELINEAR:
		if (!strcasecmp(in, "open") || !strcasecmp(in, "openawayfrom")) {
			if (e->cls == EC_PROP_DOOR && !e->locked) {
				/* InputOpen opens forward; InputOpenAwayFrom away from the named entity */
				const f32 *from = NULL;
				f32 fp[3];
				if (!strcasecmp(in, "openawayfrom") && param && *param) {
					if (!strcasecmp(param, "!player")) {
						from = player->origin;
					} else {
						Entity *x = find_named(param);
						if (x) {
							memcpy(fp, x->origin, 12);
							from = fp;
						}
					}
				}
				prop_door_direction_from(e, from, 0);
			}
			mover_open(e, activator);
			e->waitUntil = 1e30f; /* opened by script: stays open until told */
		} else if (!strcasecmp(in, "close")) {
			if (e->cls == EC_PROP_DOOR && e->moveState == MV_CLOSED && e->frac != 0) {
				/* (ajar isn't closed: Close shuts it) */
				if (e->frac < 0) {
					e->ang2[1] = e->ang1[1] - (e->ang2[1] - e->ang1[1]);
					e->frac = -e->frac;
				}
				e->moveState = MV_OPEN;
			}
			mover_close(e, activator);
		} else if (!strcasecmp(in, "toggle")) {
			/* (CBaseDoor::InputToggle: not when locked; at the bottom it opens, at the top it closes,
			 * moving it carries on) */
			if (e->locked && e->cls != EC_FUNC_MOVELINEAR)
				return;
			if (e->moveState == MV_CLOSED) {
				if (e->cls == EC_PROP_DOOR)
					prop_door_direction_from(e, NULL, 0);
				mover_open(e, activator);
			} else if (e->moveState == MV_OPEN)
				mover_close(e, activator);
		} else if (!strcasecmp(in, "settogglestate")) {
			/* (SetToggleState: straight to open (1) or closed (0)) */
			e->frac = atoi(param) ? 1.0f : 0.0f;
			e->moveState = atoi(param) ? MV_OPEN : MV_CLOSED;
			e->waitUntil = now + e->wait;
			mover_apply(e);
		} else if (!strcasecmp(in, "lock"))
			e->locked = 1;
		else if (!strcasecmp(in, "unlock"))
			e->locked = 0;
		else if (!strcasecmp(in, "setspeed"))
			e->speed = (f32)atof(param);
		else if (!strcasecmp(in, "setposition")) {
			f32 t = (f32)atof(param);
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			if (fabsf(t - e->frac) * e->moveLen > 0.001f) {
				e->value = t;
				e->latch |= 4;
				e->moveState = t > e->frac ? MV_OPENING : MV_CLOSING;
			}
		}
		break;

	case EC_FUNC_BUTTON:
		if (!strcasecmp(in, "press"))
			press_button(e, activator, BTN_PRESS);
		else if (!strcasecmp(in, "pressin"))
			press_button(e, activator, BTN_PRESSIN);
		else if (!strcasecmp(in, "pressout"))
			press_button(e, activator, BTN_PRESSOUT);
		else if (!strcasecmp(in, "lock"))
			e->locked = 1;
		else if (!strcasecmp(in, "unlock"))
			e->locked = 0;
		break;

	case EC_TRACKTRAIN:
		/* (CFuncTrackTrain's inputs) */
		if (!strcasecmp(in, "startforward")) {
			train_set_dir(e, 1);
			train_set_speed(e, e->maxSpeed, 0);
		} else if (!strcasecmp(in, "startbackward")) {
			train_set_dir(e, 0);
			train_set_speed(e, e->maxSpeed, 0);
		} else if (!strcasecmp(in, "resume")) {
			/* (m_flSpeed = m_oldSpeed; Start) */
			e->latch &= ~8;
			e->trainSpeed = e->maxv;
			if (e->trainSpeed != 0) {
				if (e->pathNode <= 0 && e->lastNode > 0)
					e->pathNode = node_next(e->lastNode, e->minv < 0);
				fire_output(e, "onstart", (s32)(e - ents));
			}
		} else if (!strcasecmp(in, "stop")) {
			e->latch &= ~8;
			e->maxv = e->trainSpeed;
			e->trainSpeed = 0;
		} else if (!strcasecmp(in, "reverse")) {
			f32 sp = fabsf(e->trainSpeed);
			train_set_dir(e, e->minv < 0);
			train_set_speed(e, sp, 0);
		} else if (!strcasecmp(in, "toggle"))
			train_set_speed(e, e->trainSpeed == 0 ? e->maxSpeed : 0, 0);
		else if (!strcasecmp(in, "setspeed")) {
			f32 v = (f32)atof(param);
			train_set_speed(e, e->maxSpeed * (v < 0 ? 0 : v > 1 ? 1 : v), 0);
		} else if (!strcasecmp(in, "setspeeddir") || !strcasecmp(in, "setspeeddiraccel")) {
			f32 v = (f32)atof(param);
			train_set_dir(e, v >= 0);
			v = fabsf(v);
			train_set_speed(e, e->maxSpeed * (v > 1 ? 1 : v), !strcasecmp(in, "setspeeddiraccel"));
		} else if (!strcasecmp(in, "setspeedreal")) {
			f32 v = (f32)atof(param);
			train_set_speed(e, v < 0 ? 0 : v > e->maxSpeed ? e->maxSpeed : v, 0);
		} else if (!strcasecmp(in, "setmaxspeed")) {
			e->maxSpeed = (f32)atof(param);
			if (fabsf(e->trainSpeed) > e->maxSpeed)
				train_set_speed(e, e->maxSpeed, 0);
		} else if (!strcasecmp(in, "teleporttopathtrack") || !strcasecmp(in, "movetopathnode"))
			train_teleport(e, node_named(param), 0);
		break;

	case EC_PATH:
		/* (CPathTrack's inputs) */
		if (!strcasecmp(in, "enablepath"))
			e->spawnflags &= ~1u;
		else if (!strcasecmp(in, "disablepath"))
			e->spawnflags |= 1;
		else if (!strcasecmp(in, "togglepath"))
			e->spawnflags ^= 1;
		else if (kv(e, "altpath") && kv(e, "altpath")[0]) {
			if (!strcasecmp(in, "enablealternatepath"))
				e->spawnflags |= 0x8000;
			else if (!strcasecmp(in, "disablealternatepath"))
				e->spawnflags &= ~0x8000u;
			else if (!strcasecmp(in, "togglealternatepath"))
				e->spawnflags ^= 0x8000;
		}
		break;

	case EC_VIEWCONTROL:
		if (!strcasecmp(in, "enable")) {
			if (activeView > 0 && activeView != (s32)(e - ents))
				view_disable();
			activeView = (s32)(e - ents);
			e->enabled = 1;
			viewUntil = now + kvf(e, "wait", 10);
			viewSnap = (e->spawnflags & 16) ? 1 : 1; /* the first frame always faces the goal */
			if (e->spawnflags & 1) {
				memcpy(e->origin, player->origin, 12);
				e->origin[2] += Player_EyeHeight(player);
			}
			const char *mv = kv(e, "moveto");
			e->pathNode = mv && mv[0] ? node_named(mv) : -1;
			e->trainSpeed = kvf(e, "speed", 0);
			if (e->pathNode > 0 && e->trainSpeed <= 0)
				e->trainSpeed = 100;
		} else if (!strcasecmp(in, "disable")) {
			if (activeView == (s32)(e - ents))
				view_disable();
		}
		break;

	case EC_FADE:
		if (!strcasecmp(in, "fade")) {
			int r = 0, g = 0, b = 0;
			const char *c = kv(e, "rendercolor");
			if (c)
				sscanf(c, "%d %d %d", &r, &g, &b);
			fadeR = (f32)r;
			fadeG = (f32)g;
			fadeB = (f32)b;
			fadeA = kvf(e, "renderamt", 255) / 255.0f;
			fadeDur = kvf(e, "duration", 2);
			fadeHold = kvf(e, "holdtime", 0);
			fadeIn = (e->spawnflags & 1) != 0;
			fadeStay = (e->spawnflags & 8) != 0;
			fadeStart = now;
			fadeOn = 1;
			fire_output(e, "onbeginfade", activator);
		}
		break;

	case EC_LOADSAVED:
		/* CRevertSaved::InputReload: fade everything out, then after "loadtime" reload the last save
		 * (here the level from where it began, as after dying) */
		if (!strcasecmp(in, "reload")) {
			int r = 0, g = 0, b = 0;
			const char *c = kv(e, "rendercolor");
			if (c)
				sscanf(c, "%d %d %d", &r, &g, &b);
			fadeR = (f32)r;
			fadeG = (f32)g;
			fadeB = (f32)b;
			fadeA = kvf(e, "renderamt", 255) / 255.0f;
			fadeDur = kvf(e, "duration", 2);
			fadeHold = kvf(e, "holdtime", 0);
			fadeIn = 0;
			fadeStay = 1;
			fadeStart = now;
			fadeOn = 1;
			e->nextThink = now + kvf(e, "loadtime", 0) + 0.001f;
		}
		break;

	case EC_ZOOM:
		if (!strcasecmp(in, "zoom")) {
			f32 cur = Ent_ZoomFov();
			zoomFrom = cur > 0 ? cur : 75;
			zoomTo = kvf(e, "fov", 75);
			zoomRate = kvf(e, "rate", 0);
			zoomStart = now;
			zoomOn = 1;
		} else if (!strcasecmp(in, "unzoom")) {
			f32 cur = Ent_ZoomFov();
			zoomFrom = cur > 0 ? cur : 75;
			zoomTo = 75;
			zoomRate = 0.5f;
			zoomStart = now;
			zoomOn = 1;
		}
		break;

	case EC_SHAKE:
		if (!strcasecmp(in, "startshake")) {
			shakeAmp = kvf(e, "amplitude", 4);
			shakeStart = now;
			shakeUntil = now + kvf(e, "duration", 1);
			memcpy(shakeOrigin, e->origin, 12);
			shakeRadius = (e->spawnflags & 1) ? 0 : kvf(e, "radius", 500);
		} else if (!strcasecmp(in, "stopshake")) {
			shakeUntil = now;
		} else if (!strcasecmp(in, "amplitude")) {
			shakeAmp = (f32)atof(param);
		}
		break;

	case EC_MICROPHONE:
		if (!strcasecmp(in, "enable"))
			e->enabled = 1;
		else if (!strcasecmp(in, "disable"))
			e->enabled = 0;
		break;

	case EC_GAME_TEXT:
		if (!strcasecmp(in, "display")) {
			int r = 255, g = 255, b = 255;
			const char *c = kv(e, "color");
			if (c)
				sscanf(c, "%d %d %d", &r, &g, &b);
			Hud_Message((int)kvf(e, "channel", 1), kv(e, "_text") ? kv(e, "_text") : kv(e, "message"),
			            kvf(e, "x", -1), kvf(e, "y", -1), (u32)r << 24 | (u32)g << 16 | (u32)b << 8 | 255,
			            kvf(e, "fadein", 0.5f), kvf(e, "holdtime", 2), kvf(e, "fadeout", 0.5f));
		} else if (!strcasecmp(in, "settext")) {
			Hud_Message((int)kvf(e, "channel", 1), param, kvf(e, "x", -1), kvf(e, "y", -1), 0xFFFFFFFF,
			            kvf(e, "fadein", 0.5f), kvf(e, "holdtime", 2), kvf(e, "fadeout", 0.5f));
		}
		break;

	case EC_ENV_CREDITS: {
		/* CCredits: ShowLogo (its "logolength", else credits.txt's logotime), RollCredits (the intro's names) */
		f32 fi = 2.5f, fo = 1, hold = 1, next, pause, lt = 1, px, py;
		const char *cp = kv(e, "_cparams");
		if (cp)
			sscanf(cp, "%f %f %f %f %f %f %f %f", &fi, &fo, &hold, &next, &pause, &lt, &px, &py);
		if (!strcasecmp(in, "showlogo")) {
			f32 len = kvf(e, "logolength", 0);
			Hud_ShowLogo(kv(e, "_logo") ? kv(e, "_logo") : "HALF-LIFE'", fi, len > 0 ? len : lt, fo);
		} else if (!strcasecmp(in, "rollcredits") && kv(e, "_names")) {
			Hud_RollCredits(kv(e, "_names"), cp);
		}
		break;
	}

	case EC_ENV_MESSAGE:
		if (!strcasecmp(in, "showmessage") && kv(e, "_text")) {
			f32 x = -1, y = 0.6f, fi = 0.5f, ho = 3, fo = 1;
			int r = 255, g = 255, b = 255;
			const char *v;
			if ((v = kv(e, "_pos")))
				sscanf(v, "%f %f", &x, &y);
			if ((v = kv(e, "_color")))
				sscanf(v, "%d %d %d", &r, &g, &b);
			if ((v = kv(e, "_time")))
				sscanf(v, "%f %f %f", &fi, &ho, &fo);
			int fx = 0, r2 = r, g2 = g, b2 = b;
			f32 fxt = 0.25f;
			if ((v = kv(e, "_fx")))
				sscanf(v, "%d %f %d %d %d", &fx, &fxt, &r2, &g2, &b2);
			if (!(e->spawnflags & 1) || !(e->flags & EF_FIRED)) {
				Hud_MessageFx(2, kv(e, "_text"), x, y, (u32)r << 24 | (u32)g << 16 | (u32)b << 8 | 255,
				              (u32)r2 << 24 | (u32)g2 << 16 | (u32)b2 << 8 | 255, fx, fi, fxt, ho, fo);
				fire_output(e, "onshowmessage", activator);
			}
			if (e->spawnflags & 1) /* SF_MESSAGE_ONCE */
				e->flags |= EF_FIRED;
		}
		break;

	case EC_HUDHINT:
		if (!strcasecmp(in, "showhudhint"))
			Hud_Hint(kv(e, "_text"), 1);
		else if (!strcasecmp(in, "hidehudhint"))
			Hud_Hint(NULL, 0);
		break;

	case EC_POINT_CAMERA:
		if (!strcasecmp(in, "setonandturnothersoff")) {
			/* Source: this camera on, every other point_camera off */
			for (u32 k = 1; k < numEnts; k++)
				if (ents[k].cls == EC_POINT_CAMERA)
					ents[k].enabled = 0;
			e->enabled = 1;
		} else if (!strcasecmp(in, "seton"))
			e->enabled = 1;
		else if (!strcasecmp(in, "setoff"))
			e->enabled = 0;
		else if (!strcasecmp(in, "changefov"))
			e->value = (f32)atof(param);
		break;

	case EC_FUNC_MONITOR:
		if (!strcasecmp(in, "setcamera")) {
			Entity *c = find_named(param);
			e->actor = c ? (s32)(c - ents) : -1;
			break;
		}
		/* fall through: enable/disable like a func_brush */
	case EC_FUNC_BRUSH:
	case EC_FUNC_WALL:
	case EC_FUNC_ILLUSIONARY:
	case EC_PROP:
		if (e->cls == EC_PROP) {
			if (!strcasecmp(in, "wake")) {
				Phys_Wake((s32)(e - ents));
				break;
			}
			if (!strcasecmp(in, "enablemotion") || !strcasecmp(in, "disablemotion")) {
				Phys_SetMotion((s32)(e - ents), !strcasecmp(in, "enablemotion"));
				break;
			}
			if (!strcasecmp(in, "setanimation") || !strcasecmp(in, "setdefaultanimation")) {
				Npc *pn = Npc_OfEntity((s32)(e - ents));
				if (pn)
					Npc_PropAnimation(pn, param, !strcasecmp(in, "setdefaultanimation"));
				break;
			}
			if (!strcasecmp(in, "break")) {
				Ent_Damage((s32)(e - ents), 1e6f, NULL, NULL);
				if (!(e->flags & EF_KILLED)) { /* unbreakable by damage: the input still breaks it */
					fire_output(e, "onbreak", activator);
					e->flags |= EF_KILLED;
					break_effects(e, NULL);
				}
				break;
			}
		}
		if (!strcasecmp(in, "enable") || !strcasecmp(in, "turnon") || !strcasecmp(in, "enablecollision"))
			set_enabled(e, 1);
		else if (!strcasecmp(in, "disable") || !strcasecmp(in, "turnoff"))
			set_enabled(e, 0);
		else if (!strcasecmp(in, "toggle"))
			set_enabled(e, !e->enabled);
		break;

	case EC_FUNC_BREAKABLE:
		if (!strcasecmp(in, "break")) {
			fire_output(e, "onbreak", activator);
			e->flags |= EF_KILLED;
			break_effects(e, NULL);
		} else if (!strcasecmp(in, "sethealth")) {
			e->value = (f32)atof(param);
			e->armed = 1;
		}
		break;

	case EC_SCRIPT_INTRO:
		script_intro_input(e, in, param);
		break;

	case EC_ROTATING:
		rotating_input(e, in, param);
		break;

	case EC_SPRITE:
	case EC_LIGHTGLOW:
		sprite_input(e, in, param);
		break;

	case EC_DYNLIGHT:
		Light_DynamicInput((s32)(e - ents), in, param);
		break;

	case EC_LIGHT: {
		/* CLight: TurnOn / TurnOff / Toggle, SetPattern / FadeToPattern (the fade is done instantly) */
		int style = (int)kvf(e, "style", 0);
		if (!strcasecmp(in, "turnon"))
			Light_StyleSwitch(style, 1);
		else if (!strcasecmp(in, "turnoff"))
			Light_StyleSwitch(style, 0);
		else if (!strcasecmp(in, "toggle"))
			Light_StyleSwitch(style, -1);
		else if (!strcasecmp(in, "setpattern") || !strcasecmp(in, "fadetopattern")) {
			Light_StylePattern(style, param);
			Light_StyleSwitch(style, 1);
		}
		break;
	}

	case EC_VEHICLE:
		Veh_Input(e, in, param);
		break;

	case EC_POINT_TEMPLATE:
		if (!strcasecmp(in, "forcespawn")) {
			for (int k = 1; k <= 16; k++) {
				char key[16];
				snprintf(key, sizeof(key), "template%02d", k);
				const char *nm = kv(e, key);
				if (!nm || !*nm)
					continue;
				for (u32 j = 1; j < numEnts; j++)
					if (name_matches(&ents[j], nm) && (ents[j].flags & EF_DISABLED) && !(ents[j].flags & EF_KILLED)) {
						ents[j].flags &= ~EF_DISABLED;
						ents[j].enabled = 1;
					}
			}
			fire_output(e, "onentityspawned", activator);
		}
		break;

	case EC_WEAPON_STRIP:
		if (!strcasecmp(in, "strip") || !strcasecmp(in, "stripweaponsandsuit")) {
			if (cbs.stripWeapons)
				cbs.stripWeapons(0);
		} else if (!strcasecmp(in, "enable"))
			e->enabled = 1;
		else if (!strcasecmp(in, "disable"))
			e->enabled = 0;
		break;

	case EC_HURT:
		if (!strcasecmp(in, "enable"))
			e->enabled = 1;
		else if (!strcasecmp(in, "disable"))
			e->enabled = 0;
		else if (!strcasecmp(in, "toggle"))
			e->enabled = !e->enabled;
		break;

	case EC_SCENE:
		if (!strcasecmp(in, "start")) {
			if (e->scene >= 0) {
				Scene_Start((s32)(e - ents), (u32)e->scene, activator);
				break;
			}
			/* scene missing from the player's scenes.image: fire its timeline outputs in order */
			fire_output(e, "onstart", activator);
			f32 t = 0.5f;
			for (int i = 1; i <= 16; i++) {
				char out[16];
				snprintf(out, sizeof(out), "ontrigger%d", i);
				if (kv(e, out)) {
					queue(e->name ? e->name : "", "_scenetrigger", out, t, activator, (s32)(e - ents));
					t += 0.5f;
				}
			}
			queue(e->name ? e->name : "", "_scenedone", "", t + 0.5f, activator, (s32)(e - ents));
		} else if (!strcasecmp(in, "_scenetrigger"))
			fire_output(e, param, activator);
		else if (!strcasecmp(in, "_scenedone"))
			fire_output(e, "oncompletion", activator);
		else if (!strcasecmp(in, "cancel") || !strcasecmp(in, "cancelatnextinterrupt"))
			Scene_Cancel((s32)(e - ents));
		else if (!strcasecmp(in, "pause"))
			Scene_Pause((s32)(e - ents));
		else if (!strcasecmp(in, "resume") || !strcasecmp(in, "stopwaitingforactor"))
			Scene_Resume((s32)(e - ents));
		else if (!strcasecmp(in, "trigger")) {
			char out[24];
			snprintf(out, sizeof(out), "ontrigger%d", atoi(param));
			fire_output(e, out, activator);
		}
		break;

	case EC_SCRIPTED_SEQUENCE:
		if (!strcasecmp(in, "beginsequence"))
			script_start(e, 1);
		else if (!strcasecmp(in, "movetoposition"))
			script_start(e, 0);
		else if (!strcasecmp(in, "cancelsequence") || (!strcasecmp(in, "scriptplayerdeath") && kvf(e, "onplayerdeath", 0) == 1))
			script_cancel(e, activator);
		break;

	case EC_SCRIPT_CONDITIONS:
		if (!strcasecmp(in, "enable"))
			queue(e->name ? e->name : "", "_cond", "", 1.0f, activator, (s32)(e - ents));
		else if (!strcasecmp(in, "_cond"))
			fire_output(e, "onconditionssatisfied", activator);
		break;

	case EC_NPC: {
		Npc *n = Npc_OfEntity((s32)(e - ents));
		if (!n)
			break;
		if (!strcasecmp(in, "setanimation") || !strcasecmp(in, "setdefaultanimation") ||
		    !strcasecmp(in, "setidleanimation"))
			Npc_PlaySequence(n, param, 1);
		else if (!strcasecmp(in, "addtoplayersquad") ||
		         (!strcasecmp(in, "setsquad") && !strcasecmp(param, "player_squad"))) {
			if (n->followEnt != 0 && !Scene_NpcBusy(n))
				npc_say(e, "_follow");
			Npc_Follow(n, 0, 100.0f);
		} else if (!strcasecmp(in, "removefromplayersquad") || !strcasecmp(in, "setsquad")) {
			if (n->followEnt == 0 && !Scene_NpcBusy(n))
				npc_say(e, "_unfollow");
			Npc_Follow(n, -1, 0);
		}
		else if (!strcasecmp(in, "startscripting"))
			n->inAScript = 1;
		else if (!strcasecmp(in, "stopscripting"))
			n->inAScript = 0;
		else if (!strcasecmp(in, "giveweapon"))
			Npc_SetWeapon(n, param);
		else if (!strcasecmp(in, "sethealth") && atof(param) <= 0) {
			fire_output(e, "ondeath", activator);
			if (!Npc_Kill(n))
				e->flags |= EF_KILLED;
			e->flags &= ~EF_SOLID;
		}
		break;
	}

	case EC_ENV_GLOBAL: {
		const char *g = kv(e, "globalstate");
		int cur = Ent_GlobalState(g);
		if (!strcasecmp(in, "turnon"))
			global_set(g, 1, 0);
		else if (!strcasecmp(in, "turnoff"))
			global_set(g, 0, 0);
		else if (!strcasecmp(in, "remove"))
			global_set(g, 2, 0);
		else if (!strcasecmp(in, "toggle"))
			global_set(g, cur == 1 ? 0 : 1, 0);
		break;
	}
	case EC_ACTBUSY:
	case EC_ACTBUSY_QUEUE:
		actbusy_input(e, in, param, activator);
		break;

	case EC_FOLLOW_GOAL:
		if (!strcasecmp(in, "activate"))
			follow_goal(e, 1);
		else if (!strcasecmp(in, "deactivate"))
			follow_goal(e, 0);
		break;

	case EC_TEMPLATE_MAKER:
		if ((!strcasecmp(in, "spawn") || !strcasecmp(in, "forcespawn") || !strcasecmp(in, "spawnnpcinradius") ||
		     !strcasecmp(in, "spawnnpcinline") || !strcasecmp(in, "enable")) &&
		    e->actor > 0 && (ents[e->actor].flags & EF_DISABLED)) {
			/* no copies: the template itself steps in at the maker */
			Entity *t = &ents[e->actor];
			memcpy(t->origin, e->origin, sizeof(t->origin));
			t->angles[1] = e->angles[1];
			const char *nm = kv(e, "npctargetname");
			if (nm && nm[0])
				t->name = nm;
			t->flags &= ~EF_DISABLED;
			Npc *n = Npc_OfEntity(e->actor);
			if (n) {
				Npc_Teleport(n, t->origin, t->angles[1]);
				n->onGround = 0;
			}
			fire_output(e, "onspawnnpc", e->actor);
		}
		break;

	case EC_AMBIENT:
		if (!strcasecmp(in, "playsound"))
			ambient_play(e);
		else if (!strcasecmp(in, "stopsound") || !strcasecmp(in, "fadeout")) {
			Audio_Stop(e->voice);
			e->voice = 0;
		} else if (!strcasecmp(in, "volume")) {
			f32 v = (f32)atof(param) / 10.0f;
			if (v <= 0) {
				Audio_Stop(e->voice);
				e->voice = 0;
			} else if (e->voice && Audio_IsPlaying(e->voice))
				Audio_SetChannelVolume(e->voice, v);
		} else if (!strcasecmp(in, "togglesound")) {
			if (e->voice && Audio_IsPlaying(e->voice)) {
				Audio_Stop(e->voice);
				e->voice = 0;
			} else
				ambient_play(e);
		}
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ spawn */

static const struct {
	const char *name;
	u16 cls;
} classes[] = {
	{"logic_auto", EC_LOGIC_AUTO},
	{"logic_relay", EC_LOGIC_RELAY},
	{"logic_timer", EC_LOGIC_TIMER},
	{"math_counter", EC_MATH_COUNTER},
	{"logic_branch", EC_LOGIC_BRANCH},
	{"logic_compare", EC_LOGIC_COMPARE},
	{"trigger_once", EC_TRIGGER_ONCE},
	{"trigger_multiple", EC_TRIGGER_MULTIPLE},
	{"trigger_look", EC_TRIGGER_LOOK},
	{"trigger_playermovement", EC_TRIGGER_PLAYERMOVE},
	{"trigger_teleport", EC_TRIGGER_TELEPORT},
	{"trigger_push", EC_TRIGGER_PUSH},
	{"trigger_changelevel", EC_TRIGGER_CHANGELEVEL},
	{"point_teleport", EC_POINT_TELEPORT},
	{"func_door", EC_FUNC_DOOR},
	{"func_water_analog", EC_FUNC_MOVELINEAR},
	{"func_door_rotating", EC_FUNC_DOOR_ROTATING},
	{"momentary_rot_button", EC_MOMENTARY},
	{"player_loadsaved", EC_LOADSAVED},
	{"func_movelinear", EC_FUNC_MOVELINEAR},
	{"func_button", EC_FUNC_BUTTON},
	{"func_rot_button", EC_FUNC_BUTTON},
	{"func_brush", EC_FUNC_BRUSH},
	{"func_wall_toggle", EC_FUNC_BRUSH},
	{"func_wall", EC_FUNC_WALL},
	{"func_illusionary", EC_FUNC_ILLUSIONARY},
	{"func_breakable", EC_FUNC_BREAKABLE},
	{"func_breakable_surf", EC_FUNC_BREAKABLE},
	{"func_physbox", EC_FUNC_BREAKABLE},
	{"prop_door_rotating", EC_PROP_DOOR},
	{"logic_choreographed_scene", EC_SCENE},
	{"scripted_sequence", EC_SCRIPTED_SEQUENCE},
	{"ai_script_conditions", EC_SCRIPT_CONDITIONS},
	{"ambient_generic", EC_AMBIENT},
	{"ai_goal_follow", EC_FOLLOW_GOAL},
	{"ai_goal_actbusy", EC_ACTBUSY},
	{"env_global", EC_ENV_GLOBAL},
	{"ai_goal_actbusy_queue", EC_ACTBUSY_QUEUE},
	{"npc_template_maker", EC_TEMPLATE_MAKER},
	{"point_camera", EC_POINT_CAMERA},
	{"func_monitor", EC_FUNC_MONITOR},
	{"game_text", EC_GAME_TEXT},
	{"env_message", EC_ENV_MESSAGE},
	{"env_credits", EC_ENV_CREDITS},
	{"env_hudhint", EC_HUDHINT},
	{"env_microphone", EC_MICROPHONE},
	{"func_tracktrain", EC_TRACKTRAIN},
	{"path_track", EC_PATH},
	{"path_corner", EC_PATH},
	{"point_viewcontrol", EC_VIEWCONTROL},
	{"env_fade", EC_FADE},
	{"env_shake", EC_SHAKE},
	{"env_zoom", EC_ZOOM},
	{"point_template", EC_POINT_TEMPLATE},
	{"script_intro", EC_SCRIPT_INTRO},
	{"func_rotating", EC_ROTATING},
	{"env_sprite", EC_SPRITE},
	{"env_sprite_oriented", EC_SPRITE},
	{"env_glow", EC_SPRITE},
	{"env_lightglow", EC_LIGHTGLOW},
	{"light", EC_LIGHT},
	{"light_dynamic", EC_DYNLIGHT},
	{"light_spot", EC_LIGHT},
	{"point_spotlight", EC_SPRITE},
	{"prop_vehicle_airboat", EC_VEHICLE},
	{"prop_vehicle_jeep", EC_VEHICLE},
	{"prop_vehicle_prisoner_pod", EC_VEHICLE},
	{"trigger_weapon_strip", EC_WEAPON_STRIP},
	{"trigger_weapon_dissolve", EC_WEAPON_STRIP},
	{"player_weaponstrip", EC_WEAPON_STRIP},
	{"trigger_hurt", EC_HURT},
};

static u16 class_of(const char *cn)
{
	for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
		if (!strcmp(cn, classes[i].name))
			return classes[i].cls;
	if (!strncmp(cn, "prop_", 5) || !strcmp(cn, "item_item_crate"))
		return EC_PROP;
	return EC_OTHER;
}

/* brush entities that are only volumes or tools: never drawn */
static int invisible_brush(const char *cn)
{
	return !strncmp(cn, "trigger_", 8) || !strncmp(cn, "func_areaportal", 15) || !strcmp(cn, "func_occluder") ||
	       !strncmp(cn, "func_clip", 9) || !strcmp(cn, "func_viscluster") || !strcmp(cn, "func_ladder") ||
	       !strncmp(cn, "env_", 4) || !strcmp(cn, "func_precipitation") || !strcmp(cn, "func_smokevolume") ||
	       !strcmp(cn, "func_dustmotes") || !strcmp(cn, "func_vehicleclip") || !strcmp(cn, "func_nav_blocker") ||
	       !strcmp(cn, "func_lod") || !strcmp(cn, "game_zone_player");
}

static int solid_brush(u16 cls, const char *cn)
{
	switch (cls) {
	case EC_FUNC_DOOR:
	case EC_FUNC_DOOR_ROTATING:
	case EC_FUNC_MOVELINEAR:
	case EC_FUNC_BUTTON:
	case EC_FUNC_MONITOR:
	case EC_FUNC_BRUSH:
	case EC_FUNC_WALL:
	case EC_FUNC_BREAKABLE:
	case EC_TRACKTRAIN:
		return 1;
	default:
		return !strcmp(cn, "func_tracktrain") || !strcmp(cn, "func_train") || !strcmp(cn, "func_rotating") ||
		       !strcmp(cn, "func_monitor") || !strcmp(cn, "func_physbox");
	}
}

/* ---- entities made while playing (npc_maker, npc_template_maker, npc_antlion_template_maker,
 * env_entity_maker / point_template): copies of a template in spare slots */
#define ENT_SPARE 128
static u32 firstSpare;
static char *clonedNames[ENT_SPARE * 2];
static int numClonedNames;

static void spare_slot(Entity *e)
{
	memset(e, 0, sizeof(*e));
	e->classname = "";
	e->flags = EF_KILLED;
	e->bmodel = e->mdl = -1;
	e->npcModel = e->scene = e->sound = e->actor = -1;
	e->parent = -1;
	e->esnd[0] = e->esnd[1] = e->esnd[2] = -1;
	e->doorSnd[0] = e->doorSnd[1] = e->doorSnd[2] = -1;
	e->tex = e->tex2 = -1;
}

static const char *keep_name(const char *nm)
{
	if (!nm || !*nm || numClonedNames >= (int)(sizeof(clonedNames) / sizeof(clonedNames[0])))
		return nm && *nm ? nm : NULL;
	char *c = malloc(strlen(nm) + 1);
	if (!c)
		return NULL;
	strcpy(c, nm);
	clonedNames[numClonedNames++] = c;
	return c;
}

s32 Ent_Clone(s32 src, const char *classname, const f32 origin[3], const f32 angles[3], const char *name, s32 npcModel)
{
	if (src <= 0 || (u32)src >= numEnts)
		return -1;
	u32 slot = 0;
	for (u32 i = firstSpare; i < numEnts; i++)
		if (!ents[i].classname[0]) {
			slot = i;
			break;
		}
	if (!slot)
		return -1;
	Entity *e = &ents[slot];
	*e = ents[src];
	if (classname)
		e->classname = classname;
	e->cls = class_of(e->classname);
	if (npcModel >= 0) {
		e->npcModel = npcModel;
		e->cls = EC_NPC;
	}
	e->flags &= (u16) ~(EF_KILLED | EF_DISABLED | EF_FIRED | EF_TEMPLATE);
	if (e->npcModel >= 0 || e->mdl >= 0 || e->bmodel > 0)
		e->flags |= EF_DRAW;
	e->name = keep_name(name);
	memcpy(e->origin, origin, 12);
	memcpy(e->angles, angles, 12);
	Ent_AngleMatrix(e->angles, e->R);
	e->parent = -1;
	e->parentAttach[0] = 0;
	e->voice = e->sndLoop = e->sndStart = 0;
	e->sndMoving = 0;
	e->phase = 0;
	e->seen = 0;
	e->fired = 0;
	if (e->npcModel >= 0)
		Npc_Add((s32)slot);
	Phys_AddBody((s32)slot);
	Ent_UpdateCollision();
	return (s32)slot;
}

int Ent_Spawn(const Pack *p, Player *pl, int newGame, const EntCallbacks *cb)
{
	Ent_Free();
	P = p;
	player = pl;
	if (cb)
		cbs = *cb;
	now = 0;
	numEnts = p->numEntities + 1 + ENT_SPARE; /* slot 0 is the player; spares at the end (Ent_Clone) */
	firstSpare = p->numEntities + 1;
	ents = calloc(numEnts, sizeof(Entity));
	kvFired = calloc(p->numEntities ? p->entities[p->numEntities - 1].firstKV + p->entities[p->numEntities - 1].numKV : 1,
	                 sizeof(u16));
	events = malloc(MAX_EVENTS * sizeof(Event));
	solids = malloc(numEnts * sizeof(CMEnt));
	if (!ents || !kvFired || !events || !solids)
		return -1;
	numEvents = 0;
	reloadRequest = 0;
	ents[0].cls = EC_PLAYER;
	ents[0].classname = "player";
	ents[0].name = "!player";
	ents[0].bmodel = ents[0].mdl = -1;
	ents[0].npcModel = ents[0].scene = ents[0].sound = ents[0].actor = -1;
	for (u32 i = firstSpare; i < numEnts; i++)
		spare_slot(&ents[i]);

	for (u32 i = 0; i < p->numEntities; i++) {
		Entity *e = &ents[i + 1];
		e->esnd[0] = e->esnd[1] = e->esnd[2] = -1;
		e->firstKV = p->entities[i].firstKV;
		e->numKV = p->entities[i].numKV;
		e->classname = kv(e, "classname");
		if (!e->classname)
			e->classname = "";
		e->name = kv(e, "targetname");
		e->cls = class_of(e->classname);
		e->spawnflags = (u32)kvf(e, "spawnflags", 0);
		kvvec(e, "origin", e->origin);
		kvvec(e, "angles", e->angles);
		if (e->cls == EC_FUNC_DOOR || e->cls == EC_FUNC_MOVELINEAR || e->cls == EC_FUNC_BUTTON ||
		    e->cls == EC_FUNC_DOOR_ROTATING) {
			/* brush movers keep their compile-time orientation; motion is added on top */
			if (e->cls != EC_FUNC_DOOR_ROTATING)
				e->angles[0] = e->angles[1] = e->angles[2] = 0;
		}
		update_matrix(e);
		e->bmodel = e->mdl = -1;
		const char *model = kv(e, "model");
		if (model && model[0] == '*')
			e->bmodel = atoi(model + 1);
		const char *mdl = kv(e, "_mdl");
		if (mdl)
			e->mdl = atoi(mdl);
		const char *npcm = kv(e, "_npc"), *scn = kv(e, "_scene"), *snd = kv(e, "_snd");
		e->npcModel = npcm ? atoi(npcm) : -1;
		e->scene = scn ? atoi(scn) : -1;
		e->sound = snd ? atoi(snd) : -1;
		e->actor = -1;
		e->doorSnd[0] = e->doorSnd[1] = e->doorSnd[2] = -1;
		const char *ds = kv(e, "_doorsnd");
		if (ds) {
			int a = -1, b = -1, c = -1;
			sscanf(ds, "%d %d %d", &a, &b, &c);
			e->doorSnd[0] = (s16)a;
			e->doorSnd[1] = (s16)b;
			e->doorSnd[2] = (s16)c;
		}
		e->fadeMax = kvf(e, "fademaxdist", 0);
		const char *es = kv(e, "_esnd");
		if (es) {
			int a = -1, b = -1, c = -1;
			sscanf(es, "%d %d %d", &a, &b, &c);
			e->esnd[0] = (s16)a;
			e->esnd[1] = (s16)b;
			e->esnd[2] = (s16)c;
		}
		if (e->npcModel >= 0 && !kv(e, "_actor")) {
			e->cls = EC_NPC;
			if ((u32)e->npcModel < p->numNpcModels && p->npcModels[e->npcModel].radius >= 8 &&
			    p->npcModels[e->npcModel].height >= 40)
				e->flags |= EF_SOLID;
		}
		const char *tint = kv(e, "_tint");
		e->tint[0] = e->tint[1] = e->tint[2] = 128;
		e->tint[3] = 255;
		if (tint) {
			int r, g, b;
			if (sscanf(tint, "%d %d %d", &r, &g, &b) == 3) {
				e->tint[0] = (u8)r;
				e->tint[1] = (u8)g;
				e->tint[2] = (u8)b;
			}
		}
		e->enabled = kvf(e, "startdisabled", 0) == 0;

		if (e->bmodel > 0 && !invisible_brush(e->classname)) {
			const char *rm = kv(e, "rendermode");
			if (!rm || atoi(rm) != 10) /* rendermode 10 = don't render */
				e->flags |= EF_DRAW;
		}
		if (e->mdl >= 0)
			e->flags |= EF_DRAW;
		if (e->bmodel > 0 && solid_brush(e->cls, e->classname))
			e->flags |= EF_SOLID;
		if (e->cls == EC_FUNC_BRUSH && kvf(e, "solidity", 0) == 1)
			e->flags &= ~EF_SOLID;
		if (e->mdl >= 0) {
			const char *solid = kv(e, "solid");
			int debris = !strncmp(e->classname, "prop_physics", 12) && (e->spawnflags & 4);
			if (!(solid && atoi(solid) == 0) && !debris)
				e->flags |= EF_SOLID;
		}
		if ((e->cls == EC_FUNC_BRUSH || e->cls == EC_PROP) && !e->enabled)
			e->flags |= EF_DISABLED;

		switch (e->cls) {
		case EC_FUNC_DOOR:
		case EC_FUNC_DOOR_ROTATING:
		case EC_FUNC_MOVELINEAR:
		case EC_FUNC_BUTTON:
		case EC_PROP_DOOR:
			setup_door(e);
			break;
		case EC_MOMENTARY:
			momentary_setup(e);
			if (e->spawnflags & 1)
				e->flags &= ~EF_SOLID; /* SF_ROTBUTTON_NOTSOLID */
			break;
		case EC_LOGIC_TIMER:
			e->interval = kvf(e, "refiretime", 0);
			if (kvf(e, "userandomtime", 0) == 0 && e->interval < 0.01f)
				e->interval = 0.01f;
			e->phase = 0;
			if (e->enabled && (kvf(e, "refiretime", 0) > 0 || kvf(e, "userandomtime", 0) != 0))
				e->nextThink = timer_interval(e);
			else
				e->enabled = 0;
			break;
		case EC_MATH_COUNTER:
			e->value = kvf(e, "startvalue", 0);
			e->minv = kvf(e, "min", 0);
			e->maxv = kvf(e, "max", 0);
			if (e->minv > e->maxv) {
				f32 t = e->minv;
				e->minv = e->maxv;
				e->maxv = t;
			}
			if (e->minv != 0 || e->maxv != 0)
				e->value = e->value < e->minv ? e->minv : e->value > e->maxv ? e->maxv : e->value;
			e->enabled = kvf(e, "startdisabled", 0) == 0;
			break;
		case EC_LOGIC_BRANCH:
		case EC_LOGIC_COMPARE:
			e->value = kvf(e, "initialvalue", 0);
			e->compare = kvf(e, "comparevalue", 0);
			break;
		case EC_TRIGGER_MULTIPLE:
			/* CTriggerMultiple::Spawn: a wait of 0 is 0.2 s; -1 fires once and is removed */
			e->wait = kvf(e, "wait", 1);
			if (e->wait == 0)
				e->wait = 0.2f;
			break;
		case EC_MICROPHONE:
			e->enabled = kvf(e, "startdisabled", 0) == 0;
			break;
		case EC_TRACKTRAIN:
			/* (Spawn: no max speed is the start speed, or 100) */
			e->trainSpeed = kvf(e, "speed", 0);
			e->maxSpeed = kvf(e, "startspeed", 0);
			if (e->maxSpeed == 0)
				e->maxSpeed = e->trainSpeed != 0 ? e->trainSpeed : 100;
			e->minv = 1;
			e->maxv = 0;
			e->lastNode = -1;
			e->flags |= EF_SOLID;
			break;
		case EC_POINT_CAMERA:
			e->enabled = !(e->spawnflags & 1); /* "Start Off" */
			e->value = kvf(e, "fov", 90);
			break;
		default:
			break;
		}
	}

	/* trains start on their first path_track */
	for (u32 i = 1; i < numEnts; i++)
		if (ents[i].cls == EC_TRACKTRAIN) {
			train_teleport(&ents[i], node_named(kv(&ents[i], "target")), 1);
			if (ents[i].pathNode <= 0)
				ents[i].pathNode = node_next(node_named(kv(&ents[i], "target")), 0);
		}
	activeView = -1;
	memset(&intro, 0, sizeof(intro));
	intro.ent = intro.camera = -1;
	for (int q = 0; q < MAX_QUEUES; q++)
		queues[q].goal = -1;
	memset(busy, 0, sizeof(busy));
	setup_hinges();
	/* useable ladders: func_useableladder (point0/point1) and func_ladderendpoint pairs, plus dismount spots */
	{
		static f32 lad[64][2][3], dis[256][3];
		int nl = 0, nd = 0;
		for (u32 i = 1; i < numEnts && nl < 64; i++) {
			Entity *e = &ents[i];
			f32 a[3], b[3];
			if (!strcasecmp(e->classname, "func_useableladder")) {
				kvvec(e, "point0", a);
				kvvec(e, "point1", b);
			} else if (!strcasecmp(e->classname, "func_ladderendpoint")) {
				Entity *o = find_named(kv(e, "target"));
				if (!o || o <= e)
					continue; /* each pair once */
				memcpy(a, e->origin, 12);
				memcpy(b, o->origin, 12);
			} else {
				continue;
			}
			int lo = a[2] <= b[2];
			memcpy(lad[nl][0], lo ? a : b, 12);
			memcpy(lad[nl][1], lo ? b : a, 12);
			nl++;
		}
		for (u32 i = 1; i < numEnts && nd < 256; i++)
			if (!strcasecmp(ents[i].classname, "info_ladder_dismount"))
				memcpy(dis[nd++], ents[i].origin, 12);
		Player_SetLadders((const f32(*)[2][3])lad, nl, (const f32(*)[3])dis, nd);
	}
	/* env_global "Set Initial State": only if the global isn't known yet (it carries over from earlier maps) */
	for (u32 i = 1; i < numEnts; i++)
		if (ents[i].cls == EC_ENV_GLOBAL && (ents[i].spawnflags & 1))
			global_set(kv(&ents[i], "globalstate"), (int)kvf(&ents[i], "initialstate", 0), 1);
	intro.queuedBlendMode = intro.queuedNextBlendMode = -1;
	for (u32 i = 1; i < numEnts; i++)
		if (ents[i].cls == EC_SCRIPT_INTRO)
			intro.alternateFov = kvf(&ents[i], "alternatefovchange", 0) != 0;
	fadeOn = 0;
	shakeUntil = 0;
	zoomOn = 0;

	/* monitors find their cameras */
	for (u32 i = 1; i < numEnts; i++)
		if (ents[i].cls == EC_FUNC_MONITOR) {
			Entity *c = find_named(kv(&ents[i], "target"));
			ents[i].actor = c ? (s32)(c - ents) : -1;
		}

	/* templates are hidden until their maker spawns them */
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (e->cls != EC_TEMPLATE_MAKER)
			continue;
		Entity *t = find_named(kv(e, "templatename"));
		if (t) {
			t->flags |= EF_DISABLED;
			e->actor = (s32)(t - ents);
		}
	}
	/* parentname: "name" or "name,attachment" */
	for (u32 i = 1; i < numEnts; i++) {
		ents[i].parent = -1;
		const char *pn = kv(&ents[i], "parentname");
		if (!pn || !*pn)
			continue;
		char nm[64];
		snprintf(nm, sizeof(nm), "%s", pn);
		char *comma = strchr(nm, ',');
		if (comma)
			*comma = 0;
		Entity *pa = find_named(nm);
		if (!pa || pa == &ents[i])
			continue;
		/* Source attaches children where the map places both; trains have already jumped to their first
		 * path_track by now, so take the parent's frame from the map, not from where it is */
		f32 so[3], sa[3], sR[3][3];
		memcpy(so, pa->origin, 12);
		memcpy(sa, pa->angles, 12);
		memcpy(sR, pa->R, sizeof(sR));
		kvvec(pa, "origin", pa->origin);
		kvvec(pa, "angles", pa->angles);
		update_matrix(pa);
		/* the child too: a train jumping to its path start has already carried the NPCs riding it */
		if (ents[i].npcModel >= 0 && ents[i].bmodel <= 0)
			kvvec(&ents[i], "origin", ents[i].origin);
		set_parent(&ents[i], pa);
		memcpy(pa->origin, so, 12);
		memcpy(pa->angles, sa, 12);
		memcpy(pa->R, sR, sizeof(sR));
	}

	/* point_template: the entities it names wait, hidden, for ForceSpawn (unless "Don't remove") */
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (e->cls != EC_POINT_TEMPLATE || (e->spawnflags & 1))
			continue;
		for (int k = 1; k <= 16; k++) {
			char key[16];
			snprintf(key, sizeof(key), "template%02d", k);
			const char *nm = kv(e, key);
			if (!nm || !*nm)
				continue;
			for (u32 j = 1; j < numEnts; j++)
				if (j != i && name_matches(&ents[j], nm))
					ents[j].flags |= EF_DISABLED;
		}
	}
	Gibs_Init(p);
	if (Npc_Spawn(p, pl) != 0 || Phys_Init(p, pl) != 0 || Face_Init(p) != 0)
		return -1;
	Scene_Init(p, pl);
	for (u32 i = 1; i < numEnts; i++)
		if (ents[i].cls == EC_DYNLIGHT)
			Light_DynamicSpawn((s32)i);
	movers_init();
	movers_snapshot();
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (e->cls == EC_SCRIPTED_SEQUENCE) {
			e->nextCine = -1;
			e->cineDelay = 0;
			e->cineWait = e->cineReady = e->cineStarted = 0;
			/* (Spawn: one with no name, or "start on spawn", goes for its NPC a second in; a named one
			 * then waits at the mark for BeginSequence) */
			if (!e->name || !e->name[0] || (e->spawnflags & 16)) {
				e->cineWait = e->name && e->name[0];
				e->actor = -1;
				e->phase = SS_MOVING;
				e->phaseUntil = now + 1.0f;
			}
		}
		else if (e->cls == EC_FOLLOW_GOAL && kvf(e, "startactive", 0) != 0)
			follow_goal(e, 1);
		else if (e->cls == EC_ACTBUSY && kvf(e, "startactive", 0) != 0)
			actbusy_activate(e);
		else if (e->cls == EC_AMBIENT && !(e->spawnflags & 16) && !(e->spawnflags & 32))
			ambient_play(e); /* looping sounds that don't start silent play from the start, like Source */
		else if (e->cls == EC_ROTATING) {
			memcpy(e->ang1, e->angles, sizeof(e->ang1)); /* (m_angStart) */
			e->speed = e->value = e->frac = 0;
			e->latch = 0;
			e->enabled = 0;
			if (e->spawnflags & 1)
				rotating_input(e, "start", "");
		}
		else if (e->cls == EC_SPRITE || e->cls == EC_LIGHTGLOW)
			sprite_setup(e);
	}

	/* the chapter title (worldspawn's chaptertitle, exported as an env_message): shown a second in, once:
	 * restarting the same map after dying doesn't show it again (Source's save no longer has it) */
	static char lastTitle[64];
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		const char *t;
		if (e->cls != EC_ENV_MESSAGE || !kv(e, "_auto") || !(t = kv(e, "_text")))
			continue;
		if (strcmp(t, lastTitle) && e->name)
			queue(e->name, "ShowMessage", "", 1.0f, 0, (s32)i);
		snprintf(lastTitle, sizeof(lastTitle), "%s", t);
	}

	EnvEnts_Spawn(); /* ropes, sparks, beams, fires... (envents.c) */
	Rel_Spawn();     /* ai_relationships that start active */
	AiEnts_Spawn();  /* makers, scripted schedules, assaults, logic_case... (aients.c) */
	Sentries_Spawn(); /* cameras and turrets */
	Flyers_Spawn();   /* manhacks and scanners */
	Mines_Spawn();    /* hopper mines */
	Tanks_Spawn();    /* mounted guns */
	Objects_Spawn();  /* canisters, thumpers, snipers, launchers, drivers */
	/* map start: logic_auto thinks 0.2 s in (CLogicAuto::Activate); logic_relay's OnSpawn just after spawn */
	autoNewGame = newGame;
	if (player)
		player->forceDuck = 0;
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (e->cls == EC_LOGIC_AUTO)
			e->nextThink = now + 0.2f;
		else if (e->cls == EC_LOGIC_RELAY && kv(e, "onspawn")) {
			fire_output(e, "onspawn", (s32)i);
			if (e->spawnflags & 1)
				e->flags |= EF_KILLED;
		}
	}
	Ent_UpdateCollision();
	return 0;
}

const Pack *Ent_Pack(void) { return P; }

void Ent_Free(void)
{
	/* (the movers table is per map: the next map's spawn must not see this one's) */
	free(moverSlot);
	moverSlot = NULL;
	numMovers = 0;
	EnvEnts_Free();
	Rel_Free();
	AiEnts_Free();
	Flyers_Free();
	for (int i = 0; i < numClonedNames; i++)
		free(clonedNames[i]);
	numClonedNames = 0;
	Hud_Clear();
	Scene_Free();
	Phys_Free();
	Face_Free();
	Npc_Free();
	Audio_StopAll();
	free(ents);
	free(kvFired);
	free(events);
	free(solids);
	ents = NULL;
	kvFired = NULL;
	events = NULL;
	solids = NULL;
	numEnts = numEvents = numSolids = 0;
	CM_SetEntities(NULL, 0);
}

/* ------------------------------------------------------------------ per tick */

/* func_door with "Touch Opens": the player walking into it opens it */
static void touch_doors(void)
{
	f32 pm[3], px[3];
	Player_Bounds(player, pm, px);
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (!alive(e) || (e->cls != EC_FUNC_DOOR && e->cls != EC_FUNC_DOOR_ROTATING) || !(e->spawnflags & 1024))
			continue;
		if (e->moveState != MV_CLOSED || e->locked)
			continue;
		f32 mn[3], mx[3];
		world_bounds(e, mn, mx);
		int in = 1;
		for (int k = 0; k < 3; k++)
			if (player->origin[k] + px[k] < mn[k] - 4 || player->origin[k] + pm[k] > mx[k] + 4)
				in = 0;
		if (in)
			mover_open(e, 0);
	}
}

int Ent_NpcOpenDoor(s32 npcEnt, const f32 pos[3], const f32 dir[3])
{
	f32 probe[3] = {pos[0] + dir[0] * 40, pos[1] + dir[1] * 40, pos[2] + 36};
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (!alive(e) || e->locked)
			continue;
		if (e->cls != EC_PROP_DOOR && e->cls != EC_FUNC_DOOR && e->cls != EC_FUNC_DOOR_ROTATING)
			continue;
		if (e->cls != EC_PROP_DOOR && (e->spawnflags & 512)) /* "NPCs can't" */
			continue;
		if (e->moveState != MV_CLOSED && e->moveState != MV_CLOSING)
			continue;
		f32 mn[3], mx[3];
		world_bounds(e, mn, mx);
		int in = 1;
		for (int k = 0; k < 3; k++)
			if (probe[k] < mn[k] - 16 || probe[k] > mx[k] + 16)
				in = 0;
		if (!in)
			continue;
		if (e->cls == EC_PROP_DOOR)
			prop_door_direction_from(e, pos, 0); /* (NPCOpenDoor: away from the NPC) */
		mover_open(e, npcEnt);
		if (e->cls == EC_PROP_DOOR && e->wait < 0)
			e->waitUntil = now + 3.0f, e->wait = 3.0f; /* let it swing shut behind them */
		return 1;
	}
	return 0;
}

/* trigger_look (CTriggerLook): while the player is inside and looking at its target (within FieldOfView,
 * as a dot product; by his velocity with flag 256) for LookTime in a row, OnTrigger; if he hasn't by
 * "timeout" seconds after coming in, OnTimeout instead. Flag 128: only once.
 * value: time looked so far (-1 not looking); nextThink: when the timeout goes; latch bit 0: it went */
static void look_trigger(Entity *e)
{
	if (!e->enabled) {
		e->touching = 0;
		return;
	}
	s32 who = trigger_toucher(e);
	int in = who == 0;
	if (in && !e->touching) {
		e->touching = 1;
		e->toucher = 0;
		fire_output(e, "onstarttouch", 0);
		f32 to = kvf(e, "timeout", 0);
		if (to > 0) {
			e->latch &= ~1;
			e->nextThink = now + to;
		}
		e->value = -1;
	} else if (!in && e->touching) {
		e->touching = 0;
		fire_output(e, "onendtouch", 0);
	}
	if (e->nextThink > 0 && now >= e->nextThink) { /* (TimeoutThink) */
		e->nextThink = 0;
		fire_output(e, "ontimeout", 0);
		e->latch |= 1;
		if (e->spawnflags & 128)
			e->flags |= EF_KILLED;
		return;
	}
	if (!in || (e->latch & 1))
		return;
	Entity *t = find_named(kv(e, "target"));
	if (!t)
		return;
	f32 look[3];
	if (e->spawnflags & 256) {
		memcpy(look, player->velocity, 12);
	} else {
		const f32 d2r = (f32)M_PI / 180.0f;
		look[0] = cosf(player->pitch * d2r) * cosf(player->yaw * d2r);
		look[1] = cosf(player->pitch * d2r) * sinf(player->yaw * d2r);
		look[2] = -sinf(player->pitch * d2r);
	}
	f32 eye[3] = {player->origin[0], player->origin[1], player->origin[2] + (player->ducked ? 28 : 64)};
	f32 to[3] = {t->origin[0] - eye[0], t->origin[1] - eye[1], t->origin[2] - eye[2]};
	f32 ll = sqrtf(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
	f32 tl = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
	if (ll < 1e-4f || tl < 1e-4f)
		return;
	f32 dot = (look[0] * to[0] + look[1] * to[1] + look[2] * to[2]) / (ll * tl);
	if (dot > kvf(e, "fieldofview", 0.9f)) {
		if (e->value < 0)
			e->value = 0;
		else
			e->value += TICK_INTERVAL;
		if (e->value >= kvf(e, "looktime", 0.5f)) {
			fire_output(e, "ontrigger", 0);
			e->value = -1;
			e->nextThink = 0; /* (the timeout is off) */
			if (e->spawnflags & 128)
				e->flags |= EF_KILLED;
		}
	} else {
		e->value = -1;
	}
}

static void touch_triggers(void)
{
	touch_doors();
	gather_npcs();
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (e->flags & EF_KILLED)
			continue;
		if (e->cls == EC_TRIGGER_LOOK) {
			look_trigger(e);
			continue;
		}
		if (e->cls == EC_TRIGGER_PLAYERMOVE) {
			/* CTriggerPlayerMovement: auto-duck (0x800) while the player is inside */
			int pin = e->enabled && trigger_toucher(e) == 0;
			if (pin != e->touching) {
				e->touching = (u8)pin;
				if (e->spawnflags & 0x800)
					player->forceDuck = pin;
				fire_output(e, pin ? "onstarttouch" : "onendtouch", 0);
			}
			continue;
		}
		if (e->cls != EC_TRIGGER_ONCE && e->cls != EC_TRIGGER_MULTIPLE && e->cls != EC_TRIGGER_TELEPORT &&
		    e->cls != EC_TRIGGER_PUSH && !(e->cls == EC_FUNC_BUTTON && (e->spawnflags & 256)) &&
		    !(e->cls == EC_WEAPON_STRIP && e->bmodel > 0) && !(e->cls == EC_HURT && e->bmodel > 0))
			continue;
		if (!e->enabled && e->cls != EC_FUNC_BUTTON) {
			e->touching = 0;
			continue;
		}
		s32 who = 0;
		int in;
		if (e->cls == EC_TRIGGER_ONCE || e->cls == EC_TRIGGER_MULTIPLE || e->cls == EC_TRIGGER_TELEPORT) {
			who = trigger_toucher(e);
			in = who >= 0;
			if (e->cls == EC_TRIGGER_TELEPORT && who > 0) {
				/* an NPC or a physics object (the plug into its socket): CTriggerTeleport moves it to the
				 * destination at once, every time it touches */
				Entity *dest = find_named(kv(e, "target"));
				if (dest) {
					Npc *tn = ents[who].cls == EC_NPC ? Npc_OfEntity(who) : NULL;
					if (tn)
						Npc_Teleport(tn, dest->origin, dest->angles[1]);
					else if (Phys_IsBody(who)) {
						Phys_SetAngles(who, dest->angles);
						/* (the origin, not the centre of mass, goes to the destination) */
						f32 off[3] = {ents[who].origin[0] - Phys_Com(who)[0], ents[who].origin[1] - Phys_Com(who)[1],
						              ents[who].origin[2] - Phys_Com(who)[2]};
						f32 c[3] = {dest->origin[0] - off[0], dest->origin[1] - off[1], dest->origin[2] - off[2]};
						Phys_Teleport(who, c);
					}
					fire_output(e, "onstarttouch", who);
				}
				in = 0;
			}
		} else
			in = player_touches(e);
		if (e->cls == EC_HURT && (e->spawnflags & (2 | 64)) && now >= e->nextThink) {
			/* CTriggerHurt::HurtAllTouchers: NPCs inside take damage per second too, in half-second bites
			 * (Ravenholm's traps, the barnacle pits) */
			e->nextThink = now + 0.5f;
			const char *filt = kv(e, "filtername");
			f32 mn[3], mx[3], c[3];
			world_bounds(e, mn, mx);
			for (int k = 0; k < 3; k++)
				c[k] = (mn[k] + mx[k]) * 0.5f;
			for (int j = 0; j < numLiveNpcs; j++) {
				Entity *n = &ents[liveNpcs[j]];
				if (npc_touches(e, n) && filter_pass_depth(filt, liveNpcs[j], 0))
					Ent_Damage(liveNpcs[j], kvf(e, "damage", 10) * 0.5f, NULL, c);
			}
		}
		if (e->cls == EC_HURT && ((u32)kvf(e, "damagetype", 0) & 262144) && cbs.geiger) {
			/* CTriggerHurt::RadiationThink: distance from the trigger's centre to the player's eyes */
			f32 mn[3], mx[3];
			{
				world_bounds(e, mn, mx);
				f32 d[3];
				for (int k = 0; k < 3; k++)
					d[k] = (mn[k] + mx[k]) * 0.5f - (player->origin[k] + (k == 2 ? 64 : 0));
				cbs.geiger(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
			}
		}
		if (in && !e->touching) {
			e->touching = 1;
			e->toucher = who;
			/* (CBaseTrigger::StartTouch: OnStartTouch, and OnStartTouchAll for the first one in) */
			if (e->cls == EC_TRIGGER_ONCE || e->cls == EC_TRIGGER_MULTIPLE)
				fire_output(e, "onstarttouchall", who);
			switch (e->cls) {
			case EC_TRIGGER_ONCE:
				fire_output(e, "onstarttouch", who);
				fire_output(e, "ontrigger", who);
				e->flags |= EF_KILLED;
				break;
			case EC_TRIGGER_MULTIPLE:
				fire_output(e, "onstarttouch", who);
				if (now >= e->waitUntil) {
					fire_output(e, "ontrigger", who);
					e->waitUntil = now + e->wait;
					if (e->wait < 0)
						e->flags |= EF_KILLED; /* (ActivateMultiTrigger: no wait, it's removed) */
				}
				break;
			case EC_TRIGGER_TELEPORT: {
				Entity *dest = find_named(kv(e, "target"));
				if (dest)
					teleport_player_to(dest);
				fire_output(e, "onstarttouch", 0);
				break;
			}
			case EC_FUNC_BUTTON:
				press_button(e, 0, BTN_USE);
				break;
			case EC_WEAPON_STRIP:
				if (cbs.stripWeapons)
					cbs.stripWeapons(!strcmp(e->classname, "trigger_weapon_dissolve"));
				fire_output(e, "onstarttouch", 0);
				break;
			default:
				fire_output(e, "onstarttouch", who > 0 ? who : 0);
				break;
			}
		} else if (in && e->cls == EC_HURT) {
			/* trigger_hurt: damage per second, dealt in half-second bites like Source */
			if (now >= e->waitUntil && (cbs.hurtPlayer || cbs.hurtPlayerFrom)) {
				f32 mn[3], mx[3], c[3];
				world_bounds(e, mn, mx);
				int hasC = 1;
				for (int k = 0; k < 3; k++)
					c[k] = (mn[k] + mx[k]) * 0.5f;
				if (cbs.hurtPlayerFrom)
					cbs.hurtPlayerFrom(kvf(e, "damage", 10) * 0.5f, (u32)kvf(e, "damagetype", 0), hasC ? c : NULL);
				else
					cbs.hurtPlayer(kvf(e, "damage", 10) * 0.5f);
				fire_output(e, "onhurtplayer", 0);
				e->waitUntil = now + 0.5f;
			}
		} else if (in && e->cls == EC_TRIGGER_PUSH) {
			/* CTriggerPush::Touch: the player gets a base velocity of pushdir * speed while inside (a
			 * steady push like a current, not one that keeps adding up); pushdir turns with the trigger */
			f32 dir[3], ang[3], w[3];
			kvvec(e, "pushdir", ang);
			f32 speed = kvf(e, "speed", 40);
			const f32 d = (f32)M_PI / 180.0f;
			dir[0] = cosf(ang[0] * d) * cosf(ang[1] * d);
			dir[1] = cosf(ang[0] * d) * sinf(ang[1] * d);
			dir[2] = -sinf(ang[0] * d);
			for (int k = 0; k < 3; k++)
				w[k] = e->R[k][0] * dir[0] + e->R[k][1] * dir[1] + e->R[k][2] * dir[2];
			f32 along = player->velocity[0] * w[0] + player->velocity[1] * w[1] + player->velocity[2] * w[2];
			if (along < speed)
				for (int k = 0; k < 3; k++)
					player->velocity[k] += w[k] * (speed - along);
			if (e->spawnflags & 128) /* "once only" */
				e->flags |= EF_KILLED;
		} else if (in && e->cls == EC_TRIGGER_MULTIPLE && now >= e->waitUntil) {
			fire_output(e, "ontrigger", who); /* (MultiTouch: again every "wait" while someone's inside) */
			e->waitUntil = now + e->wait;
			if (e->wait < 0)
				e->flags |= EF_KILLED;
		} else if (!in && e->touching) {
			e->touching = 0;
			fire_output(e, "onendtouch", e->toucher);
			if (e->cls == EC_TRIGGER_ONCE || e->cls == EC_TRIGGER_MULTIPLE)
				fire_output(e, "onendtouchall", e->toucher);
		}
	}
}

void Ent_UpdateCollision(void)
{
	numSolids = 0;
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		/* func_clip_vphysics: a wall only physics objects meet (props kept off ledges and out of corners) */
		if (e->bmodel > 0 && alive(e) && !strcmp(e->classname, "func_clip_vphysics")) {
			CMEnt *c = &solids[numSolids++];
			memset(c, 0, sizeof(*c));
			c->ent = (s32)i;
			c->bmodel = e->bmodel;
			c->mdl = -1;
			memcpy(c->origin, e->origin, sizeof(c->origin));
			memcpy(c->R, e->R, sizeof(c->R));
			c->rotated = (u8)has_rotation(e);
			world_bounds(e, c->mins, c->maxs);
			c->physOnly = 1;
			continue;
		}
		if (!(e->flags & EF_SOLID) || !alive(e))
			continue;
		if (e->npcModel >= 0 && e->cls == EC_NPC) {
			/* NPC body: an upright box. Left out while it overlaps the player, so nobody gets stuck */
			const PackNpcModel *m = &P->npcModels[e->npcModel];
			f32 pm[3], px[3];
			Player_Bounds(player, pm, px);
			CMEnt *c = &solids[numSolids];
			c->physOnly = 0;
			c->ent = (s32)i;
			c->bmodel = c->mdl = -1;
			memcpy(c->origin, e->origin, sizeof(c->origin));
			memset(c->R, 0, sizeof(c->R));
			c->R[0][0] = c->R[1][1] = c->R[2][2] = 1;
			c->rotated = 0;
			f32 h = m->height > 72 ? 72 : m->height;
			c->mins[0] = e->origin[0] - 13;
			c->mins[1] = e->origin[1] - 13;
			c->mins[2] = e->origin[2];
			c->maxs[0] = e->origin[0] + 13;
			c->maxs[1] = e->origin[1] + 13;
			c->maxs[2] = e->origin[2] + h;
			int overlap = 1;
			for (int k = 0; k < 3; k++)
				if (player->origin[k] + px[k] <= c->mins[k] || player->origin[k] + pm[k] >= c->maxs[k])
					overlap = 0;
			if (!overlap)
				numSolids++;
			continue;
		}
		if (e->bmodel <= 0 && e->mdl < 0)
			continue;
		CMEnt *c = &solids[numSolids++];
		c->physOnly = 0;
		c->ent = (s32)i;
		c->bmodel = e->bmodel;
		c->mdl = e->bmodel > 0 ? -1 : e->mdl;
		memcpy(c->origin, e->origin, sizeof(c->origin));
		memcpy(c->R, e->R, sizeof(c->R));
		c->rotated = (u8)has_rotation(e);
		world_bounds(e, c->mins, c->maxs);
	}
	CM_SetEntities(solids, numSolids);
}

void Ent_Tick(f32 dt)
{
	now += dt;
	/* entity 0 stands where the player is (the effect and AI entities read it like any other) */
	if (player) {
		memcpy(ents[0].origin, player->origin, 12);
		ents[0].angles[1] = player->yaw;
	}
	memset(playerCarry, 0, sizeof(playerCarry));
	movers_snapshot();
	intro_tick();
	Gibs_Tick(dt);
	EnvEnts_Tick(dt);
	AiEnts_Tick(dt);
	Sentries_Tick(dt);
	Flyers_Tick(dt);
	Mines_Tick(dt);
	Tanks_Tick(dt);
	Objects_Tick(dt);
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (e->flags & EF_KILLED)
			continue;
		switch (e->cls) {
		case EC_FUNC_DOOR:
		case EC_FUNC_DOOR_ROTATING:
		case EC_FUNC_MOVELINEAR:
		case EC_FUNC_BUTTON:
		case EC_PROP_DOOR:
			if (e->moveState == MV_OPENING || e->moveState == MV_CLOSING || e->moveState == MV_OPEN)
				mover_tick(e, dt);
			break;
		case EC_LOGIC_TIMER:
			if (e->enabled && now >= e->nextThink)
				timer_fire(e);
			break;
		case EC_LOGIC_AUTO:
			/* CLogicAuto: 0.2 s after the map starts, if its global state (if any) is on */
			if (e->nextThink > 0 && now >= e->nextThink) {
				e->nextThink = 0;
				const char *gs = kv(e, "globalstate");
				int gi = gs && *gs ? global_find(gs) : -1;
				if (!(gs && *gs) || (gi >= 0 && globals[gi].state == 1)) {
					fire_output(e, autoNewGame ? "onnewgame" : "onmaptransition", 0);
					fire_output(e, "onmapspawn", 0);
					if (e->spawnflags & 1)
						e->flags |= EF_KILLED;
				}
			}
			break;
		case EC_SCRIPTED_SEQUENCE:
			if (e->phase != SS_NONE)
				script_tick(e);
			break;
		case EC_TRACKTRAIN:
			train_tick(e, dt);
			break;
		case EC_ACTBUSY_QUEUE:
			queue_tick(e);
			break;
		case EC_FUNC_BREAKABLE:
			if (e->hinged && !(e->flags & EF_DISABLED))
				hinge_tick(e, dt);
			break;
		case EC_ROTATING:
			rotating_tick(e, dt);
			break;
		case EC_MOMENTARY:
			momentary_tick(e, dt);
			break;
		case EC_LOADSAVED:
			if (e->nextThink > 0 && now >= e->nextThink) {
				e->nextThink = 0;
				reloadRequest = 1;
			}
			break;
		default:
			break;
		}
	}
	busy_tick();
	follow_parents();
	motion_sounds();
	Ent_UpdateCollision(); /* NPCs riding trains trace against where the train is now, not a tick ago */
	push_movers();
	unstick_player();
	Veh_Tick(dt);
	view_tick(dt);
	Npc_Tick(dt);
	Scene_Tick(dt);
	follow_attachments();
	Phys_Tick(dt);
	touch_triggers();
	run_events();
	Ent_UpdateCollision();
}

/* ------------------------------------------------------------------ use */

static int ray_box(const f32 *o, const f32 *d, f32 len, const f32 *mn, const f32 *mx, f32 *t)
{
	f32 t0 = 0, t1 = len;
	for (int k = 0; k < 3; k++) {
		if (fabsf(d[k]) < 1e-6f) {
			if (o[k] < mn[k] || o[k] > mx[k])
				return 0;
			continue;
		}
		f32 a = (mn[k] - o[k]) / d[k], b = (mx[k] - o[k]) / d[k];
		if (a > b) {
			f32 x = a;
			a = b;
			b = x;
		}
		if (a > t0)
			t0 = a;
		if (b < t1)
			t1 = b;
		if (t0 > t1)
			return 0;
	}
	*t = t0;
	return 1;
}

void Ent_Use(const f32 eye[3], const f32 fwd[3])
{
	if (Phys_Holding()) {
		Phys_Drop(0);
		return;
	}
	Entity *best = NULL;
	f32 bestT = USE_RANGE;
	/* what's actually in front of the eye (Source traces the view): a wall in between stops the use */
	{
		Trace tr;
		f32 end[3] = {eye[0] + fwd[0] * USE_RANGE, eye[1] + fwd[1] * USE_RANGE, eye[2] + fwd[2] * USE_RANGE};
		f32 z[3] = {0, 0, 0};
		CM_BoxTrace(&tr, eye, end, z, z, CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW);
		if (tr.fraction < 1)
			bestT = tr.fraction * USE_RANGE + 2.0f;
		if (tr.ent > 0 && alive(&ents[tr.ent])) {
			Entity *h = &ents[tr.ent];
			if (h->cls == EC_FUNC_BUTTON || h->cls == EC_FUNC_DOOR || h->cls == EC_FUNC_DOOR_ROTATING ||
			    h->cls == EC_PROP_DOOR || h->cls == EC_PROP || h->cls == EC_NPC || h->cls == EC_VEHICLE)
				best = h;
		}
	}
	for (u32 i = 1; i < numEnts && !best; i++) {
		Entity *e = &ents[i];
		if (!alive(e))
			continue;
		if (e->cls != EC_FUNC_BUTTON && e->cls != EC_FUNC_DOOR && e->cls != EC_FUNC_DOOR_ROTATING &&
		    e->cls != EC_PROP_DOOR && e->cls != EC_PROP && e->cls != EC_NPC && e->cls != EC_VEHICLE)
			continue;
		f32 mn[3], mx[3], t;
		world_bounds(e, mn, mx);
		if (e->cls == EC_NPC) {
			const PackNpcModel *m = &P->npcModels[e->npcModel];
			for (int k = 0; k < 2; k++) {
				mn[k] = e->origin[k] - 16;
				mx[k] = e->origin[k] + 16;
			}
			mn[2] = e->origin[2];
			mx[2] = e->origin[2] + (m->height > 16 ? m->height : 16);
		}
		if (ray_box(eye, fwd, USE_RANGE, mn, mx, &t) && t < bestT && t > 0.0f) {
			bestT = t;
			best = e;
		}
	}
	if (!best) {
		/* CBasePlayer::FindUseEntity: nothing under the crosshair, so the usable thing nearest the
		 * middle of the view, within reach (its box's nearest point within 80 units) and no more
		 * than ~37 degrees off (dot >= 0.8), that the eye can see. Needed with stick aiming. */
		f32 bestDot = 0.8f;
		for (u32 i = 1; i < numEnts; i++) {
			Entity *e = &ents[i];
			if (!alive(e))
				continue;
			if (e->cls != EC_FUNC_BUTTON && e->cls != EC_FUNC_DOOR && e->cls != EC_FUNC_DOOR_ROTATING &&
			    e->cls != EC_PROP_DOOR && e->cls != EC_PROP && e->cls != EC_NPC && e->cls != EC_VEHICLE)
				continue;
			if (e->cls == EC_PROP && !Phys_IsBody((s32)i) && !kv(e, "onplayeruse"))
				continue; /* (static-looking props aren't usable) */
			f32 mn[3], mx[3], np[3], d[3], dl = 0;
			world_bounds(e, mn, mx);
			for (int k = 0; k < 3; k++) {
				np[k] = eye[k] < mn[k] ? mn[k] : eye[k] > mx[k] ? mx[k] : eye[k];
				d[k] = np[k] - eye[k];
				dl += d[k] * d[k];
			}
			if (dl > USE_RANGE * USE_RANGE)
				continue;
			dl = sqrtf(dl);
			f32 dot;
			if (dl < 1.0f) {
				dot = 1.0f;
			} else {
				/* aim at the box's middle when its nearest point is straight ahead or behind */
				f32 c[3] = {(mn[0] + mx[0]) * 0.5f - eye[0], (mn[1] + mx[1]) * 0.5f - eye[1], (mn[2] + mx[2]) * 0.5f - eye[2]};
				f32 cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
				f32 d1 = (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / dl;
				f32 d2 = cl > 0.01f ? (c[0] * fwd[0] + c[1] * fwd[1] + c[2] * fwd[2]) / cl : 1.0f;
				dot = d1 > d2 ? d1 : d2;
			}
			if (dot <= bestDot)
				continue;
			Trace tr;
			f32 z[3] = {0, 0, 0};
			CM_BoxTrace(&tr, eye, np, z, z, CONTENTS_SOLID | CONTENTS_WINDOW);
			if (tr.fraction < 0.99f && tr.ent != (s32)i)
				continue;
			bestDot = dot;
			best = e;
		}
	}
	if (!best)
		return;
	fire_output(best, "onplayeruse", 0);
	switch (best->cls) {
	case EC_FUNC_BUTTON:
		if (!(best->spawnflags & 256) || (best->spawnflags & 1024))
			press_button(best, 0, BTN_USE);
		break;
	case EC_PROP_DOOR:
	case EC_FUNC_DOOR:
	case EC_FUNC_DOOR_ROTATING:
		/* Source's rules: brush doors need "Use Opens"; prop doors can be set to ignore use; an open door
		 * only closes on use if it's set to ("Use Closes" / brush "Toggle"); a door blocked while
		 * closing opens again */
		if (best->cls != EC_PROP_DOOR && !(best->spawnflags & 256))
			break;
		if (best->cls == EC_PROP_DOOR && (best->spawnflags & 32768))
			break;
		if (best->locked) {
			door_sound(best, 2);
			fire_output(best, "onlockeduse", 0);
			break;
		}
		if (best->moveState == MV_CLOSING && best->blocked) {
			mover_open(best, 0);
			break;
		}
		if (best->moveState == MV_CLOSED) {
			if (best->cls == EC_PROP_DOOR)
				prop_door_direction(best);
			mover_open(best, 0);
		} else if (best->moveState == MV_OPEN &&
		           ((best->cls == EC_PROP_DOOR && (best->spawnflags & 8192)) ||
		            (best->cls != EC_PROP_DOOR && (best->spawnflags & 32)))) {
			mover_close(best, 0);
		}
		break;
	case EC_PROP:
		Phys_TryPickup((s32)(best - ents));
		break;
	case EC_VEHICLE:
		Veh_TryEnter((s32)(best - ents));
		break;
	case EC_NPC: {
		/* the response system's TLK_USE: a citizen says something back */
		Npc *n = Npc_OfEntity((s32)(best - ents));
		if (n && !n->dead && n->script < 0 && !n->inAScript && !Scene_NpcBusy(n))
			npc_say(best, "_use");
		break;
	}
	default:
		break;
	}
	run_events();
}

/* one of the lines listed in an entity key (sound indices), not the same one twice in a row */
static void npc_say(Entity *e, const char *key)
{
	const char *v = kv(e, key);
	Npc *n = Npc_OfEntity((s32)(e - ents));
	if (!v || !n)
		return;
	int ids[16], c = 0;
	const char *s = v;
	while (*s && c < 16) {
		char *end;
		long x = strtol(s, &end, 10);
		if (end == s)
			break;
		ids[c++] = (int)x;
		s = end;
	}
	if (!c)
		return;
	int k = rand() % c;
	if (c > 1 && ids[k] == n->speakSound)
		k = (k + 1) % c;
	Scene_NpcSay(n, (s16)ids[k]);
}

/* characters the story needs alive: the PC game protects them (or ends the game); here they shrug it off */
static int protected_npc(const Entity *e)
{
	static const char *const names[] = {"npc_alyx", "npc_barney", "npc_eli", "npc_kleiner", "npc_gman", "npc_breen",
	                                    "npc_mossman", "npc_monk", "npc_dog", "npc_vortigaunt", "npc_magnusson",
	                                    "npc_fisherman", "npc_furniture", "npc_citizen"};
	/* monster_generic not solid (scenery like the Combine's crushing walls): Source gives it DAMAGE_NO */
	if (!strcmp(e->classname, "monster_generic") && (e->spawnflags & 0x10000))
		return 1;
	/* cycler: 80000 health, "no cycler should die" (CCycler::Spawn) */
	if (!strncmp(e->classname, "cycler", 6))
		return 1;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(e->classname, names[i])) {
			/* plain citizens can be hurt; ones the map's scripts name are story characters */
			if (i == sizeof(names) / sizeof(names[0]) - 1)
				return e->name && *e->name;
			return 1;
		}
	return 0;
}

/* what a breaking thing leaves behind: its pieces and its sound (props by their model, brushes by material) */
static void break_effects(Entity *e, const f32 dir[3])
{
	f32 vel[3] = {0, 0, 0};
	if (dir)
		for (int k = 0; k < 3; k++)
			vel[k] = dir[k] * 120.0f;
	f32 mn[3], mx[3], c[3];
	world_bounds(e, mn, mx);
	for (int k = 0; k < 3; k++)
		c[k] = (mn[k] + mx[k]) * 0.5f;
	int material = -1;
	if (e->mdl >= 0 && e->bmodel <= 0) {
		Gibs_BreakModel(e->mdl, e->origin, e->R, vel, e->tint);
		const char *m = kv(e, "_mat");
		material = m ? -atoi(m) : -1;
	} else {
		static const s8 byMat[9] = {-2, -1, -3, 0, -4, 0, -3, -2, -4};
		int m = !strcmp(e->classname, "func_breakable_surf") ? 0 : (int)kvf(e, "material", 1);
		material = (m >= 0 && m < 9) ? byMat[m] : -1;
		if (material)
			Gibs_BreakBox(material, mn, mx, vel, e->tint);
	}
	if (material)
		Weap_BreakSound(material, c);
}

static void world_center(const Entity *e, f32 c[3])
{
	f32 mn[3], mx[3];
	world_bounds(e, mn, mx);
	for (int k = 0; k < 3; k++)
		c[k] = (mn[k] + mx[k]) * 0.5f;
}

static int breakable_health(Entity *e)
{
	if (!e->armed) {
		e->armed = 1;
		e->value = kvf(e, "_hp", 0);          /* the model's prop_data */
		if (kvf(e, "health", 0) > 0)
			e->value = kvf(e, "health", 0); /* the map's override */
		if (!strcmp(e->classname, "func_breakable_surf") && e->value <= 0)
			e->value = 1; /* glass panes break from any hit */
	}
	return e->value > 0;
}

int Ent_IsProp(s32 idx)
{
	return idx > 0 && (u32)idx < numEnts && (ents[idx].cls == EC_PROP || ents[idx].cls == EC_FUNC_BREAKABLE) && alive(&ents[idx]);
}

void Ent_Damage(s32 idx, f32 damage, const f32 dir[3], const f32 at[3])
{
	if (idx <= 0 || (u32)idx >= numEnts)
		return;
	Entity *e = &ents[idx];
	if (!alive(e) || damage <= 0)
		return;
	if (dir && at && Phys_IsBody(idx)) {
		f32 imp[3] = {dir[0] * damage * 300.0f, dir[1] * damage * 300.0f, dir[2] * damage * 300.0f};
		Phys_Impulse(idx, at, imp);
	}
	switch (e->cls) {
	case EC_NPC: {
		Npc *n = Npc_OfEntity(idx);
		if (!n || n->dead || protected_npc(e))
			break;
		f32 before = n->health;
		n->health -= damage;
		Npc_Alert(n, 8.0f); /* (hurt: it turns to look for who did it) */
		fire_output(e, "ondamaged", 0);
		if (before > 50 && n->health <= 50)
			fire_output(e, "onhalfhealth", 0);
		if (n->health <= 0) {
			fire_output(e, "ondeath", 0);
			if (dir) { /* the ragdoll flies off with the hit (Source's damage force) */
				f32 k = damage * 12.0f > 400.0f ? 400.0f : damage * 12.0f;
				for (int a = 0; a < 3; a++)
					n->deathPush[a] = dir[a] * k;
			}
			if (!Npc_Kill(n))
				e->flags |= EF_KILLED;
			e->flags &= ~EF_SOLID;
		}
		break;
	}
	case EC_FUNC_BREAKABLE:
	case EC_PROP:
		if (e->cls == EC_FUNC_BREAKABLE && (e->spawnflags & 1))
			break; /* "Only break on trigger" */
		if (!breakable_health(e))
			break;
		fire_output(e, "onhealthchanged", 0);
		e->value -= damage;
		if (e->value <= 0) {
			fire_output(e, "onbreak", 0);
			e->flags |= EF_KILLED;
			break_effects(e, dir);
			f32 c[3];
			world_center(e, c);
			Weap_Broke(idx, c);
			const char *boom = kv(e, "_boom");
			if (boom) {
				f32 dmg = 0, rad = 0;
				sscanf(boom, "%f %f", &dmg, &rad);
				Weap_Effect(WFX_EXPLOSION, c);
				Ent_RadiusDamage(c, dmg, rad);
			}
		}
		break;
	default:
		break;
	}
	run_events();
}

void Ent_RadiusDamage(const f32 at[3], f32 damage, f32 radius)
{
	Phys_Explosion(at, radius, damage * 30.0f);
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (!alive(e) || (e->cls != EC_NPC && e->cls != EC_PROP && e->cls != EC_FUNC_BREAKABLE))
			continue;
		f32 mn[3], mx[3], c[3];
		world_bounds(e, mn, mx);
		for (int k = 0; k < 3; k++)
			c[k] = (mn[k] + mx[k]) * 0.5f;
		if (e->cls == EC_NPC)
			c[2] = e->origin[2] + 36;
		f32 d[3] = {c[0] - at[0], c[1] - at[1], c[2] - at[2]};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (l > radius)
			continue;
		Trace tr;
		f32 z[3] = {0, 0, 0};
		CM_SetIgnoreEnt((s32)i);
		CM_BoxTrace(&tr, at, c, z, z, CONTENTS_SOLID);
		CM_SetIgnoreEnt(-1);
		if (tr.fraction < 0.95f && tr.ent != (s32)i)
			continue; /* behind a wall */
		f32 dir[3] = {d[0] / (l > 1 ? l : 1), d[1] / (l > 1 ? l : 1), d[2] / (l > 1 ? l : 1)};
		Ent_Damage((s32)i, damage * (1 - l / radius), dir, c);
	}
	f32 eye[3] = {player->origin[0], player->origin[1], player->origin[2] + 36};
	f32 d[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
	f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (l < radius && (cbs.hurtPlayer || cbs.hurtPlayerFrom)) {
		Trace tr;
		f32 z[3] = {0, 0, 0};
		CM_BoxTrace(&tr, at, eye, z, z, CONTENTS_SOLID);
		if (tr.fraction > 0.95f) {
			if (cbs.hurtPlayerFrom)
				cbs.hurtPlayerFrom(damage * (1 - l / radius) * 0.5f, 64 /* DMG_BLAST */, at);
			else
				cbs.hurtPlayer(damage * (1 - l / radius) * 0.5f);
			Ent_ShakeAdd(damage * 0.1f * (1 - l / radius), 1.0f);
			for (int k = 0; k < 3; k++)
				player->velocity[k] += d[k] / (l > 1 ? l : 1) * damage * 3.0f * (1 - l / radius);
		}
	}
}

void Ent_Bounds(s32 ent, f32 mins[3], f32 maxs[3]) { world_bounds(&ents[ent], mins, maxs); }
int Ent_Protected(s32 ent) { return protected_npc(&ents[ent]); }

/* the wall chargers and ammo crates use works on (held use): CBasePlayer::FindUseEntity for the things
 * that take continuous use. Only those count (a trigger volume around a charger isn't usable), the view's
 * line first, then the nearest one within ~37 degrees of it that the eye can see */
static int charge_target(const Entity *e)
{
	return !strcmp(e->classname, "item_healthcharger") || !strcmp(e->classname, "item_suitcharger") ||
	       !strcmp(e->classname, "item_ammo_crate") ||
	       (e->cls == EC_MOMENTARY && (e->spawnflags & 1024)); /* SF_BUTTON_USE_ACTIVATES */
}
s32 Ent_UseTarget(const f32 eye[3], const f32 fwd[3], f32 range)
{
	s32 best = -1;
	f32 wall = range;
	{
		Trace tr;
		f32 end[3] = {eye[0] + fwd[0] * range, eye[1] + fwd[1] * range, eye[2] + fwd[2] * range};
		f32 z[3] = {0, 0, 0};
		CM_BoxTrace(&tr, eye, end, z, z, CONTENTS_SOLID | CONTENTS_WINDOW);
		if (tr.fraction < 1)
			wall = tr.fraction * range + 8.0f; /* (a charger sits in the wall) */
	}
	f32 bestT = wall;
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (!alive(e) || !charge_target(e))
			continue;
		f32 mn[3], mx[3], t;
		world_bounds(e, mn, mx);
		if (ray_box(eye, fwd, range, mn, mx, &t) && t < bestT) {
			bestT = t;
			best = (s32)i;
		}
	}
	if (best > 0)
		return best;
	f32 bestDot = 0.8f;
	for (u32 i = 1; i < numEnts; i++) {
		Entity *e = &ents[i];
		if (!alive(e) || !charge_target(e))
			continue;
		f32 mn[3], mx[3], np[3], d[3];
		world_bounds(e, mn, mx);
		for (int k = 0; k < 3; k++)
			np[k] = eye[k] < mn[k] ? mn[k] : eye[k] > mx[k] ? mx[k] : eye[k];
		for (int k = 0; k < 3; k++)
			d[k] = np[k] - eye[k];
		f32 dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dl > range)
			continue;
		f32 c[3] = {(mn[0] + mx[0]) * 0.5f - eye[0], (mn[1] + mx[1]) * 0.5f - eye[1], (mn[2] + mx[2]) * 0.5f - eye[2]};
		f32 cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (cl < 1e-3f)
			return (s32)i;
		f32 dot = (c[0] * fwd[0] + c[1] * fwd[1] + c[2] * fwd[2]) / cl;
		if (dot < bestDot)
			continue;
		Trace tr;
		f32 z[3] = {0, 0, 0}, tgt[3] = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
		CM_BoxTrace(&tr, eye, tgt, z, z, CONTENTS_SOLID | CONTENTS_WINDOW);
		if (tr.fraction * cl < cl - 12.0f)
			continue; /* (behind a wall) */
		bestDot = dot;
		best = (s32)i;
	}
	return best;
}

Entity *Ent_List(u32 *count)
{
	if (count)
		*count = numEnts;
	return ents;
}

/* the yaw a prop door would open to, opened by someone at from (tests) */
f32 Ent_DoorOpenYaw(s32 idx, const f32 from[3])
{
	Entity *e = &ents[idx];
	f32 save = e->ang2[1];
	u8 st = e->moveState;
	e->moveState = MV_CLOSED;
	prop_door_direction_from(e, from, from != NULL);
	f32 y = e->ang2[1];
	e->ang2[1] = save;
	e->moveState = st;
	return y;
}

void Ent_WorldBounds(s32 idx, f32 mn[3], f32 mx[3])
{
	if (idx > 0 && (u32)idx < numEnts)
		world_bounds(&ents[idx], mn, mx);
}
