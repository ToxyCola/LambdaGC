/*
 * hl2gc: the rest of Half-Life 2's working objects, after their Source code:
 *   env_headcrabcanister (env_headcrabcanister.cpp): fired in, it lands, blows, opens and lets its headcrabs out
 *   prop_thumper (prop_thumper.cpp): thumps, raising dust, and keeps antlions out of its radius
 *   npc_sniper (proto_sniper.cpp): unseen; paints what it hates with its laser, then shoots
 *   npc_launcher (npc_launcher.cpp): launches homing missiles at its enemy
 *   npc_vehicledriver / npc_apcdriver (npc_vehicledriver.cpp, npc_apcdriver.cpp): drive their vehicle along
 *   path_tracks; the APC driver also fires the APC's gun and rockets; prop_vehicle_apc blows up when destroyed
 */
#include "objects.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "collide.h"
#include "ent.h"
#include "envents.h"
#include "npc.h"
#include "relations.h"
#include "weapons.h"

enum { OK_CANISTER = 1, OK_THUMPER, OK_SNIPER, OK_LAUNCHER, OK_DRIVER, OK_APCDRIVER, OK_APC };

typedef struct {
	s32 ent;
	u8 kind, state, on, flag;
	f32 t0, next, next2;
	f32 a[3], b[3];  /* canister: start, impact; sniper: laser end */
	s32 target, path, vehicle;
	int count;
	f32 speed, speedMin, speedMax;
} Obj;

#define MAX_OBJS 96
static Obj ob[MAX_OBJS];
static int numOb;

typedef struct {
	f32 pos[3], vel[3], until, damage, radius, homing, speed, start;
	s32 target, owner;
	u8 on;
} Missile;
#define MAX_MISSILES 16
static Missile ms[MAX_MISSILES];

static f32 frand(void) { return (f32)(rand() & 0xFFFF) / 65535.0f; }
static Entity *E(s32 i) { return Ent_List(NULL) + i; }
static f32 kvf(s32 ent, const char *k, f32 def)
{
	const char *v = Ent_KV(ent, k);
	return v && *v ? (f32)atof(v) : def;
}
static const char *kvs(s32 ent, const char *k)
{
	const char *v = Ent_KV(ent, k);
	return v ? v : "";
}
static s32 find(const char *n) { return n && *n ? Ent_FindActor(n) : -1; }
static void snd(s32 ent, int slot, const f32 at[3], f32 vol)
{
	const Pack *P = Ent_Pack();
	Entity *e = E(ent);
	if (P && slot >= 0 && slot < 3 && e->esnd[slot] >= 0 && (u32)e->esnd[slot] < P->numSounds)
		Audio_Play(&P->sounds[e->esnd[slot]], at ? at : e->origin, vol, SNDLVL_DEFAULT);
}
static int alive_ent(s32 x)
{
	if (x == 0)
		return 1;
	if (x < 0 || (E(x)->flags & (EF_KILLED | EF_DISABLED)))
		return 0;
	Npc *n = Npc_OfEntity(x);
	return !n || !n->dead;
}
static void center(s32 x, f32 out[3])
{
	const f32 *o = x == 0 ? Ent_Origin(0) : E(x)->origin;
	out[0] = o[0];
	out[1] = o[1];
	out[2] = o[2] + (x == 0 ? 40 : 32);
}
static f32 dist(const f32 a[3], const f32 b[3])
{
	f32 d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
	return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
static int clear_line(const f32 a[3], const f32 b[3], s32 ignore, s32 target)
{
	Trace tr;
	CM_SetIgnoreEnt(ignore);
	CM_TraceWorld(&tr, a, b, MASK_SHOT);
	CM_SetIgnoreEnt(-1);
	return tr.fraction >= 0.98f || (target > 0 && tr.ent == target);
}
/* what this one hates, nearest first, within range and in sight from 'from' */
static s32 enemy_of(s32 self, const f32 from[3], f32 range, s32 ignore)
{
	s32 best = -1;
	f32 bd = range;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 0; i < n; i++) {
		s32 x = (s32)i;
		if (x && (ents[i].npcModel < 0 || x == self || !alive_ent(x)))
			continue;
		if (x == 0 && Ent_NoTarget())
			continue;
		if (Rel_Disposition(self, x) != REL_HATE)
			continue;
		f32 c[3];
		center(x, c);
		f32 d = dist(c, from);
		if (d >= bd || !clear_line(from, c, ignore, x))
			continue;
		bd = d;
		best = x;
	}
	return best;
}

/* ---------------------------------------------------------------- missiles (launcher, APC) */

static void fire_missile(s32 owner, const f32 from[3], const f32 dir[3], s32 target, f32 speed, f32 damage,
                         f32 radius, f32 homing)
{
	for (int i = 0; i < MAX_MISSILES; i++) {
		Missile *m = &ms[i];
		if (m->on)
			continue;
		memset(m, 0, sizeof(*m));
		m->on = 1;
		memcpy(m->pos, from, 12);
		for (int k = 0; k < 3; k++)
			m->vel[k] = dir[k] * speed;
		m->speed = speed;
		m->damage = damage;
		m->radius = radius;
		m->homing = homing;
		m->target = target;
		m->owner = owner;
		m->start = Ent_Now();
		m->until = Ent_Now() + 10;
		return;
	}
}

static void missiles_tick(f32 dt, f32 now)
{
	for (int i = 0; i < MAX_MISSILES; i++) {
		Missile *m = &ms[i];
		if (!m->on)
			continue;
		if (m->target >= 0 && alive_ent(m->target) && m->homing > 0) {
			f32 c[3];
			center(m->target, c);
			f32 d[3] = {c[0] - m->pos[0], c[1] - m->pos[1], c[2] - m->pos[2]};
			f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
			f32 v = sqrtf(m->vel[0] * m->vel[0] + m->vel[1] * m->vel[1] + m->vel[2] * m->vel[2]) + 1e-3f;
			f32 turn = m->homing * dt * 0.5f; /* (HomingStrength) */
			turn = turn > 1 ? 1 : turn;
			for (int k = 0; k < 3; k++)
				m->vel[k] = (m->vel[k] / v * (1 - turn) + d[k] / l * turn) * m->speed;
		}
		f32 to[3] = {m->pos[0] + m->vel[0] * dt, m->pos[1] + m->vel[1] * dt, m->pos[2] + m->vel[2] * dt};
		Trace tr;
		CM_SetIgnoreEnt(m->owner);
		CM_TraceWorld(&tr, m->pos, to, MASK_SHOT);
		CM_SetIgnoreEnt(-1);
		memcpy(m->pos, tr.endpos, 12);
		const f32 *pp = Ent_Origin(0);
		f32 pc[3] = {pp[0], pp[1], pp[2] + 36};
		int near = dist(pc, m->pos) < 40;
		if (m->target > 0) {
			f32 c[3];
			center(m->target, c);
			near |= dist(c, m->pos) < 40;
		}
		if (tr.fraction < 1 || near || now >= m->until) {
			Weap_Explosion(m->pos, m->radius / 200.0f, 0);
			Ent_RadiusDamage(m->pos, m->damage, m->radius);
			m->on = 0;
		}
	}
}

int Objects_NumMissiles(void) { return MAX_MISSILES; }
int Objects_Missile(int i, f32 pos[3])
{
	if (i < 0 || i >= MAX_MISSILES || !ms[i].on)
		return 0;
	memcpy(pos, ms[i].pos, 12);
	return 1;
}

/* ---------------------------------------------------------------- headcrab canister */

#define HC_IMPACTED 0x1000
#define HC_LAND_AT_ORIGIN 0x2000
#define HC_WAIT_OPEN 0x4000
#define HC_WAIT_SPAWN 0x8000
#define HC_NO_IMPACT_SOUND 0x1
#define HC_NO_LAUNCH_SOUND 0x2
#define HC_NO_SHAKE 0x20000
#define HC_REMOVE 0x40000
#define HC_NO_EFFECTS 0x80000
enum { HC_WAITING, HC_FLYING, HC_LANDED, HC_OPENING, HC_OPEN };

static void can_landed(Obj *o)
{
	Entity *e = E(o->ent);
	o->state = HC_LANDED;
	o->t0 = Ent_Now();
	e->flags &= ~EF_DISABLED;
	if (!o->flag) /* (SF_START_IMPACTED: it was always there) */
		return;
}

static void can_open(Obj *o)
{
	Npc *n = Npc_OfEntity(o->ent);
	o->state = HC_OPENING;
	f32 d = n ? Npc_PlaySequence(n, "open", 0) : -1;
	o->next = Ent_Now() + (d > 0 ? d : 1.0f);
	snd(o->ent, 2, NULL, 1.0f);
}

static void can_tick(Obj *o, f32 now)
{
	Entity *e = E(o->ent);
	u32 sf = e->spawnflags;
	Npc *n = Npc_OfEntity(o->ent);
	switch (o->state) {
	case HC_FLYING: {
		f32 ft = kvf(o->ent, "flighttime", 5);
		f32 s = (now - o->t0) / (ft > 0.05f ? ft : 0.05f);
		if (s < 1) {
			for (int k = 0; k < 3; k++)
				e->origin[k] = o->a[k] + (o->b[k] - o->a[k]) * s;
			if (n)
				n->yaw = n->idealYaw = e->angles[1];
			return;
		}
		memcpy(e->origin, o->b, 12);
		/* Detonate / Landed: the blast, the shake, OnImpacted */
		if (!(sf & HC_NO_EFFECTS))
			Weap_Explosion(e->origin, 1.0f, EXPL_NOSOUND);
		if (!(sf & HC_NO_IMPACT_SOUND))
			snd(o->ent, 1, NULL, 1.0f);
		if (!(sf & HC_NO_SHAKE)) {
			const f32 *pp = Ent_Origin(0);
			f32 d = dist(pp, e->origin);
			if (d < 1000) /* sk_env_headcrabcanister_shake_radius */
				Ent_ShakeAdd(50 * (1 - d / 1000), 1.0f);
		}
		f32 dmg = kvf(o->ent, "damage", 150), rad = kvf(o->ent, "damageradius", 750);
		if (dmg > 0 && rad > 0)
			Ent_RadiusDamage(e->origin, dmg, rad);
		Ent_FireOutput(o->ent, "onimpacted", o->ent);
		if (sf & HC_REMOVE) {
			e->flags |= EF_KILLED;
			o->on = 0;
			return;
		}
		o->flag = 1;
		can_landed(o);
		return;
	}
	case HC_LANDED:
		if (!(sf & HC_WAIT_OPEN) && now - o->t0 >= 1.0f)
			can_open(o);
		return;
	case HC_OPENING:
		if (now >= o->next) {
			o->state = HC_OPEN;
			if (n)
				Npc_PlaySequence(n, "idle_open", 1);
			Ent_FireOutput(o->ent, "onopened", o->ent);
			if (!(sf & HC_IMPACTED) && !(sf & HC_WAIT_SPAWN))
				o->next2 = now + 3.0f; /* StartSpawningHeadcrabs( 3.0f ) */
		}
		return;
	case HC_OPEN:
		/* the headcrabs come out one at a time */
		if (o->next2 > 0 && now >= o->next2 && o->count > 0) {
			const char *crab = Ent_KV(o->ent, "_crab");
			int ht = (int)kvf(o->ent, "headcrabtype", 0);
			static const char *cls[3] = {"npc_headcrab", "npc_headcrab_fast", "npc_headcrab_poison"};
			f32 pos[3], R[3][3];
			if (!n || !Npc_Attachment(n, "headcrab", pos, R)) {
				memcpy(pos, e->origin, 12);
				pos[2] += 16;
			}
			f32 ang[3] = {0, e->angles[1] + (frand() - 0.5f) * 90, 0};
			s32 nc = -2;
			if (crab && *crab)
				nc = Ent_Clone(o->ent, cls[ht < 0 ? 0 : ht > 2 ? 2 : ht], pos, ang, NULL, (s32)atoi(crab));
			(void)nc;
			o->count--;
			o->next2 = now + 0.5f + frand(); /* (Source: a random wait between them) */
		}
		return;
	}
}

static int can_input(Obj *o, const char *in)
{
	Entity *e = E(o->ent);
	f32 now = Ent_Now();
	if (!strcasecmp(in, "firecanister")) {
		if (o->on == 2)
			return 1;
		o->on = 2; /* (launched) */
		if (e->spawnflags & HC_IMPACTED) {
			o->next2 = now + 0.01f;
			return 1;
		}
		if (!(e->spawnflags & HC_NO_LAUNCH_SOUND))
			snd(o->ent, 0, NULL, 1.0f);
		/* from its launch position, else from far back along its flight line, down onto the impact point */
		s32 lp = find(kvs(o->ent, "launchpositionname"));
		f32 ft = kvf(o->ent, "flighttime", 5), sp = kvf(o->ent, "flightspeed", 3000);
		if (lp > 0)
			memcpy(o->a, E(lp)->origin, 12);
		else
			for (int k = 0; k < 3; k++)
				o->a[k] = o->b[k] + e->R[k][0] * ft * sp;
		memcpy(e->origin, o->a, 12);
		e->flags &= ~EF_DISABLED;
		o->state = HC_FLYING;
		o->t0 = now;
		Ent_FireOutput(o->ent, "onlaunched", o->ent);
	} else if (!strcasecmp(in, "opencanister")) {
		if (o->state == HC_LANDED && (e->spawnflags & HC_WAIT_OPEN))
			can_open(o);
	} else if (!strcasecmp(in, "spawnheadcrabs")) {
		if (o->state == HC_OPEN && (e->spawnflags & HC_WAIT_SPAWN))
			o->next2 = now + 0.01f;
	} else if (!strcasecmp(in, "stopsmoke")) {
		;
	} else
		return 0;
	return 1;
}

static void can_spawn(Obj *o)
{
	Entity *e = E(o->ent);
	o->count = (int)kvf(o->ent, "headcrabcount", 6);
	if (e->spawnflags & HC_IMPACTED) {
		memcpy(o->b, e->origin, 12);
		o->flag = 0;
		can_landed(o);
		return;
	}
	/* where it will come down: along its back from where it's placed, unless it lands right there */
	if (*kvs(o->ent, "launchpositionname") || (e->spawnflags & HC_LAND_AT_ORIGIN)) {
		memcpy(o->b, e->origin, 12);
	} else {
		f32 end[3] = {e->origin[0] - e->R[0][0] * 10000, e->origin[1] - e->R[1][0] * 10000, e->origin[2] - e->R[2][0] * 10000};
		Trace tr;
		CM_TraceWorld(&tr, e->origin, end, MASK_PLAYERSOLID);
		memcpy(o->b, tr.endpos, 12);
	}
	o->state = HC_WAITING;
	e->flags |= EF_DISABLED; /* (not in the world until it's fired) */
}

/* ---------------------------------------------------------------- thumper */

static void thumper_tick(Obj *o, f32 now)
{
	Entity *e = E(o->ent);
	if (!o->on || now < o->next)
		return;
	o->next = now + 1.5f; /* THUMPER_SOUND_DURATION: one thump each cycle of its hammer */
	f32 r = kvf(o->ent, "effectradius", 1000);
	snd(o->ent, 0, NULL, 1.0f);
	f32 sc = strstr(kvs(o->ent, "dustscale"), "Small") ? 0.5f : 1.0f;
	Weap_Dust(e->origin, NULL, 3.0f * sc, 0x9C8C78, 200, 16);
	const f32 *pp = Ent_Origin(0);
	f32 d = dist(pp, e->origin);
	if (d < r)
		Ent_ShakeAdd(4 * (1 - d / r), 0.5f);
	Ent_FireOutput(o->ent, "onthumped", o->ent);
	/* antlions inside the radius run out of it (CAI_BaseNPC: the thumper's sound repels them) */
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		if (strcmp(ents[i].classname, "npc_antlion") || !alive_ent((s32)i))
			continue;
		Npc *np = Npc_OfEntity((s32)i);
		if (!np || np->script >= 0)
			continue;
		f32 v[3] = {ents[i].origin[0] - e->origin[0], ents[i].origin[1] - e->origin[1], 0};
		f32 l = sqrtf(v[0] * v[0] + v[1] * v[1]);
		if (l >= r)
			continue;
		if (l < 1)
			v[0] = 1, l = 1;
		f32 out[3] = {e->origin[0] + v[0] / l * (r + 128), e->origin[1] + v[1] / l * (r + 128), ents[i].origin[2]};
		Npc_MoveTo(np, out, 1, 32);
	}
}

/* ---------------------------------------------------------------- sniper */

#define SN_HIDDEN (1 << 16)
#define SN_STARTDISABLED (1 << 19)
#define SN_FAST (1 << 20)
enum { SN_IDLE, SN_PAINT, SN_SWEEP };

static void sniper_tick(Obj *o, f32 dt, f32 now)
{
	Entity *e = E(o->ent);
	(void)dt;
	if (!o->on)
		return;
	f32 eye[3] = {e->origin[0], e->origin[1], e->origin[2]};
	s32 t = o->state == SN_SWEEP ? o->target : enemy_of(o->ent, eye, 8192, o->ent);
	if (t < 0 || (o->state != SN_SWEEP && !alive_ent(t))) {
		o->state = SN_IDLE;
		o->target = -1;
		return;
	}
	f32 c[3];
	if (o->state == SN_SWEEP)
		memcpy(c, E(t)->origin, 12);
	else
		center(t, c);
	if (t != o->target || o->state == SN_IDLE) {
		/* a new target: the laser goes onto them, and stays a while before the shot */
		o->target = t;
		if (o->state == SN_IDLE)
			o->state = SN_PAINT;
		f32 paint = kvf(o->ent, "paintinterval", 1.0f);
		if (e->spawnflags & SN_FAST)
			paint *= 0.5f;
		o->next = now + paint + kvf(o->ent, "paintintervalvariance", 0.75f) * frand();
	}
	/* the laser dot slides onto the target */
	for (int k = 0; k < 3; k++)
		o->b[k] += (c[k] - o->b[k]) * 0.25f;
	u8 bright = (u8)(kvf(o->ent, "beambrightness", 100) * 255 / 100 > 255 ? 255 : kvf(o->ent, "beambrightness", 100) * 255 / 100);
	EnvEnts_Tracer(o->ent, eye, o->b, 0xFF000000u | bright, 1.0f);
	if (now < o->next)
		return;
	/* the shot: sk_npc_dmg_sniper_round (100) to what it painted, Weapon's crack */
	if (o->state == SN_SWEEP) {
		o->state = SN_IDLE;
		if (E(t)->spawnflags & 1) /* SF_SNIPERTARGET_SHOOTME: a puff where it hits */
			Weap_Dust(c, NULL, 0.5f, 0x807060, 60, 6);
		Ent_FireOutput(t, "onshotfired", o->ent);
	} else if (clear_line(eye, c, o->ent, t)) {
		if (t == 0)
			Ent_HurtPlayer(100, DMG_BULLET, eye);
		else {
			f32 d[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
			f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
			for (int k = 0; k < 3; k++)
				d[k] /= l;
			Ent_Damage(t, 100, d, c);
		}
	}
	EnvEnts_Tracer(o->ent, eye, c, 0xFFFFE0FFu, 2.0f);
	snd(o->ent, 0, eye, 1.0f);
	Ent_FireOutput(o->ent, "onshotfired", o->ent);
	o->next = now + kvf(o->ent, "paintinterval", 1.0f) + 0.4f; /* SNIPER_SUBSEQUENT_PAINT_TIME */
}

static int sniper_input(Obj *o, const char *in, const char *param)
{
	if (!strcasecmp(in, "enablesniper"))
		o->on = 1;
	else if (!strcasecmp(in, "disablesniper")) {
		o->on = 0;
		o->state = SN_IDLE;
	} else if (!strcasecmp(in, "sweeptarget") || !strcasecmp(in, "sweeptargethighestpriority")) {
		s32 t = find(param);
		if (t > 0) {
			o->state = SN_SWEEP;
			o->target = t;
			o->next = Ent_Now() + 2.0f; /* SNIPER_PAINT_DECOY_TIME */
		}
	} else if (!strcasecmp(in, "stopsweeping")) {
		o->state = SN_IDLE;
	} else if (!strcasecmp(in, "sweepgrouprandomly") || !strcasecmp(in, "setdecoyradius") ||
	           !strcasecmp(in, "protecttarget") || !strcasecmp(in, "setpaintinterval") ||
	           !strcasecmp(in, "setpaintintervalvariance"))
		;
	else
		return 0;
	return 1;
}

/* ---------------------------------------------------------------- launcher */

static void launcher_tick(Obj *o, f32 now)
{
	Entity *e = E(o->ent);
	if (!o->on || now < o->next)
		return;
	f32 from[3] = {e->origin[0], e->origin[1], e->origin[2] + 16};
	s32 t = o->target >= 0 && alive_ent(o->target) ? o->target : enemy_of(o->ent, from, kvf(o->ent, "maxrange", 2048), o->ent);
	o->next = now + 0.5f;
	if (t < 0)
		return;
	f32 c[3];
	center(t, c);
	if (dist(c, from) < kvf(o->ent, "minrange", 100))
		return;
	/* the missile goes up out of the launcher, then homes in */
	f32 dir[3] = {e->R[0][0] * 0.3f, e->R[1][0] * 0.3f, 1};
	f32 l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	for (int k = 0; k < 3; k++)
		dir[k] /= l;
	fire_missile(o->ent, from, dir, t, kvf(o->ent, "homingspeed", 400), kvf(o->ent, "damage", 50),
	             kvf(o->ent, "damageradius", 200), kvf(o->ent, "homingstrength", 10));
	snd(o->ent, 0, NULL, 1.0f);
	Ent_FireOutput(o->ent, "onlaunch", o->ent);
	o->next = now + kvf(o->ent, "launchdelay", 8);
	if (o->flag) /* (FireOnce) */
		o->on = 0, o->flag = 0;
}

static int launcher_input(Obj *o, const char *in, const char *param)
{
	if (!strcasecmp(in, "turnon"))
		o->on = 1;
	else if (!strcasecmp(in, "turnoff"))
		o->on = 0;
	else if (!strcasecmp(in, "fireonce")) {
		o->on = 1;
		o->flag = 1;
		o->next = 0;
	} else if (!strcasecmp(in, "setenemyentity"))
		o->target = find(param);
	else if (!strcasecmp(in, "clearenemyentity"))
		o->target = -1;
	else if (!strcasecmp(in, "loscheckon") || !strcasecmp(in, "loscheckoff"))
		;
	else
		return 0;
	return 1;
}

/* ---------------------------------------------------------------- vehicle drivers, the APC */

#define APC_MAXSPEED 450.0f /* (apc_npc.txt's top speed, in units a second) */

static void driver_tick(Obj *o, f32 dt, f32 now)
{
	if (o->vehicle <= 0 || !alive_ent(o->vehicle))
		return;
	Entity *v = E(o->vehicle);
	Npc *vn = Npc_OfEntity(o->vehicle);
	/* drive along the path */
	if (o->on && o->path > 0 && !(E(o->path)->flags & EF_KILLED)) {
		f32 goal[3];
		memcpy(goal, E(o->path)->origin, 12);
		f32 d[3] = {goal[0] - v->origin[0], goal[1] - v->origin[1], 0};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1]);
		f32 want = o->speedMax * APC_MAXSPEED;
		f32 ps = kvf(o->path, "speed", 0); /* (a path_track's speed asks for that) */
		if (ps > 0 && ps < want)
			want = ps;
		if (want < o->speedMin * APC_MAXSPEED)
			want = o->speedMin * APC_MAXSPEED;
		o->speed += (want > o->speed ? 1 : -1) * 300 * dt;
		if (fabsf(o->speed - want) < 300 * dt)
			o->speed = want;
		f32 step = o->speed * dt;
		if (l <= step + 8) {
			memcpy(v->origin, goal, 12);
			Ent_FireOutput(o->path, "onpass", o->vehicle);
			const char *nx = kvs(o->path, "target");
			s32 nxt = find(nx);
			if (nxt > 0)
				o->path = nxt;
			else {
				o->on = 0;
				o->speed = 0;
			}
		} else {
			f32 yaw = atan2f(d[1], d[0]) * 180.0f / (f32)M_PI;
			v->origin[0] += d[0] / l * step;
			v->origin[1] += d[1] / l * step;
			v->origin[2] += (goal[2] - v->origin[2]) * (step / (l + 1e-3f));
			f32 dy = fmodf(yaw - v->angles[1] + 540.0f, 360.0f) - 180.0f;
			f32 tr = 90 * dt;
			v->angles[1] += dy > tr ? tr : dy < -tr ? -tr : dy;
			Ent_AngleMatrix(v->angles, v->R);
			if (vn)
				vn->yaw = vn->idealYaw = v->angles[1];
		}
	}
	if (o->kind != OK_APCDRIVER || !o->flag)
		return;
	/* the APC's guns: a burst from the machine gun (3 a round), a rocket every so often */
	f32 gun[3] = {v->origin[0], v->origin[1], v->origin[2] + 96};
	s32 t = enemy_of(o->ent, gun, 2048, o->vehicle);
	if (t < 0)
		return;
	f32 c[3];
	center(t, c);
	if (now >= o->next && !(o->state & 1)) {
		f32 d[3] = {c[0] - gun[0] + (frand() - 0.5f) * 48, c[1] - gun[1] + (frand() - 0.5f) * 48, c[2] - gun[2] + (frand() - 0.5f) * 32};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
		for (int k = 0; k < 3; k++)
			d[k] /= l;
		f32 end[3] = {gun[0] + d[0] * l * 1.5f, gun[1] + d[1] * l * 1.5f, gun[2] + d[2] * l * 1.5f};
		Trace tr;
		CM_SetIgnoreEnt(o->vehicle);
		CM_TraceWorld(&tr, gun, end, MASK_SHOT);
		CM_SetIgnoreEnt(-1);
		if (t == 0 && dist(tr.endpos, gun) + 24 >= l)
			Ent_HurtPlayer(3, DMG_BULLET, gun);
		else if (t > 0 && tr.ent == t)
			Ent_Damage(t, 3, d, tr.endpos);
		else if (tr.fraction < 1)
			Weap_BulletImpact(&tr, d);
		EnvEnts_Tracer(o->vehicle, gun, tr.endpos, 0x80C0FFFFu, 2.0f);
		Weap_Muzzle(gun, 0.8f);
		snd(o->vehicle, 1, gun, 1.0f);
		o->count++;
		o->next = now + (o->count % 10 ? 0.1f : 1.5f); /* (bursts of ten) */
	}
	if (now >= o->next2 && !(o->state & 2)) {
		f32 d[3] = {c[0] - gun[0], c[1] - gun[1], c[2] - gun[2] + 200};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
		for (int k = 0; k < 3; k++)
			d[k] /= l;
		fire_missile(o->vehicle, gun, d, t, 600, 25, 128, 8); /* (sk_apc_missile_damage) */
		snd(o->vehicle, 2, gun, 1.0f);
		o->next2 = now + 4 + frand() * 2;
	}
}

static int driver_input(Obj *o, const char *in, const char *param)
{
	if (!strcasecmp(in, "startforward") || !strcasecmp(in, "resume"))
		o->on = 1;
	else if (!strcasecmp(in, "stop")) {
		o->on = 0;
		o->speed = 0;
	} else if (!strcasecmp(in, "gotopathcorner")) {
		s32 p = find(param);
		if (p > 0) {
			o->path = p;
			o->on = 1;
		}
	} else if (!strcasecmp(in, "setdriversmaxspeed"))
		o->speedMax = (f32)atof(param);
	else if (!strcasecmp(in, "setdriversminspeed"))
		o->speedMin = (f32)atof(param);
	else if (!strcasecmp(in, "startfiring") || !strcasecmp(in, "enablefiring") || !strcasecmp(in, "enable"))
		o->flag = 1;
	else if (!strcasecmp(in, "stopfiring") || !strcasecmp(in, "disablefiring") || !strcasecmp(in, "disable"))
		o->flag = 0;
	else if (!strcasecmp(in, "firerocketat")) {
		s32 t = find(param);
		if (t >= 0 && o->vehicle > 0) {
			Entity *v = E(o->vehicle);
			f32 gun[3] = {v->origin[0], v->origin[1], v->origin[2] + 96}, c[3];
			center(t, c);
			f32 d[3] = {c[0] - gun[0], c[1] - gun[1], c[2] - gun[2]};
			f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
			for (int k = 0; k < 3; k++)
				d[k] /= l;
			fire_missile(o->vehicle, gun, d, t, 600, 25, 128, 8);
		}
	} else
		return 0;
	return 1;
}

static void apc_tick(Obj *o)
{
	Npc *n = Npc_OfEntity(o->ent);
	Entity *e = E(o->ent);
	if (!o->flag && n && n->dead) {
		/* destroyed: it blows up (CPropAPC::Event_Killed), OnDeath */
		o->flag = 1;
		Weap_Explosion(e->origin, 2.0f, 0);
		Ent_RadiusDamage(e->origin, 100, 300);
		Ent_FireOutput(o->ent, "ondeath", o->ent);
	}
}

/* ---------------------------------------------------------------- dispatch */

static int kind_of(const char *cn)
{
	if (!strcmp(cn, "env_headcrabcanister"))
		return OK_CANISTER;
	if (!strcmp(cn, "prop_thumper"))
		return OK_THUMPER;
	if (!strcmp(cn, "npc_sniper"))
		return OK_SNIPER;
	if (!strcmp(cn, "npc_launcher"))
		return OK_LAUNCHER;
	if (!strcmp(cn, "npc_vehicledriver"))
		return OK_DRIVER;
	if (!strcmp(cn, "npc_apcdriver"))
		return OK_APCDRIVER;
	if (!strcmp(cn, "prop_vehicle_apc"))
		return OK_APC;
	return 0;
}

void Objects_Spawn(void)
{
	numOb = 0;
	memset(ms, 0, sizeof(ms));
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n && numOb < MAX_OBJS; i++) {
		int k = kind_of(ents[i].classname);
		if (!k)
			continue;
		Obj *o = &ob[numOb++];
		memset(o, 0, sizeof(*o));
		o->ent = (s32)i;
		o->kind = (u8)k;
		o->target = -1;
		u32 sf = ents[i].spawnflags;
		switch (k) {
		case OK_CANISTER:
			can_spawn(o);
			break;
		case OK_THUMPER:
			o->on = 1;
			o->next = Ent_Now() + frand();
			break;
		case OK_SNIPER:
			o->on = !(sf & SN_STARTDISABLED);
			memcpy(o->b, ents[i].origin, 12);
			break;
		case OK_LAUNCHER:
			o->on = kvf((s32)i, "starton", 0) != 0;
			o->next = Ent_Now() + 1;
			break;
		case OK_DRIVER:
		case OK_APCDRIVER:
			o->vehicle = find(kvs((s32)i, "vehicle"));
			o->path = find(kvs((s32)i, "target"));
			o->speedMax = kvf((s32)i, "drivermaxspeed", 1);
			o->speedMin = kvf((s32)i, "driverminspeed", 0);
			o->flag = k == OK_APCDRIVER; /* (firing enabled: EnableFiring / DisableFiring) */
			/* SF_APCDRIVER_NO_ROCKET_ATTACK 0x10000, SF_APCDRIVER_NO_GUN_ATTACK 0x20000 */
			o->state = k == OK_APCDRIVER ? (u8)(((sf & 0x20000) ? 1 : 0) | ((sf & 0x10000) ? 2 : 0)) : 0;
			o->on = o->path > 0 && !(sf & 0x10000);
			break;
		}
	}
}

void Objects_Tick(f32 dt)
{
	f32 now = Ent_Now();
	for (int i = 0; i < numOb; i++) {
		Obj *o = &ob[i];
		Entity *e = E(o->ent);
		if (e->flags & EF_KILLED)
			continue;
		switch (o->kind) {
		case OK_CANISTER:
			can_tick(o, now);
			break;
		case OK_THUMPER:
			if (!(e->flags & EF_DISABLED))
				thumper_tick(o, now);
			break;
		case OK_SNIPER:
			sniper_tick(o, dt, now);
			break;
		case OK_LAUNCHER:
			launcher_tick(o, now);
			break;
		case OK_DRIVER:
		case OK_APCDRIVER:
			driver_tick(o, dt, now);
			break;
		case OK_APC:
			apc_tick(o);
			break;
		}
	}
	missiles_tick(dt, now);
}

int Objects_Input(s32 ent, const char *in, const char *param)
{
	for (int i = 0; i < numOb; i++) {
		Obj *o = &ob[i];
		if (o->ent != ent)
			continue;
		switch (o->kind) {
		case OK_CANISTER:
			return can_input(o, in);
		case OK_THUMPER:
			if (!strcasecmp(in, "enable"))
				o->on = 1;
			else if (!strcasecmp(in, "disable"))
				o->on = 0;
			else
				return 0;
			return 1;
		case OK_SNIPER:
			return sniper_input(o, in, param);
		case OK_LAUNCHER:
			return launcher_input(o, in, param);
		case OK_DRIVER:
		case OK_APCDRIVER:
			return driver_input(o, in, param);
		}
		return 0;
	}
	return 0;
}

int Objects_Probe(s32 ent, int *state, f32 pos[3])
{
	for (int i = 0; i < numOb; i++)
		if (ob[i].ent == ent) {
			*state = ob[i].state | (ob[i].on << 8);
			memcpy(pos, E(ent)->origin, 12);
			return ob[i].kind;
		}
	return 0;
}
