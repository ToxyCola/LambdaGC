/*
 * hl2gc: Combine cameras and turrets, after Source's npc_combinecamera.cpp, npc_turret_ceiling.cpp and
 * npc_turret_ground.cpp. They don't walk: they deploy, look for what they hate, turn their head to it with
 * their aim_yaw / aim_pitch pose parameters (the ground turret turns as a whole) and, turrets, shoot.
 */
#include "sentries.h"

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

enum { SK_CAMERA = 1, SK_CEILING, SK_GROUND };
enum { ST_OFF, ST_DEPLOYING, ST_SEARCH, ST_ACTIVE, ST_RETIRING };
enum { EYE_IDLE, EYE_SEEKING, EYE_FOUND, EYE_ANGRY, EYE_HAPPY };

typedef struct {
	s32 ent;
	u8 kind, state, enabled, angry, eye, clicks, boom;
	f32 until;      /* the deploy / retire sequence ends */
	f32 goalYaw, goalPitch; /* ground turret: yaw wanted, in its own frame */
	f32 goalDir[3];         /* where it wants to look (world); zero: back to its rest aim */
	f32 shotTime, lastSight, lostAt, clickTime, flashOff, pingTime, eyeHappy;
	s32 target;     /* -1 none, 0 the player, an entity */
	u8 seenTarget;  /* the camera: has reported this target */
	f32 burstLeft;
	int burst;
	f32 yawSign, pitchSign; /* which way each pose parameter turns its eyes (measured at spawn) */
} Sentry;

#define MAX_SENTRIES 64
static Sentry sen[MAX_SENTRIES];
static int numSen;

static f32 frand(void) { return (f32)(rand() & 0xFFFF) / 65535.0f; }
static Entity *E(s32 i) { return Ent_List(NULL) + i; }
static f32 kvf(s32 ent, const char *k, f32 def)
{
	const char *v = Ent_KV(ent, k);
	return v && *v ? (f32)atof(v) : def;
}
static f32 angle_norm(f32 a)
{
	a = fmodf(a + 180.0f, 360.0f);
	if (a < 0)
		a += 360.0f;
	return a - 180.0f;
}
static f32 approach(f32 target, f32 v, f32 speed)
{
	f32 d = angle_norm(target - v);
	if (d > speed)
		d = speed;
	else if (d < -speed)
		d = -speed;
	return v + d;
}

static void snd(Sentry *s, int slot)
{
	const Pack *P = Ent_Pack();
	Entity *e = E(s->ent);
	if (P && slot >= 0 && slot < 3 && e->esnd[slot] >= 0 && (u32)e->esnd[slot] < P->numSounds)
		Audio_Play(&P->sounds[e->esnd[slot]], e->origin, 1.0f, SNDLVL_DEFAULT);
}

static int kind_of(const char *cn)
{
	if (!strcmp(cn, "npc_combine_camera"))
		return SK_CAMERA;
	if (!strcmp(cn, "npc_turret_ceiling"))
		return SK_CEILING;
	if (!strcmp(cn, "npc_turret_ground"))
		return SK_GROUND;
	return 0;
}

static void calibrate(Sentry *s, Npc *n);

void Sentries_Spawn(void)
{
	numSen = 0;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n && numSen < MAX_SENTRIES; i++) {
		int k = kind_of(ents[i].classname);
		Npc *np = k ? Npc_OfEntity((s32)i) : NULL;
		if (!np)
			continue;
		Sentry *s = &sen[numSen++];
		memset(s, 0, sizeof(*s));
		s->ent = (s32)i;
		s->kind = (u8)k;
		s->target = -1;
		u32 sf = ents[i].spawnflags;
		s->enabled = k == SK_CAMERA ? !(sf & 128) : k == SK_CEILING ? !(sf & 0x40) : 1;
		np->aimOn = k != SK_GROUND;
		np->aimYaw = np->aimPitch = 0;
		calibrate(s, np);
		if (k == SK_GROUND) {
			s->state = s->enabled ? ST_SEARCH : ST_OFF;
			Npc_PlaySequence(np, "idle", 1);
		} else if (s->enabled && k == SK_CAMERA) {
			/* (cameras start deployed and looking) */
			s->state = ST_SEARCH;
			Npc_PlaySequence(np, "idlealert", 1);
		} else {
			s->state = ST_OFF;
			Npc_PlaySequence(np, "idle", 1);
		}
	}
}

/* the "eyes" attachment: where it looks from (and the bullets come out) */
static void eyes(Sentry *s, Npc *n, f32 at[3], f32 fwd[3])
{
	f32 R[3][3];
	if (Npc_Attachment(n, "eyes", at, R)) {
		for (int k = 0; k < 3; k++)
			fwd[k] = R[k][0];
		return;
	}
	Entity *e = E(s->ent);
	memcpy(at, e->origin, 12);
	for (int k = 0; k < 3; k++)
		fwd[k] = e->R[k][0];
}

static int alive_target(s32 t)
{
	if (t == 0)
		return 1;
	if (t < 0 || (E(t)->flags & (EF_KILLED | EF_DISABLED)))
		return 0;
	Npc *n = Npc_OfEntity(t);
	return !n || !n->dead;
}

static void target_pos(s32 t, f32 out[3])
{
	const f32 *o = E(t)->origin;
	out[0] = o[0];
	out[1] = o[1];
	out[2] = o[2] + (t == 0 ? 40 : 32); /* (WorldSpaceCenter) */
}

/* the nearest thing it hates within range that it can see */
static s32 look(Sentry *s, const f32 from[3], f32 range)
{
	s32 best = -1;
	f32 bestD = range * range;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 0; i < n; i++) {
		if (i && (ents[i].npcModel < 0 || !alive_target((s32)i) || (s32)i == s->ent))
			continue;
		if (i == 0 && Ent_NoTarget())
			continue;
		if (Rel_Disposition(s->ent, (s32)i) != REL_HATE)
			continue;
		f32 p[3];
		target_pos((s32)i, p);
		f32 d[3] = {p[0] - from[0], p[1] - from[1], p[2] - from[2]};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd >= bestD)
			continue;
		Trace tr;
		CM_SetIgnoreEnt(s->ent);
		CM_TraceWorld(&tr, from, p, MASK_SHOT);
		CM_SetIgnoreEnt(-1);
		if (tr.fraction < 0.98f && tr.ent != (s32)i)
			continue;
		best = (s32)i;
		bestD = dd;
	}
	return best;
}

/* the aim wanted for a world point */
static void aim_at(Sentry *s, const f32 from[3], const f32 p[3])
{
	Entity *e = E(s->ent);
	f32 d[3] = {p[0] - from[0], p[1] - from[1], p[2] - from[2]};
	f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
	for (int k = 0; k < 3; k++)
		s->goalDir[k] = d[k] / l;
	f32 lo[3];
	for (int k = 0; k < 3; k++) /* (the ground turret turns its body: yaw in its own frame) */
		lo[k] = e->R[0][k] * d[0] + e->R[1][k] * d[1] + e->R[2][k] * d[2];
	s->goalYaw = atan2f(lo[1], lo[0]) * 180.0f / (f32)M_PI;
}

static void aim_rest(Sentry *s)
{
	s->goalDir[0] = s->goalDir[1] = s->goalDir[2] = 0;
}

static void fwd_angles(const f32 f[3], f32 *yaw, f32 *pitch)
{
	*yaw = atan2f(f[1], f[0]) * 180.0f / (f32)M_PI;
	*pitch = -atan2f(f[2], sqrtf(f[0] * f[0] + f[1] * f[1])) * 180.0f / (f32)M_PI; /* (down positive) */
}

/* which way aim_yaw / aim_pitch turn the eyes: each model hangs its "eyes" its own way, so measure it */
static void calibrate(Sentry *s, Npc *n)
{
	s->yawSign = s->pitchSign = 1;
	if (s->kind == SK_GROUND)
		return;
	f32 at[3], R[3][3], y0, p0, y1, p1;
	n->aimYaw = n->aimPitch = 0;
	if (!Npc_Attachment(n, "eyes", at, R))
		return;
	f32 f[3] = {R[0][0], R[1][0], R[2][0]};
	fwd_angles(f, &y0, &p0);
	n->aimYaw = 10;
	Npc_Attachment(n, "eyes", at, R);
	f[0] = R[0][0], f[1] = R[1][0], f[2] = R[2][0];
	fwd_angles(f, &y1, &p1);
	s->yawSign = angle_norm(y1 - y0) >= 0 ? 1.0f : -1.0f;
	n->aimYaw = 0;
	n->aimPitch = 10;
	Npc_Attachment(n, "eyes", at, R);
	f[0] = R[0][0], f[1] = R[1][0], f[2] = R[2][0];
	fwd_angles(f, &y1, &p1);
	s->pitchSign = p1 - p0 >= 0 ? 1.0f : -1.0f;
	n->aimPitch = 0;
}

/* CNPC_CombineCamera / CNPC_CeilingTurret::UpdateFacing: turn toward the goal, the error from where the eyes
 * point now, at most speed degrees a second (Source: 0.1 * MaxYawSpeed a think, / 1.5) */
static void turn(Sentry *s, Npc *n, f32 speed, f32 dt)
{
	if (s->kind == SK_GROUND) {
		n->idealYaw = E(s->ent)->angles[1] + s->goalYaw;
		return;
	}
	f32 step = speed * dt;
	if (s->goalDir[0] == 0 && s->goalDir[1] == 0 && s->goalDir[2] == 0) {
		n->aimYaw = approach(0, n->aimYaw, step);
		n->aimPitch = approach(0, n->aimPitch, step);
		return;
	}
	f32 at[3], R[3][3];
	if (!Npc_Attachment(n, "eyes", at, R))
		return;
	f32 f[3] = {R[0][0], R[1][0], R[2][0]}, cy, cp, gy, gp;
	fwd_angles(f, &cy, &cp);
	fwd_angles(s->goalDir, &gy, &gp);
	f32 dy = angle_norm(gy - cy), dp = gp - cp;
	dy = dy > step ? step : dy < -step ? -step : dy;
	dp = dp > step ? step : dp < -step ? -step : dp;
	n->aimYaw = angle_norm(n->aimYaw + s->yawSign * dy / 1.5f);
	n->aimPitch += s->pitchSign * dp / 1.5f;
	n->aimPitch = n->aimPitch < -90 ? -90 : n->aimPitch > 90 ? 90 : n->aimPitch;
}

static void shoot(Sentry *s, Npc *n, const f32 from[3], const f32 dir[3])
{
	/* AR2 rounds (sk_plr_dmg_ar2 8 / sk_npc_dmg_ar2 3), 2 degree spread, a tracer each */
	f32 d[3];
	for (int k = 0; k < 3; k++)
		d[k] = dir[k] + (frand() - 0.5f) * 0.07f;
	f32 end[3] = {from[0] + d[0] * 4096, from[1] + d[1] * 4096, from[2] + d[2] * 4096};
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_SetIgnoreEnt(s->ent);
	CM_BoxTrace(&tr, from, end, z, z, MASK_SHOT);
	CM_SetIgnoreEnt(-1);
	/* the player: the box trace doesn't hit him, so test the line against his box */
	const f32 *pp = E(0)->origin;
	f32 to[3] = {pp[0] - from[0], pp[1] - from[1], pp[2] + 36 - from[2]};
	f32 t = to[0] * d[0] + to[1] * d[1] + to[2] * d[2];
	f32 dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	t /= dl * dl;
	int hitPlayer = 0;
	if (t > 0 && t * dl < tr.fraction * 4096 * dl) {
		f32 c[3] = {from[0] + d[0] * t - pp[0], from[1] + d[1] * t - pp[1], from[2] + d[2] * t - pp[2] - 36};
		if (fabsf(c[0]) < 16 && fabsf(c[1]) < 16 && fabsf(c[2]) < 36)
			hitPlayer = 1;
	}
	if (hitPlayer)
		Ent_HurtPlayer(s->kind == SK_GROUND ? 3.0f : 8.0f, DMG_BULLET, from);
	else if (tr.fraction < 1) {
		if (tr.ent > 0 && Npc_OfEntity(tr.ent)) {
			f32 dn[3] = {d[0] / dl, d[1] / dl, d[2] / dl};
			Ent_Damage(tr.ent, 3.0f, dn, tr.endpos);
		} else {
			f32 dn[3] = {d[0] / dl, d[1] / dl, d[2] / dl};
			Weap_BulletImpact(&tr, dn);
		}
	}
	Weap_Muzzle(from, s->kind == SK_GROUND ? 0.6f : 1.0f);
	snd(s, 1);
	(void)n;
}

static void camera_tick(Sentry *s, Npc *n, f32 dt, f32 now)
{
	f32 from[3], fwd[3];
	eyes(s, n, from, fwd);
	f32 inner = kvf(s->ent, "innerradius", 300), outer = kvf(s->ent, "outerradius", 450);
	u32 sf = E(s->ent)->spawnflags;
	s32 t = (sf & 0x40) ? -1 : look(s, from, outer); /* SF_COMBINE_CAMERA_IGNOREENEMIES */
	if (t < 0) {
		if (s->target >= 0) {
			/* nobody suspicious any more: back to idle */
			s->target = -1;
			s->seenTarget = 0;
			if ((sf & 32) && s->angry) /* (it got angry by itself: calms by itself) */
				s->angry = 0;
			s->eye = EYE_IDLE;
		}
		/* idle: it watches its default target (a cop at a gate, a chair), else looks ahead as it was hung */
		const char *dt_ = Ent_KV(s->ent, "defaulttarget");
		s32 dtar = dt_ && *dt_ ? Ent_FindActor(dt_) : -1;
		if (dtar > 0) {
			f32 dp[3];
			target_pos(dtar, dp);
			aim_at(s, from, dp);
		} else
			aim_rest(s);
		turn(s, n, 60, dt);
		return;
	}
	if (t != s->target) {
		s->target = t;
		s->seenTarget = 0;
		s->eye = EYE_SEEKING;
	}
	f32 p[3];
	target_pos(t, p);
	f32 d2 = (p[0] - from[0]) * (p[0] - from[0]) + (p[1] - from[1]) * (p[1] - from[1]) + (p[2] - from[2]) * (p[2] - from[2]);
	if (!s->seenTarget && d2 < inner * inner) {
		/* within the inner radius: it has found them */
		s->seenTarget = 1;
		Ent_FireOutput(s->ent, "onfoundenemy", t);
		if (t == 0) {
			s->eye = EYE_FOUND;
			if (sf & 32) { /* SF_COMBINE_CAMERA_BECOMEANGRY */
				if (!s->angry) {
					s->angry = 1;
					s->clicks = 0;
					s->clickTime = now + 0.4f;
					snd(s, 1);
				}
			} else {
				snd(s, 0);
			}
			Ent_FireOutput(s->ent, "onfoundplayer", 0);
		} else {
			s->eye = EYE_HAPPY;
			s->eyeHappy = now + 2;
		}
	}
	aim_at(s, from, p);
	turn(s, n, 180, dt);
	/* angry: a few photos of them (the flash and the shutter) */
	if (s->angry && s->clicks <= 3 && s->clickTime && now >= s->clickTime) {
		snd(s, 2);
		s->flashOff = now + 0.1f;
		s->clickTime = now + 0.3f;
		s->clicks++;
	}
	/* (it pings so the player knows where it is) */
	if (now >= s->pingTime) {
		s->pingTime = now + 1.0f;
	}
}

static void turret_tick(Sentry *s, Npc *n, f32 dt, f32 now)
{
	f32 from[3], fwd[3];
	eyes(s, n, from, fwd);
	u32 sf = E(s->ent)->spawnflags;
	f32 range = s->kind == SK_GROUND ? 1200.0f : 1500.0f; /* CEILING_TURRET_RANGE */
	switch (s->state) {
	case ST_OFF:
		/* AutoActivate: it deploys when it sees something to shoot */
		if (s->enabled && (s->kind == SK_GROUND || (sf & 0x20)) && look(s, from, range) >= 0) {
			s->state = ST_DEPLOYING;
			f32 dur = Npc_PlaySequence(n, "deploy", 0);
			s->until = now + (dur > 0 ? dur : 1.0f);
			snd(s, 0);
		} else if (s->enabled && s->kind == SK_CEILING && !(sf & 0x20)) {
			s->state = ST_DEPLOYING;
			f32 dur = Npc_PlaySequence(n, "deploy", 0);
			s->until = now + (dur > 0 ? dur : 1.0f);
			snd(s, 0);
		}
		return;
	case ST_DEPLOYING:
		if (now >= s->until) {
			s->state = ST_SEARCH;
			Npc_PlaySequence(n, "idlealert", 1);
			Ent_FireOutput(s->ent, "ondeploy", 0);
			s->lostAt = now;
		}
		return;
	case ST_RETIRING:
		if (now >= s->until) {
			s->state = ST_OFF;
			Npc_PlaySequence(n, "idle", 1);
			Ent_FireOutput(s->ent, "onretire", 0);
		}
		return;
	default:
		break;
	}
	if (!s->enabled) {
		/* told to go away: back up into the ceiling */
		s->state = ST_RETIRING;
		f32 dur = Npc_PlaySequence(n, "retract", 0);
		s->until = now + (dur > 0 ? dur : 1.0f);
		snd(s, 2);
		s->target = -1;
		return;
	}
	s32 t = look(s, from, range);
	if (t >= 0) {
		s->target = t;
		s->lastSight = now;
	} else if (s->target >= 0 && (!alive_target(s->target) || now - s->lastSight > 0.5f)) {
		s->target = -1;
		s->lostAt = now;
		if (s->kind == SK_GROUND)
			Ent_FireOutput(s->ent, "onareaclear", 0);
	}
	if (s->target < 0) {
		if (s->kind != SK_GROUND)
			aim_rest(s);
		turn(s, n, 60, dt);
		/* nothing for a while: retire (unless it never does) */
		if (s->kind == SK_CEILING && !(sf & 0x80) && now - s->lostAt > 7.0f) {
			s->state = ST_RETIRING;
			f32 dur = Npc_PlaySequence(n, "retract", 0);
			s->until = now + (dur > 0 ? dur : 1.0f);
			snd(s, 2);
		}
		return;
	}
	s->state = ST_ACTIVE;
	f32 p[3];
	target_pos(s->target, p);
	aim_at(s, from, p);
	turn(s, n, s->kind == SK_GROUND ? 90 : 180, dt);
	/* fire when it's looking within 10 degrees of them, ten rounds a second (the ground turret in bursts) */
	f32 to[3] = {p[0] - from[0], p[1] - from[1], p[2] - from[2]};
	f32 l = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]) + 1e-3f;
	f32 dot = (to[0] * fwd[0] + to[1] * fwd[1] + to[2] * fwd[2]) / l;
	if (now >= s->shotTime && dot >= 0.9848f && !(sf & 0x100)) { /* SF_CEILING_TURRET_OUT_OF_AMMO */
		f32 dir[3] = {to[0] / l, to[1] / l, to[2] / l};
		shoot(s, n, from, dir);
		if (s->kind == SK_GROUND) {
			if (--s->burst <= 0) {
				s->burst = 5;
				s->shotTime = now + 1.0f;
			} else
				s->shotTime = now + 0.1f;
		} else {
			s->shotTime = now + 0.1f;
		}
	}
}

void Sentries_Tick(f32 dt)
{
	f32 now = Ent_Now();
	for (int i = 0; i < numSen; i++) {
		Sentry *s = &sen[i];
		Entity *e = E(s->ent);
		Npc *n = Npc_OfEntity(s->ent);
		if (!n)
			continue;
		if (n->dead) {
			if (!s->boom) {
				/* destroyed: it blows up (ExplosionCreate), sparks */
				s->boom = 1;
				Weap_Explosion(e->origin, 0.5f, 0);
				Ent_FireOutput(s->ent, "ondamaged", 0);
			}
			continue;
		}
		if (e->flags & EF_KILLED)
			continue;
		if (e->flags & EF_DISABLED)
			continue;
		if (s->kind == SK_CAMERA) {
			if (!s->enabled)
				continue;
			camera_tick(s, n, dt, now);
		} else {
			turret_tick(s, n, dt, now);
		}
	}
}

int Sentries_Input(s32 ent, const char *in)
{
	for (int i = 0; i < numSen; i++) {
		Sentry *s = &sen[i];
		if (s->ent != ent)
			continue;
		Npc *n = Npc_OfEntity(ent);
		if (!strcasecmp(in, "enable")) {
			if (!s->enabled && s->kind == SK_CAMERA && n) {
				Npc_PlaySequence(n, "deploy", 0);
				s->state = ST_SEARCH;
			}
			s->enabled = 1;
		} else if (!strcasecmp(in, "disable")) {
			s->enabled = 0;
			s->target = -1;
			if (s->kind == SK_CAMERA && n)
				Npc_PlaySequence(n, "retract", 0);
		} else if (!strcasecmp(in, "toggle")) {
			return Sentries_Input(ent, s->enabled ? "disable" : "enable");
		} else if (!strcasecmp(in, "setangry")) {
			if (!s->angry) {
				s->angry = 1;
				s->clicks = 0;
				s->clickTime = Ent_Now() + 0.4f;
				s->eye = EYE_ANGRY;
				snd(s, 1);
			}
		} else if (!strcasecmp(in, "setidle")) {
			s->angry = 0;
			s->eye = s->target >= 0 ? EYE_SEEKING : EYE_IDLE;
		} else {
			return 0;
		}
		return 1;
	}
	return 0;
}

int Sentries_Count(void) { return numSen; }

int Sentries_Glow(int i, f32 pos[3], u32 *rgba, f32 *size)
{
	if (i < 0 || i >= numSen)
		return 0;
	Sentry *s = &sen[i];
	Npc *n = Npc_OfEntity(s->ent);
	if (!n || n->dead || !s->enabled || (E(s->ent)->flags & (EF_KILLED | EF_DISABLED)) || s->state == ST_OFF)
		return 0;
	f32 fwd[3];
	eyes(s, n, pos, fwd);
	f32 now = Ent_Now();
	if (s->kind == SK_CAMERA && s->flashOff > now) {
		*rgba = 0xFFFFFFFFu; /* the photo's flash */
		*size = 24;
		return 1;
	}
	/* SetEyeState: idle yellow-ish, seeking yellow, found / angry red, happy green */
	static const u32 col[5] = {0xFFC00080u, 0xFFFF00C0u, 0xFF0000FFu, 0xFF0000FFu, 0x00FF00FFu};
	int eye = s->eye;
	if (eye == EYE_HAPPY && now > s->eyeHappy)
		eye = EYE_IDLE;
	if (s->kind != SK_CAMERA)
		eye = s->target >= 0 ? EYE_FOUND : EYE_IDLE;
	*rgba = col[eye];
	*size = s->kind == SK_CAMERA ? 6 : 4;
	return 1;
}

int Sentries_Probe(s32 ent, f32 *aimYaw, f32 *aimPitch, s32 *target, int *state)
{
	for (int i = 0; i < numSen; i++)
		if (sen[i].ent == ent) {
			Npc *n = Npc_OfEntity(ent);
			*aimYaw = n ? n->aimYaw : 0;
			*aimPitch = n ? n->aimPitch : 0;
			*target = sen[i].target;
			*state = sen[i].state;
			return 1;
		}
	return 0;
}
