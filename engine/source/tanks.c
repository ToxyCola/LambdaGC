/*
 * hl2gc: mounted guns (func_tank, func_tankairboatgun, func_tankrocket, func_tankmortar; func_tank.cpp). A
 * func_tank is aimed by whoever mans it: the player (from inside its control_volume, the gun follows his
 * crosshair and the fire trigger shoots), an NPC standing at its npc_man_point (it shoots that NPC's enemies),
 * or by itself when active (SF_TANK_ACTIVE: what its flags say to target, or SetTargetEntity / Position).
 * Bunker guns turn their model's aim_yaw / aim_pitch (gun_yaw_pose_param); others turn the brush itself.
 */
#include "tanks.h"

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

#define SF_ACTIVE 0x1
#define SF_PLAYER 0x2
#define SF_HUMANS 0x4
#define SF_ALIENS 0x8
#define SF_LOS 0x10
#define SF_CANCONTROL 0x20
#define SF_NPC_CONTROLLABLE 0x400

enum { TK_GUN, TK_AIRBOAT, TK_ROCKET, TK_MORTAR };

typedef struct {
	s32 ent, model, vol, man, controller, targetEnt;
	u8 kind, active, hasTargetPos, findNpcs, ready, haveTarget;
	f32 targetPos[3];
	f32 yaw, pitch;                /* brush tanks: current aim relative to the rest angles */
	f32 rest[3];                   /* rest angles */
	f32 yawSign, pitchSign;
	f32 yawRate, yawRange, yawTol, pitchRate, pitchRange, pitchTol;
	f32 fireRate, damage, damagePlayer, spread, minRange, maxRange, persist;
	f32 nextAttack, lastSight;
	int ammo;
} Tank;

#define MAX_TANKS 32
static Tank tk[MAX_TANKS];
static int numTk;
static s32 playerTank = -1; /* the tank the player is on (index), -1 */
static f32 viewEye[3], viewFwd[3];

typedef struct {
	f32 pos[3], vel[3], at;
	f32 damage, radius;
	u8 on, mortar;
} Shell;
#define MAX_SHELLS 16
static Shell sh[MAX_SHELLS];

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
static s32 find(const char *name) { return name && *name ? Ent_FindActor(name) : -1; }
static f32 angle_norm(f32 a)
{
	a = fmodf(a + 180.0f, 360.0f);
	if (a < 0)
		a += 360.0f;
	return a - 180.0f;
}
static void snd(Tank *t, int slot, f32 vol)
{
	const Pack *P = Ent_Pack();
	Entity *e = E(t->ent);
	if (P && e->esnd[slot] >= 0 && (u32)e->esnd[slot] < P->numSounds)
		Audio_Play(&P->sounds[e->esnd[slot]], e->origin, vol, SNDLVL_DEFAULT);
}
static void fwd_angles(const f32 f[3], f32 *yaw, f32 *pitch)
{
	*yaw = atan2f(f[1], f[0]) * 180.0f / (f32)M_PI;
	*pitch = -atan2f(f[2], sqrtf(f[0] * f[0] + f[1] * f[1])) * 180.0f / (f32)M_PI;
}

/* where the shots come from and which way the barrel points */
static void barrel(Tank *t, f32 pos[3], f32 fwd[3])
{
	Npc *m = t->model > 0 ? Npc_OfEntity(t->model) : NULL;
	f32 R[3][3];
	const char *att = kvs(t->ent, "gun_barrel_attach");
	if (m && Npc_Attachment(m, *att ? att : "muzzle", pos, R)) {
		for (int k = 0; k < 3; k++)
			fwd[k] = R[k][0];
		return;
	}
	Entity *e = E(t->ent);
	f32 mn[3], mx[3];
	Ent_Bounds(t->ent, mn, mx);
	for (int k = 0; k < 3; k++) {
		pos[k] = (mn[k] + mx[k]) * 0.5f;
		fwd[k] = e->R[k][0];
	}
}

static void calibrate(Tank *t)
{
	t->yawSign = t->pitchSign = 1;
	Npc *m = t->model > 0 ? Npc_OfEntity(t->model) : NULL;
	if (!m)
		return;
	m->aimOn = 1;
	f32 p[3], f[3], y0, p0, y1, p1;
	m->aimYaw = m->aimPitch = 0;
	barrel(t, p, f);
	fwd_angles(f, &y0, &p0);
	m->aimYaw = 10;
	barrel(t, p, f);
	fwd_angles(f, &y1, &p1);
	t->yawSign = angle_norm(y1 - y0) >= 0 ? 1.0f : -1.0f;
	m->aimYaw = 0;
	m->aimPitch = 10;
	barrel(t, p, f);
	fwd_angles(f, &y1, &p1);
	t->pitchSign = p1 - p0 >= 0 ? 1.0f : -1.0f;
	m->aimPitch = 0;
}

void Tanks_Spawn(void)
{
	numTk = 0;
	playerTank = -1;
	memset(sh, 0, sizeof(sh));
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n && numTk < MAX_TANKS; i++) {
		const char *cn = ents[i].classname;
		if (strncmp(cn, "func_tank", 9) || !strcmp(cn, "func_tankcontrols") || !strcmp(cn, "func_tanktrain"))
			continue;
		Tank *t = &tk[numTk++];
		memset(t, 0, sizeof(*t));
		s32 e = (s32)i;
		t->ent = e;
		t->kind = !strcmp(cn, "func_tankairboatgun") ? TK_AIRBOAT : !strcmp(cn, "func_tankrocket") ? TK_ROCKET
		                                              : !strcmp(cn, "func_tankmortar") ? TK_MORTAR : TK_GUN;
		/* the bunker gun model is its parent, turned by its pose parameters */
		s32 par = find(kvs(e, "parentname"));
		t->model = (par > 0 && *kvs(e, "gun_yaw_pose_param") && Npc_OfEntity(par)) ? par : -1;
		t->vol = find(kvs(e, "control_volume"));
		t->man = find(kvs(e, "npc_man_point"));
		t->controller = -1;
		t->targetEnt = -1;
		t->active = (ents[i].spawnflags & SF_ACTIVE) != 0;
		t->findNpcs = (ents[i].spawnflags & SF_NPC_CONTROLLABLE) != 0;
		memcpy(t->rest, ents[i].angles, 12);
		t->yawRate = kvf(e, "yawrate", 30);
		t->yawRange = kvf(e, "yawrange", 180);
		t->yawTol = kvf(e, "yawtolerance", 15);
		t->pitchRate = kvf(e, "pitchrate", 0);
		t->pitchRange = kvf(e, "pitchrange", 0);
		t->pitchTol = kvf(e, "pitchtolerance", 5);
		t->fireRate = kvf(e, "firerate", 1);
		t->damage = kvf(e, "bullet_damage", 0);
		t->damagePlayer = kvf(e, "bullet_damage_vs_player", 0);
		static const f32 spreads[5] = {0, 0.025f, 0.05f, 0.1f, 0.25f};
		int si = (int)kvf(e, "firespread", 0);
		t->spread = spreads[si < 0 ? 0 : si > 4 ? 4 : si];
		t->minRange = kvf(e, "minrange", 0);
		t->maxRange = kvf(e, "maxrange", 0);
		t->persist = kvf(e, "persistence", 1);
		t->ammo = (int)kvf(e, "ammo_count", -1);
		if (t->ammo == 0)
			t->ammo = -1;
		calibrate(t);
	}
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
static int visible(Tank *t, const f32 from[3], s32 x, const f32 p[3])
{
	Trace tr;
	CM_SetIgnoreEnt(t->model > 0 ? t->model : t->ent);
	CM_TraceWorld(&tr, from, p, MASK_SHOT);
	CM_SetIgnoreEnt(-1);
	return tr.fraction >= 0.98f || tr.ent == x;
}

/* the target: the controller's enemies, or (unmanned) what its flags say */
static s32 find_target(Tank *t, const f32 from[3])
{
	u32 sf = E(t->ent)->spawnflags;
	f32 range = t->maxRange > 0 ? t->maxRange : 8192;
	s32 best = -1;
	f32 bd = range * range;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 0; i < n; i++) {
		s32 x = (s32)i;
		if (x && (ents[i].npcModel < 0 || x == t->controller || !alive_ent(x)))
			continue;
		if (x == 0 && Ent_NoTarget())
			continue;
		if (t->controller > 0) {
			if (Rel_Disposition(t->controller, x) != REL_HATE)
				continue;
		} else if (x == 0) {
			if (!(sf & SF_PLAYER))
				continue;
		} else {
			/* SF_TANK_HUMANS / ALIENS: (Combine and rebels / everything else, as Classify groups them) */
			int alien = !strncmp(ents[i].classname, "npc_antlion", 11) || !strncmp(ents[i].classname, "npc_zombie", 10) ||
			            !strncmp(ents[i].classname, "npc_headcrab", 12) || !strncmp(ents[i].classname, "npc_fastzombie", 14);
			if (!((sf & SF_ALIENS) && alien) && !((sf & SF_HUMANS) && !alien))
				continue;
		}
		f32 p[3];
		center(x, p);
		f32 d[3] = {p[0] - from[0], p[1] - from[1], p[2] - from[2]};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd >= bd || dd < t->minRange * t->minRange)
			continue;
		if (!visible(t, from, x, p))
			continue;
		best = x;
		bd = dd;
	}
	return best;
}

/* turn toward a world point, within its ranges, at its rates; returns 1 when on target (the tolerances) */
static int aim(Tank *t, const f32 goal[3], f32 dt)
{
	f32 from[3], f[3];
	barrel(t, from, f);
	f32 d[3] = {goal[0] - from[0], goal[1] - from[1], goal[2] - from[2]};
	f32 gy, gp, cy, cp;
	fwd_angles(d, &gy, &gp);
	fwd_angles(f, &cy, &cp);
	f32 dy = angle_norm(gy - cy), dp = gp - cp;
	f32 ys = t->yawRate * dt, ps = (t->pitchRate > 0 ? t->pitchRate : t->yawRate) * dt;
	f32 sy = dy > ys ? ys : dy < -ys ? -ys : dy;
	f32 sp = dp > ps ? ps : dp < -ps ? -ps : dp;
	Npc *m = t->model > 0 ? Npc_OfEntity(t->model) : NULL;
	if (m) {
		m->aimOn = 1;
		f32 ny = m->aimYaw + t->yawSign * sy, np = m->aimPitch + t->pitchSign * sp;
		if (t->yawRange < 180)
			ny = ny > t->yawRange ? t->yawRange : ny < -t->yawRange ? -t->yawRange : ny;
		f32 pr = t->pitchRange > 0 ? t->pitchRange : 60;
		np = np > pr ? pr : np < -pr ? -pr : np;
		m->aimYaw = ny;
		m->aimPitch = np;
	} else {
		/* a brush tank: turn the brush (its children go with it) */
		Entity *e = E(t->ent);
		t->yaw = angle_norm(t->yaw + sy);
		t->pitch += sp;
		if (t->yawRange < 180)
			t->yaw = t->yaw > t->yawRange ? t->yawRange : t->yaw < -t->yawRange ? -t->yawRange : t->yaw;
		f32 pr = t->pitchRange;
		t->pitch = t->pitch > pr ? pr : t->pitch < -pr ? -pr : t->pitch;
		e->angles[0] = t->rest[0] + t->pitch;
		e->angles[1] = t->rest[1] + t->yaw;
		Ent_AngleMatrix(e->angles, e->R);
	}
	return fabsf(dy) <= t->yawTol && fabsf(dp) <= t->pitchTol + 2;
}

static void launch_shell(Tank *t, const f32 from[3], const f32 dir[3], const f32 goal[3])
{
	for (int i = 0; i < MAX_SHELLS; i++) {
		Shell *s = &sh[i];
		if (s->on)
			continue;
		memset(s, 0, sizeof(*s));
		s->on = 1;
		if (t->kind == TK_MORTAR) {
			/* func_tankmortar: the shell comes down on the target after the warning time */
			s->mortar = 1;
			memcpy(s->pos, goal, 12);
			s->at = Ent_Now() + kvf(t->ent, "warningtime", 1) + kvf(t->ent, "firedelay", 0);
			s->damage = kvf(t->ent, "imagnitude", 100);
			s->radius = s->damage * 2.5f;
		} else {
			f32 sp = kvf(t->ent, "rocketspeed", 800);
			memcpy(s->pos, from, 12);
			for (int k = 0; k < 3; k++)
				s->vel[k] = dir[k] * sp;
			s->at = Ent_Now() + 8;
			s->damage = 100;
			s->radius = 160;
		}
		return;
	}
}

static void fire(Tank *t, const f32 from[3], const f32 dir0[3], const f32 goal[3], int byPlayer)
{
	Ent_FireOutput(t->ent, "onfire", byPlayer ? 0 : t->ent);
	if (t->kind == TK_ROCKET || t->kind == TK_MORTAR) {
		launch_shell(t, from, dir0, goal);
		snd(t, 0, 1.0f);
		return;
	}
	if (atoi(kvs(t->ent, "bullet")) == 0 && t->kind != TK_AIRBOAT && t->damage <= 0)
		return; /* (a tank that only aims: the prison's camera mounts) */
	f32 d[3];
	for (int k = 0; k < 3; k++)
		d[k] = dir0[k] + (frand() * 2 - 1) * t->spread;
	f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
	for (int k = 0; k < 3; k++)
		d[k] /= l;
	f32 end[3] = {from[0] + d[0] * 8192, from[1] + d[1] * 8192, from[2] + d[2] * 8192};
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_SetIgnoreEnt(t->model > 0 ? t->model : t->ent);
	CM_BoxTrace(&tr, from, end, z, z, MASK_SHOT);
	CM_SetIgnoreEnt(-1);
	f32 dmg = t->kind == TK_AIRBOAT ? t->damage / (t->fireRate > 1 ? t->fireRate : 1) : t->damage;
	/* the player (the trace doesn't hit him): the line against his box */
	if (!byPlayer) {
		const f32 *pp = Ent_Origin(0);
		f32 to[3] = {pp[0] - from[0], pp[1] - from[1], pp[2] + 36 - from[2]};
		f32 s = to[0] * d[0] + to[1] * d[1] + to[2] * d[2];
		if (s > 0 && s < tr.fraction * 8192) {
			f32 c[3] = {from[0] + d[0] * s - pp[0], from[1] + d[1] * s - pp[1], from[2] + d[2] * s - pp[2] - 36};
			if (fabsf(c[0]) < 16 && fabsf(c[1]) < 16 && fabsf(c[2]) < 36) {
				Ent_HurtPlayer(t->damagePlayer > 0 ? t->damagePlayer : dmg, DMG_BULLET, from);
				tr.fraction = s / 8192;
				for (int k = 0; k < 3; k++)
					tr.endpos[k] = from[k] + d[k] * s;
				tr.ent = -1;
			}
		}
	}
	if (tr.fraction < 1) {
		if (tr.ent > 0 && Npc_OfEntity(tr.ent))
			Ent_Damage(tr.ent, dmg, d, tr.endpos);
		else if (tr.ent != -1 || byPlayer)
			Weap_BulletImpact(&tr, d);
	}
	EnvEnts_Tracer(t->ent, from, tr.endpos, t->kind == TK_AIRBOAT ? 0x80C0FFFFu : 0xFFE0A0C0u, t->kind == TK_AIRBOAT ? 3 : 1.5f);
	Weap_Muzzle(from, 1.0f);
	snd(t, 0, 1.0f);
	if (t->ammo > 0 && --t->ammo == 0) {
		Ent_FireOutput(t->ent, "onammodepleted", t->ent);
		E(t->ent)->spawnflags &= ~SF_CANCONTROL;
		if (playerTank == (s32)(t - tk))
			Tanks_PlayerLeave();
	}
}

static void stop_control(Tank *t)
{
	if (t->controller == 0) {
		Ent_FireOutput(t->ent, "onlostplayercontroller", 0);
		playerTank = -1;
	}
	if (t->controller >= 0)
		Ent_FireOutput(t->ent, "onlostcontroller", t->controller);
	t->controller = -1;
}

static void start_control(Tank *t, s32 who)
{
	t->controller = who;
	Ent_FireOutput(t->ent, "ongotcontroller", who);
	if (who == 0) {
		Ent_FireOutput(t->ent, "ongotplayercontroller", 0);
		snd(t, 1, 1.0f);
	}
}

/* NPCs come to man it: the nearest one that isn't busy walks to its npc_man_point */
static void find_npc(Tank *t)
{
	if (t->man <= 0 || !t->findNpcs)
		return;
	const f32 *mp = E(t->man)->origin;
	u32 n;
	Entity *ents = Ent_List(&n);
	s32 best = -1;
	f32 bd = 600 * 600;
	for (u32 i = 1; i < n; i++) {
		Npc *np = ents[i].npcModel >= 0 ? Npc_OfEntity((s32)i) : NULL;
		if (!np || np->dead || np->prop || np->script >= 0 || np->locked || (ents[i].flags & (EF_KILLED | EF_DISABLED | EF_TEMPLATE)))
			continue;
		const char *cn = ents[i].classname;
		if (strcmp(cn, "npc_combine_s") && strcmp(cn, "npc_metropolice") && strcmp(cn, "npc_citizen") &&
		    strcmp(cn, "npc_alyx") && strcmp(cn, "npc_barney"))
			continue;
		int busy = 0;
		for (int j = 0; j < numTk; j++)
			if (tk[j].controller == (s32)i)
				busy = 1;
		if (busy)
			continue;
		f32 d[3] = {ents[i].origin[0] - mp[0], ents[i].origin[1] - mp[1], ents[i].origin[2] - mp[2]};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd < bd) {
			bd = dd;
			best = (s32)i;
		}
	}
	if (best < 0)
		return;
	Npc *np = Npc_OfEntity(best);
	if (bd < 40 * 40) {
		Npc_Stop(np);
		start_control(t, best);
	} else if (!np->moving)
		Npc_MoveTo(np, mp, 1, 16);
}

static void tank_tick(Tank *t, f32 dt, f32 now)
{
	Entity *e = E(t->ent);
	if (e->flags & (EF_KILLED | EF_DISABLED))
		return;
	f32 from[3], f[3];
	barrel(t, from, f);
	/* the controller: still there? */
	if (t->controller > 0) {
		Npc *np = Npc_OfEntity(t->controller);
		const f32 *mp = t->man > 0 ? E(t->man)->origin : from;
		const f32 *o = E(t->controller)->origin;
		f32 dd = (o[0] - mp[0]) * (o[0] - mp[0]) + (o[1] - mp[1]) * (o[1] - mp[1]);
		if (!np || np->dead || !alive_ent(t->controller) || dd > 64 * 64 || np->script >= 0)
			stop_control(t);
		else
			Npc_FacePoint(np, from);
	}
	if (t->controller == 0) {
		/* the player: the barrel goes where his crosshair is */
		f32 end[3] = {viewEye[0] + viewFwd[0] * 8192, viewEye[1] + viewFwd[1] * 8192, viewEye[2] + viewFwd[2] * 8192};
		Trace tr;
		CM_SetIgnoreEnt(t->model > 0 ? t->model : t->ent);
		CM_TraceWorld(&tr, viewEye, end, MASK_SHOT);
		CM_SetIgnoreEnt(-1);
		aim(t, tr.endpos, dt);
		/* off the gun when he leaves its controls */
		if (t->vol > 0) {
			f32 mn[3], mx[3];
			Ent_Bounds(t->vol, mn, mx);
			const f32 *p = Ent_Origin(0);
			if (p[0] < mn[0] - 16 || p[0] > mx[0] + 16 || p[1] < mn[1] - 16 || p[1] > mx[1] + 16 ||
			    p[2] + 36 < mn[2] || p[2] > mx[2])
				stop_control(t);
		}
		return;
	}
	if (t->controller < 0 && t->findNpcs)
		find_npc(t);
	if (t->controller < 0 && !t->active)
		return;
	if (!t->ready && now >= t->nextAttack) {
		Ent_FireOutput(t->ent, "onreadytofire", t->ent);
		t->ready = 1;
	}
	f32 goal[3];
	int have = 0;
	if (t->hasTargetPos) {
		memcpy(goal, t->targetPos, 12);
		have = 1;
	} else if (t->targetEnt >= 0 && alive_ent(t->targetEnt)) {
		center(t->targetEnt, goal);
		have = 1;
	} else {
		s32 x = find_target(t, from);
		if (x >= 0) {
			center(x, goal);
			have = 1;
			t->lastSight = now;
		} else if (t->haveTarget && now - t->lastSight > t->persist) {
			t->haveTarget = 0;
			Ent_FireOutput(t->ent, "onlosetarget", t->ent);
		}
	}
	if (!have)
		return;
	if (!t->haveTarget && !t->hasTargetPos) {
		t->haveTarget = 1;
		Ent_FireOutput(t->ent, "onaquiretarget", t->ent);
	}
	int on = aim(t, goal, dt);
	if (on && now >= t->nextAttack && t->fireRate > 0) {
		f32 d[3] = {goal[0] - from[0], goal[1] - from[1], goal[2] - from[2]};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
		for (int k = 0; k < 3; k++)
			d[k] = d[k] / l * 0.3f + f[k] * 0.7f; /* (mostly where the barrel is) */
		fire(t, from, d, goal, 0);
		t->nextAttack = now + 1.0f / t->fireRate;
	}
}

static void shells_tick(f32 dt, f32 now)
{
	for (int i = 0; i < MAX_SHELLS; i++) {
		Shell *s = &sh[i];
		if (!s->on)
			continue;
		int boom = now >= s->at;
		if (!s->mortar && !boom) {
			f32 to[3] = {s->pos[0] + s->vel[0] * dt, s->pos[1] + s->vel[1] * dt, s->pos[2] + s->vel[2] * dt};
			Trace tr;
			CM_TraceWorld(&tr, s->pos, to, MASK_SHOT);
			memcpy(s->pos, tr.endpos, 12);
			const f32 *pp = Ent_Origin(0);
			f32 dp[3] = {pp[0] - s->pos[0], pp[1] - s->pos[1], pp[2] + 36 - s->pos[2]};
			boom = tr.fraction < 1 || dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2] < 40 * 40;
		}
		if (boom) {
			Weap_Explosion(s->pos, s->radius / 200.0f, 0);
			Ent_RadiusDamage(s->pos, s->damage, s->radius);
			s->on = 0;
		}
	}
}

void Tanks_Tick(f32 dt)
{
	f32 now = Ent_Now();
	for (int i = 0; i < numTk; i++)
		tank_tick(&tk[i], dt, now);
	shells_tick(dt, now);
}

void Tanks_SetView(const f32 eye[3], const f32 fwd[3])
{
	memcpy(viewEye, eye, 12);
	memcpy(viewFwd, fwd, 12);
}

int Tanks_PlayerUse(const f32 eye[3], const f32 fwd[3])
{
	if (playerTank >= 0) {
		Tanks_PlayerLeave();
		return 1;
	}
	const f32 *p = Ent_Origin(0);
	for (int i = 0; i < numTk; i++) {
		Tank *t = &tk[i];
		Entity *e = E(t->ent);
		if (!(e->spawnflags & SF_CANCONTROL) || t->vol <= 0 || (e->flags & (EF_KILLED | EF_DISABLED)))
			continue;
		f32 mn[3], mx[3];
		Ent_Bounds(t->vol, mn, mx);
		if (p[0] < mn[0] || p[0] > mx[0] || p[1] < mn[1] || p[1] > mx[1] || p[2] + 36 < mn[2] || p[2] > mx[2])
			continue;
		/* (looking at the gun) */
		f32 bp[3], bf[3];
		barrel(t, bp, bf);
		f32 d[3] = {bp[0] - eye[0], bp[1] - eye[1], bp[2] - eye[2]};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
		if ((d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / l < 0.5f)
			continue;
		if (t->controller > 0)
			stop_control(t); /* (an NPC on it gets off) */
		start_control(t, 0);
		playerTank = i;
		Tanks_SetView(eye, fwd);
		return 1;
	}
	return 0;
}

void Tanks_PlayerLeave(void)
{
	if (playerTank >= 0)
		stop_control(&tk[playerTank]);
	playerTank = -1;
}

int Tanks_PlayerManned(void) { return playerTank >= 0; }

void Tanks_PlayerFire(int held)
{
	if (playerTank < 0 || !held)
		return;
	Tank *t = &tk[playerTank];
	f32 now = Ent_Now();
	if (now < t->nextAttack)
		return;
	f32 from[3], f[3];
	barrel(t, from, f);
	f32 goal[3] = {from[0] + f[0] * 1000, from[1] + f[1] * 1000, from[2] + f[2] * 1000};
	fire(t, from, f, goal, 1);
	t->nextAttack = now + 1.0f / (t->fireRate > 0 ? t->fireRate : 1);
}

int Tanks_Input(s32 ent, const char *in, const char *param)
{
	for (int i = 0; i < numTk; i++) {
		Tank *t = &tk[i];
		if (t->ent != ent)
			continue;
		if (!strcasecmp(in, "activate"))
			t->active = 1;
		else if (!strcasecmp(in, "deactivate")) {
			t->active = 0;
			t->haveTarget = 0;
		} else if (!strcasecmp(in, "setfirerate"))
			t->fireRate = (f32)atof(param);
		else if (!strcasecmp(in, "setdamage"))
			t->damage = (f32)atof(param);
		else if (!strcasecmp(in, "setmaxrange"))
			t->maxRange = (f32)atof(param);
		else if (!strcasecmp(in, "settargetposition")) {
			t->hasTargetPos = sscanf(param, "%f %f %f", &t->targetPos[0], &t->targetPos[1], &t->targetPos[2]) == 3;
		} else if (!strcasecmp(in, "settargetdir")) {
			f32 a[3] = {0};
			if (sscanf(param, "%f %f %f", &a[0], &a[1], &a[2]) >= 2) {
				f32 R[3][3];
				Ent_AngleMatrix(a, R);
				f32 p[3], f[3];
				barrel(t, p, f);
				for (int k = 0; k < 3; k++)
					t->targetPos[k] = p[k] + R[k][0] * 1000;
				t->hasTargetPos = 1;
			}
		} else if (!strcasecmp(in, "settargetentityname") || !strcasecmp(in, "settargetentity")) {
			t->targetEnt = find(param);
			t->hasTargetPos = 0;
		} else if (!strcasecmp(in, "cleartargetentity")) {
			t->targetEnt = -1;
			t->hasTargetPos = 0;
		} else if (!strcasecmp(in, "findnpctomantank") || !strcasecmp(in, "startfindingnpcs"))
			t->findNpcs = 1;
		else if (!strcasecmp(in, "stopfindingnpcs"))
			t->findNpcs = 0;
		else if (!strcasecmp(in, "forcenpcoff")) {
			if (t->controller > 0)
				stop_control(t);
		} else if (!strcasecmp(in, "shootgun") || !strcasecmp(in, "fireatwill")) {
			f32 p[3], f[3];
			barrel(t, p, f);
			f32 g[3] = {p[0] + f[0] * 1000, p[1] + f[1] * 1000, p[2] + f[2] * 1000};
			if (t->hasTargetPos)
				memcpy(g, t->targetPos, 12);
			fire(t, p, f, g, 0);
		} else if (!strcasecmp(in, "enableharrass") || !strcasecmp(in, "disableharrass"))
			;
		else
			return 0;
		return 1;
	}
	return 0;
}

int Tanks_Probe(s32 ent, s32 *controller, f32 *aimYaw)
{
	for (int i = 0; i < numTk; i++)
		if (tk[i].ent == ent) {
			Npc *m = tk[i].model > 0 ? Npc_OfEntity(tk[i].model) : NULL;
			*controller = tk[i].controller;
			*aimYaw = m ? m->aimYaw : tk[i].yaw;
			return tk[i].model > 0 ? 2 : 1;
		}
	return 0;
}
