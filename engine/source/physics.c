#include "physics.h"
#include "weapons.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collide.h"
#include "ent.h"
#include "npc.h"

#define GRAVITY 600.0f
#define RESTITUTION 0.25f
#define FRICTION 0.6f
#define MAX_LIFT_MASS 35.0f
#define HOLD_DIST 54.0f
#define SLEEP_TIME 0.6f
#define NUM_SAMPLES 14

typedef struct {
	s32 ent;
	f32 mass, invMass;
	f32 invI[3];          /* inverse inertia, box, local axes */
	f32 center[3];        /* box centre in model space */
	f32 half[3];
	u8 inWater;
	f32 q[4];             /* orientation (x, y, z, w) */
	f32 com[3];           /* world centre of mass */
	f32 v[3], w[3];
	f32 still;
	u8 awake, frozen;
	u8 noCollide; /* hinged to the world with "no collision" (a phys_motor's propeller): the world doesn't stop it */
	f32 nextImpact; /* impact sounds: not more often than this (Source: 0.05 s per object) */
	f32 nextNpcHit; /* NPC impact damage: once per hit, not every tick of the contact */
	f32 samp[NUM_SAMPLES][3]; /* where it touches things (model space, from centre) */
} Body;

static const Pack *P;
static Player *player;
static int heldByGun; /* what's held is in the gravity gun (else the player's hands) */
static f32 physClock; /* seconds simulated (impact sound timing) */
static Body *bodies;
static u32 numBodies;
static s32 held = -1;     /* body index */
static f32 holdDist = HOLD_DIST;
static f32 viewEye[3], viewFwd[3];
static f32 wish[2]; /* the player's intended walking velocity (input, before collision) */

static f32 dot(const f32 a[3], const f32 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross(const f32 a[3], const f32 b[3], f32 o[3])
{
	f32 r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	memcpy(o, r, 12);
}

static void quat_to_R(const f32 q[4], f32 R[3][3])
{
	f32 x = q[0], y = q[1], z = q[2], w = q[3];
	R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - w * z);     R[0][2] = 2 * (x * z + w * y);
	R[1][0] = 2 * (x * y + w * z);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - w * x);
	R[2][0] = 2 * (x * z - w * y);     R[2][1] = 2 * (y * z + w * x);     R[2][2] = 1 - 2 * (x * x + y * y);
}

static void R_to_quat(const f32 R[3][3], f32 q[4])
{
	f32 t = R[0][0] + R[1][1] + R[2][2];
	if (t > 0) {
		f32 s = sqrtf(t + 1) * 2;
		q[3] = 0.25f * s;
		q[0] = (R[2][1] - R[1][2]) / s;
		q[1] = (R[0][2] - R[2][0]) / s;
		q[2] = (R[1][0] - R[0][1]) / s;
	} else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
		f32 s = sqrtf(1 + R[0][0] - R[1][1] - R[2][2]) * 2;
		q[3] = (R[2][1] - R[1][2]) / s;
		q[0] = 0.25f * s;
		q[1] = (R[0][1] + R[1][0]) / s;
		q[2] = (R[0][2] + R[2][0]) / s;
	} else if (R[1][1] > R[2][2]) {
		f32 s = sqrtf(1 + R[1][1] - R[0][0] - R[2][2]) * 2;
		q[3] = (R[0][2] - R[2][0]) / s;
		q[0] = (R[0][1] + R[1][0]) / s;
		q[1] = 0.25f * s;
		q[2] = (R[1][2] + R[2][1]) / s;
	} else {
		f32 s = sqrtf(1 + R[2][2] - R[0][0] - R[1][1]) * 2;
		q[3] = (R[1][0] - R[0][1]) / s;
		q[0] = (R[0][2] + R[2][0]) / s;
		q[1] = (R[1][2] + R[2][1]) / s;
		q[2] = 0.25f * s;
	}
}

/* Source angles (pitch, yaw, roll) from a rotation whose columns are forward, left, up */
static void R_to_angles(const f32 R[3][3], f32 a[3])
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

static Entity *ent_of(const Body *b)
{
	u32 n;
	return &Ent_List(&n)[b->ent];
}

/* write the body's pose back to its entity (origin = model origin, not the centre of mass) */
static void sync(Body *b)
{
	Entity *e = ent_of(b);
	quat_to_R(b->q, e->R);
	for (int r = 0; r < 3; r++)
		e->origin[r] = b->com[r] - (e->R[r][0] * b->center[0] + e->R[r][1] * b->center[1] + e->R[r][2] * b->center[2]);
	R_to_angles(e->R, e->angles);
}

static Body *body_of(s32 ent)
{
	for (u32 i = 0; i < numBodies; i++)
		if (bodies[i].ent == ent)
			return &bodies[i];
	return NULL;
}

/* what's simulated: prop_physics models and func_physbox brushes */
static int is_body_ent(const Entity *e)
{
	if (e->mdl >= 0 && !strncmp(e->classname, "prop_physics", 12) && P && (u32)e->mdl < P->numModels)
		return 1;
	return e->bmodel > 0 && P && (u32)e->bmodel < P->numBModels && !strcmp(e->classname, "func_physbox");
}

/* the "Motion Disabled" spawnflag: 8 on props, 0x8000 on func_physbox (SF_PHYSBOX_MOTIONDISABLED) */
static int motion_disabled(const Entity *e)
{
	return e->mdl < 0 ? (e->spawnflags & 0x8000) != 0 : (e->spawnflags & 8) != 0;
}

static void init_constraints(void);
static void init_samples(Body *b, const Entity *e);
static void sample_point(const Body *b, int i, f32 out[3]);
static Body *body_init(u32 i);
static u32 bodyCap;
#define PHYS_SPARE 32
static void solve_constraints(f32 dt);

static Body *body_init(u32 i)
{
	const Pack *p = P;
	Entity *e = Ent_List(NULL) + i;
	if (!is_body_ent(e) || numBodies >= bodyCap)
		return NULL;
	{
		const f32 *lo, *hi;
		int brush = e->mdl < 0;
		if (brush) {
			lo = p->bmodels[e->bmodel].mins;
			hi = p->bmodels[e->bmodel].maxs;
		} else {
			lo = p->models[e->mdl].mins;
			hi = p->models[e->mdl].maxs;
		}
		Body *b = &bodies[numBodies++];
		b->ent = (s32)i;
		const char *ms = Ent_KV((s32)i, "_mass");
		if (brush) {
			/* func_physbox: Source weighs the brush by its volume and material; a box of the same size at
			 * about 0.01 kg per cubic inch (so a plug or a crate is carried by hand, as on PC) */
			f32 vol = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
			b->mass = vol * 0.01f;
			b->mass = b->mass < 2 ? 2 : b->mass > 400 ? 400 : b->mass;
		} else {
			b->mass = ms ? (f32)atof(ms) : 30.0f;
		}
		const char *ov = Ent_KV((s32)i, "massscale");
		if (ov && atof(ov) > 0)
			b->mass *= (f32)atof(ov);
		if (b->mass < 1)
			b->mass = 1;
		b->invMass = 1.0f / b->mass;
		f32 d[3];
		for (int k = 0; k < 3; k++) {
			b->center[k] = (lo[k] + hi[k]) * 0.5f;
			b->half[k] = (hi[k] - lo[k]) * 0.5f;
			if (b->half[k] < 1)
				b->half[k] = 1;
			d[k] = b->half[k] * 2;
		}
		/* box inertia; scaled up a little, which damps wobble */
		b->invI[0] = 12.0f / (b->mass * (d[1] * d[1] + d[2] * d[2])) * 0.6f;
		b->invI[1] = 12.0f / (b->mass * (d[0] * d[0] + d[2] * d[2])) * 0.6f;
		b->invI[2] = 12.0f / (b->mass * (d[0] * d[0] + d[1] * d[1])) * 0.6f;
		R_to_quat(e->R, b->q);
		for (int r = 0; r < 3; r++)
			b->com[r] = e->origin[r] + e->R[r][0] * b->center[0] + e->R[r][1] * b->center[1] + e->R[r][2] * b->center[2];
		init_samples(b, e);
		b->frozen = motion_disabled(e); /* "Motion Disabled" */
		/* (asleep until something disturbs it: props are stacked on each other and on debris, which this
		 * solver can't hold up at once) */
		b->awake = 0;
		if (!b->frozen && (CM_PointContents(e->origin) & MASK_WATER))
			b->awake = 1; /* things placed in water float from the start */
	}
	return &bodies[numBodies - 1];
}

int Phys_AddBody(s32 ent)
{
	if (!bodies || ent <= 0 || body_of(ent))
		return 0;
	return body_init((u32)ent) != NULL;
}

/* Source's props start awake (only "Start Asleep" ones don't) and settle onto what's under them when the
 * level starts: trash a mapper left a little above the floor drops onto it. Here the ones standing alone
 * are put down: clear of everything, no other prop above or below them, not tied by a constraint
 * (stacks and things put partly inside furniture stay as they are). */
static int constrained(s32 ent);
static void settle_loose(void)
{
	Entity *E = Ent_List(&(u32){0});
	for (u32 i = 0; i < numBodies; i++) {
		Body *b = &bodies[i];
		Entity *e = ent_of(b);
		if (b->frozen || b->awake || constrained(b->ent) || e->parent >= 0)
			continue;
		int asleep = !strcmp(e->classname, "func_physbox") ? (e->spawnflags & 0x1000) != 0 : (e->spawnflags & 1) != 0;
		if (asleep)
			continue;
		/* another prop over or under it */
		int alone = 1;
		f32 r = b->half[0] > b->half[1] ? b->half[0] : b->half[1];
		for (u32 j = 0; j < numBodies && alone; j++) {
			if (j == i)
				continue;
			const Body *o = &bodies[j];
			f32 orr = o->half[0] > o->half[1] ? o->half[0] : o->half[1];
			f32 dx = o->com[0] - b->com[0], dy = o->com[1] - b->com[1];
			if (dx * dx + dy * dy < (r + orr) * (r + orr) && fabsf(o->com[2] - b->com[2]) < b->half[2] + o->half[2] + 48)
				alone = 0;
		}
		if (!alone)
			continue;
		/* clear of the world and everything else */
		int clear = 1;
		CM_SetIgnoreEnt(b->ent);
		f32 R[3][3];
		quat_to_R(b->q, R);
		for (int k = 0; k < NUM_SAMPLES && clear; k++) {
			f32 l[3], w[3];
			sample_point(b, k, l);
			for (int c = 0; c < 3; c++)
				w[c] = b->com[c] + R[c][0] * l[0] + R[c][1] * l[1] + R[c][2] * l[2];
			Trace tr;
			static const f32 z[3] = {0, 0, 0};
			CM_BoxTrace(&tr, w, w, z, z, MASK_PLAYERSOLID);
			clear = !tr.startsolid;
		}
		/* how far its lowest points are above what's under them: if it's clearly hanging in the air, it's
		 * put down onto it, turned as the mapper left it (no simulation: nothing to jitter or tip) */
		if (clear) {
			f32 gap = 64.0f;
			for (int k = 0; k < NUM_SAMPLES; k++) {
				f32 l[3], w[3];
				sample_point(b, k, l);
				for (int c = 0; c < 3; c++)
					w[c] = b->com[c] + R[c][0] * l[0] + R[c][1] * l[1] + R[c][2] * l[2];
				if (w[2] > b->com[2] - b->half[2] * 0.25f)
					continue; /* (only the low ones) */
				Trace tr;
				static const f32 z[3] = {0, 0, 0};
				f32 dn[3] = {w[0], w[1], w[2] - 64.0f};
				CM_BoxTrace(&tr, w, dn, z, z, MASK_PLAYERSOLID | MASK_WATER);
				f32 g = tr.fraction * 64.0f;
				if (g < gap)
					gap = g;
			}
			if (gap > 4.0f && gap < 64.0f) {
				b->com[2] -= gap - 0.5f;
				sync(b);
			}
		}
		CM_SetIgnoreEnt(-1);
	}
	(void)E;
}

int Phys_Init(const Pack *p, Player *pl)
{
	Phys_Free();
	P = p;
	player = pl;
	u32 n;
	Entity *ents = Ent_List(&n);
	u32 count = 0;
	for (u32 i = 1; i < n; i++)
		if (is_body_ent(&ents[i]))
			count++;
	bodyCap = count + PHYS_SPARE; /* room for props made while playing (env_entity_maker) */
	bodies = calloc(bodyCap, sizeof(Body));
	if (!bodies)
		return -1;
	for (u32 i = 1; i < n; i++)
		body_init(i);
	held = -1;
	init_constraints();
	settle_loose();
	return 0;
}

void Phys_Free(void)
{
	free(bodies);
	bodies = NULL;
	numBodies = 0;
	held = -1;
}

void Phys_Wake(s32 ent)
{
	Body *b = body_of(ent);
	if (b && !b->frozen) {
		b->awake = 1;
		b->still = 0;
	}
}

void Phys_SetMotion(s32 ent, int enabled)
{
	Body *b = body_of(ent);
	if (b) {
		b->frozen = !enabled;
		if (enabled)
			Phys_Wake(ent);
	}
}

/* world-space inverse inertia times a vector */
static void apply_invI(const Body *b, const f32 R[3][3], const f32 x[3], f32 out[3])
{
	f32 l[3];
	for (int k = 0; k < 3; k++)
		l[k] = (R[0][k] * x[0] + R[1][k] * x[1] + R[2][k] * x[2]) * b->invI[k];
	for (int r = 0; r < 3; r++)
		out[r] = R[r][0] * l[0] + R[r][1] * l[1] + R[r][2] * l[2];
}

static const s8 sampDirs[NUM_SAMPLES][3] = {
	{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1},
	{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0},
};

static void sample_point(const Body *b, int i, f32 out[3])
{
	memcpy(out, b->samp[i], 12);
}

/* the points it touches things with: its box's corners and the middles of its faces */
static void init_samples(Body *b, const Entity *e)
{
	(void)e;
	for (int i = 0; i < NUM_SAMPLES; i++)
		for (int k = 0; k < 3; k++)
			b->samp[i][k] = sampDirs[i][k] * b->half[k] * 0.98f;
}

/* Props against NPC bodies (the world traces above skip NPC boxes, which are only upright boxes):
 * a prop moving into one stops against it and pushes out, and a fast one hurts it, by Source's
 * gDefaultNPCImpactDamageTable (speed^2 thresholds 150/250/350/500/1000 -> 5/10/50/100/500 damage;
 * under 2 kg does nothing; held objects don't hurt). */
static void npc_contacts(Body *b, int holding)
{
	u32 nn, ne;
	Npc *list = Npc_List(&nn);
	Entity *E = Ent_List(&ne);
	f32 r = b->half[0];
	for (int k = 1; k < 3; k++)
		r = b->half[k] > r ? b->half[k] : r;
	r *= 0.8f; /* (a sphere a bit inside the box's corners) */
	for (u32 i = 0; i < nn; i++) {
		s32 ei = list[i].ent;
		if (ei <= 0 || (u32)ei >= ne)
			continue;
		Entity *e = &E[ei];
		if (!(e->flags & EF_SOLID) || (e->flags & (EF_KILLED | EF_DISABLED)) || e->cls != EC_NPC || e->npcModel < 0)
			continue;
		f32 h = P->npcModels[e->npcModel].height;
		h = h > 72 ? 72 : h < 8 ? 8 : h;
		f32 mn[3] = {e->origin[0] - 13, e->origin[1] - 13, e->origin[2]}, mx[3] = {e->origin[0] + 13, e->origin[1] + 13, e->origin[2] + h};
		f32 c[3], d[3], dd = 0;
		for (int k = 0; k < 3; k++) {
			c[k] = b->com[k] < mn[k] ? mn[k] : b->com[k] > mx[k] ? mx[k] : b->com[k];
			d[k] = b->com[k] - c[k];
			dd += d[k] * d[k];
		}
		if (dd >= r * r)
			continue;
		f32 dl = sqrtf(dd), n[3];
		if (dl < 0.01f) { /* centre inside the box: out sideways from the NPC's middle */
			n[0] = b->com[0] - e->origin[0];
			n[1] = b->com[1] - e->origin[1];
			n[2] = 0;
			f32 l = sqrtf(n[0] * n[0] + n[1] * n[1]);
			if (l < 0.01f)
				n[0] = 1, l = 1;
			n[0] /= l;
			n[1] /= l;
			dl = 0;
		} else
			for (int k = 0; k < 3; k++)
				n[k] = d[k] / dl;
		/* (never down: an NPC stepping over a prop on the floor mustn't press it through the floor) */
		if (n[2] < 0) {
			n[2] = 0;
			f32 l = sqrtf(n[0] * n[0] + n[1] * n[1]);
			if (l < 0.01f)
				continue;
			n[0] /= l;
			n[1] /= l;
		}
		f32 vn = dot(b->v, n);
		if (vn < 0) {
			f32 speed2 = vn * vn;
			if (!holding && b->mass >= 2.0f && physClock >= b->nextNpcHit && speed2 >= 150.0f * 150.0f) {
				f32 dmg = speed2 >= 1000.0f * 1000.0f ? 500 : speed2 >= 500.0f * 500.0f ? 100 : speed2 >= 350.0f * 350.0f ? 50
				        : speed2 >= 250.0f * 250.0f ? 10 : 5;
				if (b->mass < 8.0f)
					dmg *= b->mass / 8.0f; /* light things hurt less */
				f32 dir[3] = {-n[0], -n[1], -n[2]};
				f32 l = sqrtf(b->v[0] * b->v[0] + b->v[1] * b->v[1] + b->v[2] * b->v[2]);
				if (l > 1)
					for (int k = 0; k < 3; k++)
						dir[k] = b->v[k] / l;
				b->nextNpcHit = physClock + 0.25f;
				Ent_Damage(ei, dmg, dir, c);
			}
			/* stop against it, with a little bounce */
			for (int k = 0; k < 3; k++)
				b->v[k] -= 1.3f * vn * n[k];
			for (int k = 0; k < 3; k++)
				b->w[k] *= 0.8f;
		}
		/* out of the box */
		f32 pen = r - dl;
		if (pen > 0) {
			f32 out = pen > 4 ? 4 : pen;
			/* (as far as the world lets it go) */
			f32 to[3] = {b->com[0] + n[0] * out, b->com[1] + n[1] * out, b->com[2] + n[2] * out};
			Trace tw;
			CM_TraceWorld(&tw, b->com, to, MASK_PLAYERSOLID);
			if (!tw.startsolid)
				memcpy(b->com, tw.endpos, 12);
		}
		if (vn < 0 || pen > 1)
			b->awake = 1;
	}
}

static f32 read_table(const f32 (*t)[2], int n, f32 energy)
{
	f32 d = 0;
	for (int i = 0; i < n; i++)
		if (energy >= t[i][0] && t[i][1] > d)
			d = t[i][1];
	return d;
}

static void impact_damage(Body *b, f32 dv, const f32 at[3])
{
	Entity *e = ent_of(b);
	if ((e->spawnflags & 2) || !Ent_IsProp(b->ent))
		return;
	const char *hp = Ent_KV(b->ent, "_hp");
	if (!hp)
		return; /* (not breakable) */
	const char *mt = Ent_KV(b->ent, "_mat"), *sc = Ent_KV(b->ent, "physdamagescale");
	int mat = mt ? atoi(mt) : 0;
	f32 scale = sc ? (f32)atof(sc) : 0;
	if (scale <= 0)
		scale = 0.1f;
	/* propdata.txt: Glass.Small, Glass.Window and Pottery.Small use damage_table "glass" */
	int glass = mat == 2 || (mat == 4 && atof(hp) <= 5);
	static const f32 npcT[5][2] = {{150 * 150, 5}, {250 * 250, 10}, {350 * 350, 50}, {500 * 500, 100}, {1000 * 1000, 500}};
	static const f32 glassT[6][2] = {{25 * 25, 10}, {50 * 50, 20}, {100 * 100, 50}, {200 * 200, 75}, {500 * 500, 100}, {250 * 250, 500}};
	f32 energy = dv * dv * scale;
	f32 dmg = glass ? read_table(glassT, 6, energy) : read_table(npcT, 5, energy);
	if (dmg > 0)
		Ent_Damage(b->ent, dmg, NULL, at);
}

static void step_body(Body *b, f32 dt)
{
	f32 R[3][3];
	quat_to_R(b->q, R);
	f32 prev[NUM_SAMPLES][3];
	for (int i = 0; i < NUM_SAMPLES; i++) {
		f32 l[3];
		sample_point(b, i, l);
		for (int r = 0; r < 3; r++)
			prev[i][r] = b->com[r] + R[r][0] * l[0] + R[r][1] * l[1] + R[r][2] * l[2];
	}

	int holding = (held >= 0 && &bodies[held] == b);
	if (holding) {
		/* drive towards a point in front of the player's view */
		f32 target[3];
		f32 reach = holdDist + (b->half[0] > b->half[1] ? b->half[0] : b->half[1]);
		for (int k = 0; k < 3; k++)
			target[k] = viewEye[k] + viewFwd[k] * reach;
		for (int k = 0; k < 3; k++) {
			b->v[k] = (target[k] - b->com[k]) * 12.0f;
			if (b->v[k] > 500)
				b->v[k] = 500;
			if (b->v[k] < -500)
				b->v[k] = -500;
			b->w[k] *= 0.8f;
		}
	} else {
		b->v[2] -= GRAVITY * dt;
		/* water (VPhysics' fluid controller): the lift on each part of the body is the weight of the water
		 * that part displaces, pushing at that part, so a floating crate settles at the depth its density
		 * gives, tilts until it floats level, and bobs; the water's drag slows it, more the deeper it is.
		 * The body's 8 corners and centre each carry a share of its volume, and how much of a share is
		 * under depends on how far below the surface that point is (over the body's own height). */
		f32 extZ = fabsf(R[2][0]) * b->half[0] + fabsf(R[2][1]) * b->half[1] + fabsf(R[2][2]) * b->half[2];
		if (extZ < 1)
			extZ = 1;
		f32 pts[9][3];
		int anyWet = 0;
		for (int i = 0; i < 9; i++) {
			f32 l[3] = {0, 0, 0};
			if (i < 8) {
				l[0] = ((i & 1) ? 1 : -1) * b->half[0] * 0.8f;
				l[1] = ((i & 2) ? 1 : -1) * b->half[1] * 0.8f;
				l[2] = ((i & 4) ? 1 : -1) * b->half[2] * 0.8f;
			}
			for (int r = 0; r < 3; r++)
				pts[i][r] = b->com[r] + R[r][0] * l[0] + R[r][1] * l[1] + R[r][2] * l[2];
			anyWet |= (CM_PointContents(pts[i]) & MASK_WATER) != 0;
		}
		if (anyWet) {
			/* the surface: up from the lowest wet point to where the water ends */
			f32 lo[3] = {b->com[0], b->com[1], b->com[2] - extZ};
			f32 top = b->com[2] + extZ + 2;
			f32 surf;
			{
				f32 a = lo[2], c = top, q[3] = {lo[0], lo[1], top};
				if (CM_PointContents(q) & MASK_WATER)
					surf = top + 64; /* (all under) */
				else {
					for (int it = 0; it < 10; it++) {
						q[2] = (a + c) * 0.5f;
						if (CM_PointContents(q) & MASK_WATER)
							a = q[2];
						else
							c = q[2];
					}
					surf = a;
				}
			}
			f32 vol = 8.0f * b->half[0] * b->half[1] * b->half[2] * 1.6387e-5f; /* cubic inches -> m^3 */
			f32 density = vol > 1e-6f ? b->mass / vol : 1000.0f;
			if (density < 50)
				density = 50;
			/* lift per unit submerged, as an acceleration of the whole body (g x water / body density) */
			f32 full = GRAVITY * (1000.0f / density);
			if (full > GRAVITY * 4)
				full = GRAVITY * 4;
			f32 subTotal = 0;
			for (int i = 0; i < 9; i++) {
				f32 depth = surf - pts[i][2];
				f32 s_ = depth / (2.0f * extZ) + 0.5f; /* (each point stands for a slab of the body's height) */
				s_ = s_ < 0 ? 0 : s_ > 1 ? 1 : s_;
				if (s_ <= 0)
					continue;
				f32 w_ = i < 8 ? 0.1f : 0.2f; /* (the centre a bigger share) */
				subTotal += s_ * w_;
				f32 J[3] = {0, 0, full * s_ * w_ * dt * b->mass};
				f32 r_[3] = {pts[i][0] - b->com[0], pts[i][1] - b->com[1], pts[i][2] - b->com[2]};
				f32 tq[3], dw[3];
				cross(r_, J, tq);
				apply_invI(b, R, tq, dw);
				b->v[2] += J[2] * b->invMass;
				for (int k = 0; k < 3; k++)
					b->w[k] += dw[k] * 0.5f;
			}
			/* drag: linear and angular, by how much of it is under */
			f32 lin = 1.0f - (dt * 2.5f * subTotal > 0.9f ? 0.9f : dt * 2.5f * subTotal);
			f32 ang = 1.0f - (dt * 3.0f * subTotal > 0.9f ? 0.9f : dt * 3.0f * subTotal);
			for (int k = 0; k < 3; k++) {
				b->v[k] *= lin;
				b->w[k] *= ang;
			}
			b->inWater = subTotal > 0;
		} else {
			b->inWater = 0;
		}
	}

	/* integrate */
	for (int k = 0; k < 3; k++)
		b->com[k] += b->v[k] * dt;
	f32 wq[4] = {b->w[0], b->w[1], b->w[2], 0};
	f32 *q = b->q;
	f32 dq[4] = {
		0.5f * dt * (wq[3] * q[0] + wq[0] * q[3] + wq[1] * q[2] - wq[2] * q[1]),
		0.5f * dt * (wq[3] * q[1] - wq[0] * q[2] + wq[1] * q[3] + wq[2] * q[0]),
		0.5f * dt * (wq[3] * q[2] + wq[0] * q[1] - wq[1] * q[0] + wq[2] * q[3]),
		0.5f * dt * (wq[3] * q[3] - wq[0] * q[0] - wq[1] * q[1] - wq[2] * q[2]),
	};
	f32 len = 0;
	for (int k = 0; k < 4; k++) {
		q[k] += dq[k];
		len += q[k] * q[k];
	}
	len = 1.0f / sqrtf(len);
	for (int k = 0; k < 4; k++)
		q[k] *= len;
	quat_to_R(q, R);

	/* contacts: trace each sample point from where it was to where it is */
	static const f32 zero[3] = {0, 0, 0};
	f32 push[3] = {0, 0, 0};
	int contacts = 0, embedded = 0;
	f32 hardest = 0, hitAt[3] = {0, 0, 0};
	int hitSurf = -1;
	f32 speed0 = sqrtf(dot(b->v, b->v));
	CM_SetIgnoreEnt(b->ent);
	CM_SetSkipBoxes(1);
	for (int i = 0; i < NUM_SAMPLES && !b->noCollide; i++) {
		f32 l[3], p[3];
		sample_point(b, i, l);
		for (int r = 0; r < 3; r++)
			p[r] = b->com[r] + R[r][0] * l[0] + R[r][1] * l[1] + R[r][2] * l[2];
		Trace tr;
		CM_BoxTrace(&tr, prev[i], p, zero, zero, MASK_PLAYERSOLID);
		if (tr.startsolid) {
			/* (inside another object's solid, a prop it was put in or pushed into: the world still
			 * stops it, so it can't sink through the floor while it works its way out) */
			CM_TraceWorld(&tr, prev[i], p, MASK_PLAYERSOLID);
			if (tr.startsolid) {
				embedded++;
				continue;
			}
		}
		if (tr.fraction >= 1.0f)
			continue;
		const f32 *n = tr.normal;
		f32 depth = (p[0] - tr.endpos[0]) * -n[0] + (p[1] - tr.endpos[1]) * -n[1] + (p[2] - tr.endpos[2]) * -n[2];
		if (depth < 0)
			depth = 0;
		for (int k = 0; k < 3; k++) {
			f32 c = n[k] * (depth + 0.05f);
			if (fabsf(c) > fabsf(push[k]))
				push[k] = c;
		}
		contacts++;
		/* impulse at the contact */
		f32 r[3] = {p[0] - b->com[0], p[1] - b->com[1], p[2] - b->com[2]};
		f32 wr[3], vp[3];
		cross(b->w, r, wr);
		for (int k = 0; k < 3; k++)
			vp[k] = b->v[k] + wr[k];
		f32 vn = dot(vp, n);
		if (vn >= 0)
			continue;
		if (-vn > hardest) {
			hardest = -vn;
			memcpy(hitAt, p, 12);
			hitSurf = tr.ent > 0 ? Weap_EntSurface(tr.ent) : tr.surface;
		}
		f32 rn[3], t1[3], t2[3];
		cross(r, n, rn);
		apply_invI(b, R, rn, t1);
		cross(t1, r, t2);
		f32 kn = b->invMass + dot(n, t2);
		f32 e = vn < -60.0f ? RESTITUTION : 0.0f; /* slow contacts don't bounce: lets things rest */
		f32 j = -(1 + e) * vn / kn;
		f32 imp[3];
		/* friction: remove tangential velocity up to the Coulomb limit */
		f32 vt[3] = {vp[0] - n[0] * vn, vp[1] - n[1] * vn, vp[2] - n[2] * vn};
		f32 vtl = sqrtf(dot(vt, vt));
		f32 jt = 0;
		if (vtl > 0.01f) {
			for (int k = 0; k < 3; k++)
				vt[k] /= vtl;
			cross(r, vt, rn);
			apply_invI(b, R, rn, t1);
			cross(t1, r, t2);
			f32 kt = b->invMass + dot(vt, t2);
			jt = vtl / kt;
			if (jt > FRICTION * j)
				jt = FRICTION * j;
		}
		for (int k = 0; k < 3; k++)
			imp[k] = n[k] * j - (vtl > 0.01f ? vt[k] * jt : 0);
		for (int k = 0; k < 3; k++)
			b->v[k] += imp[k] * b->invMass;
		f32 ri[3], dw[3];
		cross(r, imp, ri);
		apply_invI(b, R, ri, dw);
		for (int k = 0; k < 3; k++)
			b->w[k] += dw[k];
	}
	CM_SetSkipBoxes(0);
	CM_SetIgnoreEnt(-1);
	/* impact damage to itself (CBreakableProp::VPhysicsCollision -> CalculateDefaultPhysicsDamage): the
	 * speed it lost hitting something, squared and scaled by its physdamagescale (0.1 by default), read
	 * from its damage table: the "glass" one for small glass and pottery (bottles break on a throw or a
	 * fall off a table), the NPC one otherwise. Not while it's held, nor with "don't take physics damage" */
	if (contacts && !holding && physClock >= b->nextImpact) {
		f32 dv = speed0 - sqrtf(dot(b->v, b->v));
		if (dv > 20.0f)
			impact_damage(b, dv, hitAt);
	}
	/* impact sounds, like Source's physics impact: the prop's own surface (hard over ~70 in/s, soft
	 * above ~25), a little of the surface it hit too, louder the harder it hits */
	if (hardest > 25.0f && physClock >= b->nextImpact) {
		f32 vol = hardest > 300.0f ? 1.0f : hardest / 300.0f;
		if (vol < 0.15f)
			vol = 0.15f;
		int kind = hardest > 70.0f ? 0 : 1;
		Weap_SurfaceSound(Weap_EntSurface(b->ent), kind, hitAt, vol);
		if (kind == 0 && hitSurf >= 0 && hardest > 150.0f)
			Weap_SurfaceSound(hitSurf, 1, hitAt, vol * 0.6f);
		b->nextImpact = physClock + 0.12f;
	}
	if (embedded > NUM_SAMPLES / 2 && !holding) {
		/* stuck inside something (spawned or shoved into it): hold still rather than fall through */
		for (int k = 0; k < 3; k++)
			b->com[k] -= b->v[k] * dt;
		memset(b->v, 0, 12);
		memset(b->w, 0, 12);
		b->awake = 0;
		sync(b);
		return;
	}
	for (int k = 0; k < 3; k++)
		b->com[k] += push[k];

	/* damping and sleep */
	for (int k = 0; k < 3; k++) {
		b->v[k] *= contacts ? 0.995f : 0.9995f;
		b->w[k] *= contacts ? 0.97f : 0.995f;
	}
	f32 sv = dot(b->v, b->v), sw = dot(b->w, b->w);
	if (!holding && contacts && sv < 6 * 6 && sw < 0.15f * 0.15f) {
		b->still += dt;
		if (b->still > SLEEP_TIME) {
			b->awake = 0;
			memset(b->v, 0, 12);
			memset(b->w, 0, 12);
		}
	} else {
		b->still = 0;
	}
	npc_contacts(b, holding);
	/* fell out of the world: stop simulating it */
	if (b->com[2] < -16384) {
		b->awake = 0;
		ent_of(b)->flags |= EF_KILLED;
	}
	sync(b);
}

/* the player walking into a prop shoves it */
static void player_push(Body *b)
{
	Entity *e = ent_of(b);
	f32 pm[3], px[3];
	Player_Bounds(player, pm, px);
	f32 pv = sqrtf(wish[0] * wish[0] + wish[1] * wish[1]);
	if (pv < 20)
		return;
	f32 r = b->half[0] > b->half[1] ? b->half[0] : b->half[1];
	f32 d[3] = {b->com[0] - player->origin[0], b->com[1] - player->origin[1], 0};
	f32 dist = sqrtf(d[0] * d[0] + d[1] * d[1]);
	if (dist > r + px[0] + 6 || b->com[2] + b->half[2] < player->origin[2] + 2 || b->com[2] - b->half[2] > player->origin[2] + px[2])
		return;
	f32 dir[2] = {wish[0] / pv, wish[1] / pv};
	if (dist > 0.1f && (d[0] * dir[0] + d[1] * dir[1]) / dist < 0.3f)
		return; /* not walking towards it */
	f32 speed = pv * (b->mass < 40 ? 1.0f : 40.0f / b->mass);
	if (speed < 4)
		return;
	(void)e;
	b->awake = 1;
	b->still = 0;
	f32 cur = b->v[0] * dir[0] + b->v[1] * dir[1];
	if (cur < speed) {
		b->v[0] += dir[0] * (speed - cur);
		b->v[1] += dir[1] * (speed - cur);
	}
}

void Phys_Tick(f32 dt)
{
	physClock += dt;
	CM_SetPhysicsTrace(1); /* (physics objects meet func_clip_vphysics) */
	for (u32 i = 0; i < numBodies; i++) {
		Body *b = &bodies[i];
		Entity *e = ent_of(b);
		if (e->flags & (EF_KILLED | EF_DISABLED) || b->frozen)
			continue;
		player_push(b);
		if (b->awake)
			step_body(b, dt);
	}
	solve_constraints(dt);
	CM_SetPhysicsTrace(0);
	if (held >= 0) {
		Body *b = &bodies[held];
		f32 d[3] = {b->com[0] - viewEye[0], b->com[1] - viewEye[1], b->com[2] - viewEye[2]};
		if (dot(d, d) > (holdDist + 120.0f) * (holdDist + 120.0f) || (ent_of(b)->flags & EF_KILLED))
			Phys_Drop(0); /* snagged on something */
	}
}

int Phys_TryPickup(s32 ent)
{
	if (held >= 0) {
		Phys_Drop(0);
		return 1;
	}
	Body *b = body_of(ent);
	if (!b || b->frozen || b->mass > MAX_LIFT_MASS)
		return 0;
	holdDist = HOLD_DIST;
	held = (s32)(b - bodies);
	heldByGun = 0;
	b->awake = 1;
	b->still = 0;
	/* (CPhysBox fires OnPhysGunPickup for any pickup; props fire OnPlayerPickup for the player's hands) */
	Ent_FireOutput(b->ent, ent_of(b)->mdl < 0 ? "onphysgunpickup" : "onplayerpickup", 0);
	return 1;
}

int Phys_Holding(void) { return held >= 0; }

void Phys_Drop(int throwIt)
{
	if (held < 0)
		return;
	Body *b = &bodies[held];
	held = -1;
	Ent_FireOutput(b->ent, heldByGun || ent_of(b)->mdl < 0 ? "onphysgundrop" : "onplayerdrop", 0);
	if (throwIt) {
		/* Source's player throw: a fixed impulse, so light things fly further than heavy ones */
		f32 speed = 1000.0f / (b->mass < 2 ? 2 : b->mass) * 3.0f;
		if (speed > 600)
			speed = 600;
		if (speed < 150)
			speed = 150;
		for (int k = 0; k < 3; k++)
			b->v[k] = viewFwd[k] * speed;
	}
	b->awake = 1;
}

int Phys_IsBody(s32 ent) { return body_of(ent) != NULL; }

int Phys_IsAwake(s32 ent)
{
	Body *b = body_of(ent);
	return b && b->awake && !b->frozen;
}

/* (tests) how many are being simulated */
int Phys_AwakeCount(void)
{
	int n = 0;
	for (u32 i = 0; i < numBodies; i++)
		n += bodies[i].awake && !bodies[i].frozen;
	return n;
}

/* every simulated object's entity (the triggers that physics objects touch: SF_TRIGGER_ALLOW_PHYSICS) */
int Phys_BodyEnts(s32 *out, int max)
{
	int n = 0;
	for (u32 i = 0; i < numBodies && n < max; i++)
		if (bodies[i].ent > 0)
			out[n++] = bodies[i].ent;
	return n;
}

f32 Phys_Mass(s32 ent)
{
	Body *b = body_of(ent);
	return b ? b->mass : 0;
}

int Phys_HeldEnt(void) { return held >= 0 ? bodies[held].ent : -1; }

int Phys_GrabFar(s32 ent, f32 maxMass)
{
	Body *b = body_of(ent);
	if (!b || b->mass > maxMass)
		return 0;
	if (b->frozen)
		b->frozen = 0; /* the gravity gun pulls things loose, as on PC (motion-disabled props stay put) */
	if (ent_of(b)->mdl >= 0 && (ent_of(b)->spawnflags & 8))
		return 0;
	held = (s32)(b - bodies);
	heldByGun = 1;
	Ent_FireOutput(b->ent, "onphysgunpickup", 0);
	holdDist = 40.0f;
	b->awake = 1;
	b->still = 0;
	return 1;
}

void Phys_Pull(s32 ent, const f32 eye[3], f32 strength)
{
	Body *b = body_of(ent);
	if (!b || b->frozen)
		return;
	f32 d[3] = {eye[0] - b->com[0], eye[1] - b->com[1], eye[2] - b->com[2]};
	f32 l = sqrtf(dot(d, d));
	if (l < 1)
		return;
	b->awake = 1;
	b->still = 0;
	for (int k = 0; k < 3; k++)
		b->v[k] += d[k] / l * strength * b->invMass * 30.0f;
}

void Phys_Launch(f32 impulse)
{
	if (held < 0)
		return;
	Body *b = &bodies[held];
	held = -1;
	Ent_FireOutput(b->ent, "onphysgunpunt", 0);
	f32 sp = impulse * b->invMass;
	if (sp > 2000)
		sp = 2000;
	for (int k = 0; k < 3; k++)
		b->v[k] = viewFwd[k] * sp;
	b->w[0] += 3.0f;
	b->awake = 1;
	b->still = 0;
}

void Phys_Impulse(s32 ent, const f32 point[3], const f32 imp[3])
{
	Body *b = body_of(ent);
	if (!b || b->frozen)
		return;
	f32 R[3][3];
	quat_to_R(b->q, R);
	f32 r[3] = {point[0] - b->com[0], point[1] - b->com[1], point[2] - b->com[2]};
	f32 tq[3], dw[3];
	cross(r, imp, tq);
	apply_invI(b, R, tq, dw);
	for (int k = 0; k < 3; k++) {
		b->v[k] += imp[k] * b->invMass;
		b->w[k] += dw[k];
	}
	f32 sp = sqrtf(dot(b->v, b->v));
	if (sp > 2000.0f)
		for (int k = 0; k < 3; k++)
			b->v[k] *= 2000.0f / sp;
	for (int k = 0; k < 3; k++) {
		if (b->w[k] > 30)
			b->w[k] = 30;
		if (b->w[k] < -30)
			b->w[k] = -30;
	}
	b->awake = 1;
	b->still = 0;
}

void Phys_Explosion(const f32 at[3], f32 radius, f32 force)
{
	for (u32 i = 0; i < numBodies; i++) {
		Body *b = &bodies[i];
		if (b->frozen || (ent_of(b)->flags & (EF_KILLED | EF_DISABLED)))
			continue;
		f32 d[3] = {b->com[0] - at[0], b->com[1] - at[1], b->com[2] - at[2]};
		f32 l = sqrtf(dot(d, d));
		if (l > radius)
			continue;
		f32 s = force * (1 - l / radius) / (l > 1 ? l : 1);
		f32 imp[3] = {d[0] * s, d[1] * s, d[2] * s + force * 0.3f * (1 - l / radius)};
		Phys_Impulse(b->ent, b->com, imp);
	}
}

void Phys_GetWish(f32 out[2])
{
	out[0] = wish[0];
	out[1] = wish[1];
}

void Phys_SetWish(f32 vx, f32 vy)
{
	wish[0] = vx;
	wish[1] = vy;
}

void Phys_SetView(const f32 eye[3], const f32 fwd[3])
{
	memcpy(viewEye, eye, 12);
	memcpy(viewFwd, fwd, 12);
}

/* move a prop's centre of mass to pos, at rest (tests, and scripts that place props) */
void Phys_Teleport(s32 ent, const f32 pos[3])
{
	Body *b = body_of(ent);
	if (!b)
		return;
	memcpy(b->com, pos, 12);
	memset(b->v, 0, 12);
	memset(b->w, 0, 12);
	b->awake = 1;
	b->still = 0;
	sync(b);
}

/* its centre of mass (world) */
const f32 *Phys_Com(s32 ent)
{
	static const f32 z[3] = {0, 0, 0};
	Body *b = body_of(ent);
	return b ? b->com : z;
}

/* puts the object with its origin at pos, turned to angles (a trigger_teleport's destination) */
void Phys_SetAngles(s32 ent, const f32 angles[3])
{
	Body *b = body_of(ent);
	if (!b)
		return;
	Entity *e = ent_of(b);
	f32 o[3];
	memcpy(o, e->origin, 12); /* (keeps the origin where it is) */
	f32 R[3][3];
	Ent_AngleMatrix(angles, R);
	R_to_quat(R, b->q);
	for (int r = 0; r < 3; r++)
		b->com[r] = o[r] + R[r][0] * b->center[0] + R[r][1] * b->center[1] + R[r][2] * b->center[2];
	memset(b->v, 0, 12);
	memset(b->w, 0, 12);
	b->awake = 1;
	b->still = 0;
	sync(b);
}

/* ------------------------------------------------------------------ constraints
 * Source's phys_spring (physobj.cpp), phys_lengthconstraint / phys_ballsocket / phys_constraint
 * (physconstraint.cpp): what ties a plug to its cable's anchor, hangs a lamp from a chain, welds a board to a
 * wall. Each end is a body (its own frame), another entity (a func_brush, a moving train: follows it) or the
 * world. Springs push with constant * stretch and damping * the ends' closing speed (vphysics' units, kg and
 * inches, so a = k x / m); the others are solved as position limits plus the impulse that stops the ends
 * separating, a few passes a tick. */

enum { CT_SPRING, CT_LENGTH, CT_BALL, CT_FIXED, CT_MOTOR };
#define MAX_CONSTRAINTS 128
typedef struct {
	s32 ent;           /* the constraint entity (TurnOn / TurnOff / Break) */
	u8 type, on, broken, onlyStretch;
	s32 e[2];          /* attached entities: -1 = the world */
	s32 body[2];       /* their bodies, -1 if none */
	f32 local[2][3];   /* each end in its entity's frame (world: the point) */
	f32 len, minLen, k, damp;
	f32 relR[3][3], relP[3]; /* CT_FIXED: body 1 in body 0's (or the entity's) frame */
	f32 forceLimit;
	/* CT_MOTOR (CPhysMotor): world axis, target speed (m_flSpeed, deg/s), the controller's speed now
	 * (m_motor.m_speed), the spin-up rate and the most it can turn the object up a second */
	f32 axis[3], target, speed, rate, maxAcc;
} Constraint;
static Constraint cons[MAX_CONSTRAINTS];
static int numCons;

static s32 body_index(s32 ent)
{
	Body *b = body_of(ent);
	return b ? (s32)(b - bodies) : -1;
}

/* the frame (rotation, origin) an end's local point is in */
static void end_frame(const Constraint *c, int i, f32 R[3][3], f32 o[3])
{
	if (c->body[i] >= 0) {
		const Body *b = &bodies[c->body[i]];
		quat_to_R(b->q, R);
		memcpy(o, b->com, 12);
	} else if (c->e[i] > 0) {
		Entity *e = Ent_List(NULL) + c->e[i];
		memcpy(R, e->R, sizeof(f32) * 9);
		memcpy(o, e->origin, 12);
	} else {
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 3; k++)
				R[r][k] = r == k;
		o[0] = o[1] = o[2] = 0;
	}
}

static void end_world(const Constraint *c, int i, f32 out[3])
{
	f32 R[3][3], o[3];
	end_frame(c, i, R, o);
	for (int r = 0; r < 3; r++)
		out[r] = o[r] + R[r][0] * c->local[i][0] + R[r][1] * c->local[i][1] + R[r][2] * c->local[i][2];
}

static void set_end(Constraint *c, int i, const f32 world[3])
{
	f32 R[3][3], o[3];
	end_frame(c, i, R, o);
	f32 d[3] = {world[0] - o[0], world[1] - o[1], world[2] - o[2]};
	for (int k = 0; k < 3; k++)
		c->local[i][k] = R[0][k] * d[0] + R[1][k] * d[1] + R[2][k] * d[2];
}

static void end_velocity(const Constraint *c, int i, const f32 p[3], f32 v[3])
{
	v[0] = v[1] = v[2] = 0;
	if (c->body[i] < 0)
		return;
	const Body *b = &bodies[c->body[i]];
	f32 r[3] = {p[0] - b->com[0], p[1] - b->com[1], p[2] - b->com[2]}, wr[3];
	cross(b->w, r, wr);
	for (int k = 0; k < 3; k++)
		v[k] = b->v[k] + wr[k];
}

static void impulse_at(Body *b, const f32 p[3], const f32 J[3])
{
	if (b->frozen)
		return;
	f32 R[3][3], r[3] = {p[0] - b->com[0], p[1] - b->com[1], p[2] - b->com[2]}, rj[3], dw[3];
	quat_to_R(b->q, R);
	cross(r, J, rj);
	apply_invI(b, R, rj, dw);
	for (int k = 0; k < 3; k++) {
		b->v[k] += J[k] * b->invMass;
		b->w[k] += dw[k];
	}
	b->awake = 1;
	b->still = 0;
}

/* the inverse mass an end shows along n at p (translation and turning) */
static f32 inv_mass_along(const Body *b, const f32 p[3], const f32 n[3])
{
	if (!b || b->frozen)
		return 0;
	f32 R[3][3], r[3] = {p[0] - b->com[0], p[1] - b->com[1], p[2] - b->com[2]}, rn[3], t[3], u[3];
	quat_to_R(b->q, R);
	cross(r, n, rn);
	apply_invI(b, R, rn, t);
	cross(t, r, u);
	return b->invMass + dot(u, n);
}

static const char *kvs(s32 ent, const char *k)
{
	const char *v = Ent_KV(ent, k);
	return v ? v : "";
}

static int parse3(const char *s, f32 out[3])
{
	return s && sscanf(s, "%f %f %f", &out[0], &out[1], &out[2]) == 3;
}

static void add_ends(Constraint *c, s32 i, const char *a1, const char *a2)
{
	Entity *ents = Ent_List(NULL);
	for (int k = 0; k < 2; k++) {
		const char *nm = k ? a2 : a1;
		c->e[k] = nm && *nm ? Ent_FindActor(nm) : -1;
		c->body[k] = c->e[k] > 0 ? body_index(c->e[k]) : -1;
		if (c->e[k] > 0 && c->body[k] < 0 && ents[c->e[k]].parent > 0 && body_index(ents[c->e[k]].parent) >= 0) {
			c->e[k] = ents[c->e[k]].parent;
			c->body[k] = body_index(c->e[k]);
		}
	}
	(void)i;
}

/* a hinge (phys_hinge, or a phys_motor's own): two ball joints on the axis, at the pivot and a little along
 * it, so the object can only turn about the axis */
static void add_hinge(s32 ent, const char *a1, const char *a2, const f32 pivot[3], const f32 dir[3], int on, int nocollide)
{
	for (int h = 0; h < 2 && numCons < MAX_CONSTRAINTS; h++) {
		Constraint *c = &cons[numCons];
		memset(c, 0, sizeof(*c));
		c->ent = ent;
		c->type = CT_BALL;
		add_ends(c, ent, a1, a2);
		if (c->body[0] < 0 && c->body[1] < 0)
			return;
		f32 p[3] = {pivot[0] + dir[0] * 24.0f * h, pivot[1] + dir[1] * 24.0f * h, pivot[2] + dir[2] * 24.0f * h};
		set_end(c, 0, p);
		set_end(c, 1, p);
		c->on = (u8)on;
		c->onlyStretch = 2; /* (part of a hinge) */
		if (nocollide && c->e[1] < 0 && c->body[0] >= 0)
			bodies[c->body[0]].noCollide = 1;
		numCons++;
	}
}

/* UTIL_SnapDirectionToAxis: an axis within a hair of a world axis is that axis */
static void snap_axis(f32 d[3])
{
	for (int k = 0; k < 3; k++)
		if (fabsf(d[k]) > 1.0f - 0.002f) {
			for (int j = 0; j < 3; j++)
				d[j] = j == k ? (d[k] > 0 ? 1.0f : -1.0f) : 0;
			return;
		}
}

static void init_constraints(void)
{
	numCons = 0;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n && numCons < MAX_CONSTRAINTS; i++) {
		const char *cn = ents[i].classname;
		int type;
		if (!strcmp(cn, "phys_hinge")) {
			/* (ent.c turns the upright ones on brushes hinged to the world: turnstiles, carousels) */
			const char *a1 = kvs((s32)i, "attach1"), *a2 = kvs((s32)i, "attach2");
			s32 o = *a1 ? Ent_FindActor(a1) : -1;
			if (o > 0 && ents[o].hinged)
				continue;
			f32 ax[3], d[3];
			if (!parse3(kvs((s32)i, "hingeaxis"), ax))
				continue;
			for (int k = 0; k < 3; k++)
				d[k] = ax[k] - ents[i].origin[k];
			f32 l = sqrtf(dot(d, d));
			if (l < 0.01f)
				continue;
			for (int k = 0; k < 3; k++)
				d[k] /= l;
			add_hinge((s32)i, a1, a2, ents[i].origin, d, !(ents[i].spawnflags & 4), (ents[i].spawnflags & 1) != 0);
			continue;
		}
		if (!strcmp(cn, "phys_motor")) {
			/* CPhysMotor::Spawn / Activate: the axis from the motor to its "axis" point; torque enough to spin
			 * the object up at speed/spinup (plus addangaccel) times inertiafactor; flag 4 hinges it to the world
			 * there; flag 1 starts it on */
			Constraint *c = &cons[numCons];
			memset(c, 0, sizeof(*c));
			c->ent = (s32)i;
			c->type = CT_MOTOR;
			add_ends(c, (s32)i, kvs((s32)i, "attach1"), "");
			if (c->body[0] < 0)
				continue;
			f32 ax[3];
			if (!parse3(kvs((s32)i, "axis"), ax))
				continue;
			for (int k = 0; k < 3; k++)
				c->axis[k] = ax[k] - ents[i].origin[k];
			f32 l = sqrtf(dot(c->axis, c->axis));
			if (l <= 1.0f)
				continue; /* (no valid axis helper: it removes itself) */
			for (int k = 0; k < 3; k++)
				c->axis[k] /= l;
			snap_axis(c->axis);
			c->target = (f32)atof(kvs((s32)i, "speed"));
			f32 spin = (f32)atof(kvs((s32)i, "spinup")), add = (f32)atof(kvs((s32)i, "addangaccel"));
			const char *inf = Ent_KV((s32)i, "inertiafactor");
			f32 fac = inf && *inf ? (f32)atof(inf) : 1.0f;
			c->rate = spin > 0 ? fabsf(c->target / spin) : fabsf(c->target);
			c->maxAcc = fac * (c->rate + add);
			c->on = (ents[i].spawnflags & 1) != 0;
			c->speed = 0;
			numCons++;
			if (ents[i].spawnflags & 4)
				add_hinge((s32)i, kvs((s32)i, "attach1"), "", ents[i].origin, c->axis, 1, (ents[i].spawnflags & 2) != 0);
			else if ((ents[i].spawnflags & 2) && c->body[0] >= 0)
				bodies[c->body[0]].noCollide = 0; /* (SF_MOTOR_NOCOLLIDE only goes with its own hinge) */
			continue;
		}
		if (!strcmp(cn, "phys_spring"))
			type = CT_SPRING;
		else if (!strcmp(cn, "phys_lengthconstraint"))
			type = CT_LENGTH;
		else if (!strcmp(cn, "phys_ballsocket"))
			type = CT_BALL;
		else if (!strcmp(cn, "phys_constraint"))
			type = CT_FIXED;
		else
			continue;
		Constraint *c = &cons[numCons];
		memset(c, 0, sizeof(*c));
		c->ent = (s32)i;
		c->type = (u8)type;
		for (int k = 0; k < 2; k++) {
			const char *nm = kvs((s32)i, k ? "attach2" : "attach1");
			c->e[k] = *nm ? Ent_FindActor(nm) : -1;
			c->body[k] = c->e[k] > 0 ? body_index(c->e[k]) : -1;
			/* attached to something that isn't simulated but rides on something that is (a plug's model on its
			 * physbox): the spring holds that */
			if (c->e[k] > 0 && c->body[k] < 0 && ents[c->e[k]].parent > 0 && body_index(ents[c->e[k]].parent) >= 0) {
				c->e[k] = ents[c->e[k]].parent;
				c->body[k] = body_index(c->e[k]);
			}
		}
		if (c->body[0] < 0 && c->body[1] < 0)
			continue; /* nothing simulated on either end */
		f32 p0[3], p1[3];
		memcpy(p0, ents[i].origin, 12);
		memcpy(p1, ents[i].origin, 12);
		if (type == CT_SPRING) {
			parse3(kvs((s32)i, "springaxis"), p1);
			/* (Activate: the end point nearer attach2's object goes on it) */
			const f32 *o2 = c->e[1] > 0 ? ents[c->e[1]].origin : NULL;
			if (o2) {
				f32 a[3] = {p0[0] - o2[0], p0[1] - o2[1], p0[2] - o2[2]}, b[3] = {p1[0] - o2[0], p1[1] - o2[1], p1[2] - o2[2]};
				if (dot(a, a) < dot(b, b)) {
					f32 t[3];
					memcpy(t, p0, 12);
					memcpy(p0, p1, 12);
					memcpy(p1, t, 12);
				}
			}
			c->k = (f32)atof(kvs((s32)i, "constant"));
			c->damp = (f32)atof(kvs((s32)i, "damping"));
			c->len = (f32)atof(kvs((s32)i, "length"));
			f32 d[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
			if (c->len <= 0)
				c->len = sqrtf(dot(d, d));
			c->onlyStretch = (ents[i].spawnflags & 1) != 0; /* SF_SPRING_ONLYSTRETCH */
		} else if (type == CT_LENGTH) {
			parse3(kvs((s32)i, "attachpoint"), p1);
			f32 d[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
			c->len = sqrtf(dot(d, d)) + (f32)atof(kvs((s32)i, "addlength"));
			c->minLen = (ents[i].spawnflags & 2) ? c->len : (f32)atof(kvs((s32)i, "minlength")); /* SF_LENGTH_RIGID */
		}
		set_end(c, 0, p0);
		set_end(c, 1, p1);
		if (type == CT_FIXED) {
			/* body 1 (or 0 if 1 isn't one) held where it is relative to the other end's frame */
			int mov = c->body[1] >= 0 ? 1 : 0, ref = 1 - mov;
			f32 Rr[3][3], orr[3], Rm[3][3];
			end_frame(c, ref, Rr, orr);
			quat_to_R(bodies[c->body[mov]].q, Rm);
			f32 d[3] = {bodies[c->body[mov]].com[0] - orr[0], bodies[c->body[mov]].com[1] - orr[1], bodies[c->body[mov]].com[2] - orr[2]};
			for (int r = 0; r < 3; r++) {
				c->relP[r] = Rr[0][r] * d[0] + Rr[1][r] * d[1] + Rr[2][r] * d[2];
				for (int k = 0; k < 3; k++)
					c->relR[r][k] = Rr[0][r] * Rm[0][k] + Rr[1][r] * Rm[1][k] + Rr[2][r] * Rm[2][k];
			}
		}
		c->forceLimit = (f32)atof(kvs((s32)i, "forcelimit"));
		c->on = !(ents[i].spawnflags & 4); /* SF_CONSTRAINT_START_INACTIVE */
		if (type == CT_SPRING)
			c->on = 1;
		numCons++;
	}
}

static void break_constraint(Constraint *c)
{
	if (c->broken)
		return;
	c->broken = 1;
	c->on = 0;
	Ent_FireOutput(c->ent, "onbreak", 0);
}

static void solve_one(Constraint *c, f32 dt, int pass)
{
	Body *b0 = c->body[0] >= 0 ? &bodies[c->body[0]] : NULL, *b1 = c->body[1] >= 0 ? &bodies[c->body[1]] : NULL;
	if ((b0 && (ent_of(b0)->flags & EF_KILLED)) || (b1 && (ent_of(b1)->flags & EF_KILLED))) {
		break_constraint(c);
		return;
	}
	/* (both ends at rest: nothing to hold - the joint mustn't keep waking them) */
	if ((!b0 || !b0->awake || b0->frozen) && (!b1 || !b1->awake || b1->frozen) && c->type != CT_MOTOR)
		return;
	if (c->type == CT_MOTOR) {
		/* Think: the controller's speed approaches the target at the spin-up rate; Simulate: torque toward
		 * that spin about the axis, no more than the motor has */
		if (pass || !b0 || b0->frozen)
			return;
		f32 st = c->rate * dt, d = c->target - c->speed;
		c->speed += fabsf(d) <= st ? d : (d > 0 ? st : -st);
		if (c->speed == 0)
			return;
		const f32 k = (f32)M_PI / 180.0f;
		f32 cur = dot(b0->w, c->axis), want = c->speed * k, acc = c->maxAcc * k * dt;
		f32 dw = want - cur;
		if (c->maxAcc > 0)
			dw = dw > acc ? acc : dw < -acc ? -acc : dw;
		for (int r = 0; r < 3; r++)
			b0->w[r] += c->axis[r] * dw;
		b0->awake = 1;
		b0->still = 0;
		return;
	}
	if (c->type == CT_FIXED) {
		if (pass)
			return;
		int mov = b1 ? 1 : 0;
		Body *m = mov ? b1 : b0;
		if (m->frozen || (s32)(m - bodies) == held)
			return;
		f32 Rr[3][3], orr[3], Rm[3][3];
		end_frame(c, 1 - mov, Rr, orr);
		for (int r = 0; r < 3; r++) {
			m->com[r] = orr[r] + Rr[r][0] * c->relP[0] + Rr[r][1] * c->relP[1] + Rr[r][2] * c->relP[2];
			for (int k = 0; k < 3; k++)
				Rm[r][k] = Rr[r][0] * c->relR[0][k] + Rr[r][1] * c->relR[1][k] + Rr[r][2] * c->relR[2][k];
		}
		R_to_quat(Rm, m->q);
		Body *o = mov ? b0 : b1;
		for (int k = 0; k < 3; k++) {
			m->v[k] = o ? o->v[k] : 0;
			m->w[k] = o ? o->w[k] : 0;
		}
		sync(m);
		return;
	}
	f32 p0[3], p1[3];
	end_world(c, 0, p0);
	end_world(c, 1, p1);
	f32 d[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
	f32 len = sqrtf(dot(d, d));
	if (len < 1e-3f)
		return;
	f32 n[3] = {d[0] / len, d[1] / len, d[2] / len};
	f32 v0[3], v1[3];
	end_velocity(c, 0, p0, v0);
	end_velocity(c, 1, p1, v1);
	f32 rel = (v1[0] - v0[0]) * n[0] + (v1[1] - v0[1]) * n[1] + (v1[2] - v0[2]) * n[2];
	if (c->type == CT_SPRING) {
		if (pass)
			return;
		f32 stretch = len - c->len;
		if (c->onlyStretch && stretch <= 0)
			return;
		f32 F = c->k * stretch + c->damp * rel; /* pulls the ends together when stretched */
		f32 J[3] = {n[0] * F * dt, n[1] * F * dt, n[2] * F * dt}, mJ[3] = {-J[0], -J[1], -J[2]};
		if (b0)
			impulse_at(b0, p0, J);
		if (b1)
			impulse_at(b1, p1, mJ);
		return;
	}
	f32 target = c->type == CT_BALL ? 0 : c->len;
	f32 err = len - target;
	if (c->type == CT_LENGTH && err < 0 && (c->minLen <= 0 || len >= c->minLen))
		return; /* slack: a rope only pulls */
	if (c->type == CT_LENGTH && err < 0)
		err = len - c->minLen;
	f32 w0 = inv_mass_along(b0, p0, n), w1 = inv_mass_along(b1, p1, n), W = w0 + w1;
	if (W <= 0)
		return;
	if (fabsf(err) < 0.05f && fabsf(rel) < 0.5f)
		return; /* (held: leave it be, so it can go to sleep) */
	/* the impulse that stops them moving apart (or together, if too close), then move them back in line */
	f32 lambda = 0;
	if ((err > 0 && rel > 0) || (err < 0 && rel < 0))
		lambda = rel / W;
	if (c->forceLimit > 0 && fabsf(lambda) / (dt > 0 ? dt : 1) > c->forceLimit * 100.0f) {
		break_constraint(c);
		return;
	}
	f32 J[3] = {n[0] * lambda, n[1] * lambda, n[2] * lambda}, mJ[3] = {-J[0], -J[1], -J[2]};
	if (b0)
		impulse_at(b0, p0, J);
	if (b1)
		impulse_at(b1, p1, mJ);
	/* move them back in line: a position-level push at each end, turning the body as well as shifting it
	 * (so two joints on an axis, a hinge, keep the axis straight instead of sagging) */
	f32 P = err * 0.6f / W;
	for (int k = 0; k < 2; k++) {
		Body *b = k ? b1 : b0;
		f32 w = k ? w1 : w0;
		if (!b || b->frozen || w <= 0)
			continue;
		f32 J[3] = {n[0] * P * (k ? -1.0f : 1.0f), n[1] * P * (k ? -1.0f : 1.0f), n[2] * P * (k ? -1.0f : 1.0f)};
		const f32 *p = k ? p1 : p0;
		f32 R[3][3], r[3] = {p[0] - b->com[0], p[1] - b->com[1], p[2] - b->com[2]}, rj[3], dth[3];
		quat_to_R(b->q, R);
		cross(r, J, rj);
		apply_invI(b, R, rj, dth);
		for (int r2 = 0; r2 < 3; r2++)
			b->com[r2] += J[r2] * b->invMass;
		f32 *q = b->q, dq[4] = {
			0.5f * (dth[0] * q[3] + dth[1] * q[2] - dth[2] * q[1]),
			0.5f * (-dth[0] * q[2] + dth[1] * q[3] + dth[2] * q[0]),
			0.5f * (dth[0] * q[1] - dth[1] * q[0] + dth[2] * q[3]),
			0.5f * (-dth[0] * q[0] - dth[1] * q[1] - dth[2] * q[2]),
		};
		f32 ql = 0;
		for (int i = 0; i < 4; i++) {
			q[i] += dq[i];
			ql += q[i] * q[i];
		}
		ql = 1.0f / sqrtf(ql);
		for (int i = 0; i < 4; i++)
			q[i] *= ql;
		sync(b);
	}
}

static void solve_constraints(f32 dt)
{
	for (int pass = 0; pass < 12; pass++)
		for (int i = 0; i < numCons; i++)
			if (cons[i].on && !cons[i].broken)
				solve_one(&cons[i], dt, pass);
}

int Phys_ConstraintInput(s32 ent, const char *in)
{
	int r = 0;
	for (int i = 0; i < numCons; i++) {
		Constraint *c = &cons[i];
		if (c->ent != ent)
			continue;
		if (c->type == CT_MOTOR) {
			/* (CPhysMotor: TurnOn spins it up again, TurnOff lets it coast, SetSpeed the new target) */
			if (!strcasecmp(in, "turnon")) {
				c->on = 1;
				if (c->body[0] >= 0)
					bodies[c->body[0]].awake = 1;
				r = 1;
			} else if (!strcasecmp(in, "turnoff")) {
				c->speed = 0;
				c->on = 0;
				r = 1;
			} else if (!strcasecmp(in, "setspeed"))
				r = 2;
			continue;
		}
		if (c->onlyStretch == 2 && strcmp(Ent_List(NULL)[ent].classname, "phys_hinge"))
			continue; /* (a motor's own hinge stays) */
		if (!strcasecmp(in, "turnon")) {
			if (!c->broken) {
				c->on = 1;
				for (int k = 0; k < 2; k++)
					if (c->body[k] >= 0) {
						bodies[c->body[k]].awake = 1;
						bodies[c->body[k]].still = 0;
					}
			}
		} else if (!strcasecmp(in, "turnoff")) {
			c->on = 0;
		} else if (!strcasecmp(in, "break") || !strcasecmp(in, "constraintbroken")) {
			break_constraint(c);
		} else if (!strcasecmp(in, "setspringconstant") || !strcasecmp(in, "setspringlength") ||
		           !strcasecmp(in, "setspringdamping")) {
			return 2; /* (the value comes with the parameter: Phys_ConstraintValue) */
		} else {
			continue;
		}
		if (!r)
			r = 1;
	}
	return r;
}

void Phys_ConstraintValue(s32 ent, const char *in, f32 v)
{
	for (int i = 0; i < numCons; i++)
		if (cons[i].ent == ent && cons[i].type == CT_MOTOR) {
			if (!strcasecmp(in, "setspeed") && cons[i].target != v) {
				Constraint *c = &cons[i];
				c->target = v;
				/* (CalculateAcceleration again) */
				const char *sp = Ent_KV(c->ent, "spinup");
				f32 spin = sp ? (f32)atof(sp) : 0;
				c->rate = spin > 0 ? fabsf(v / spin) : fabsf(v);
				if (c->body[0] >= 0)
					bodies[c->body[0]].awake = 1;
			}
		} else if (cons[i].ent == ent) {
			if (!strcasecmp(in, "setspringconstant"))
				cons[i].k = v;
			else if (!strcasecmp(in, "setspringlength"))
				cons[i].len = v;
			else if (!strcasecmp(in, "setspringdamping"))
				cons[i].damp = v;
			for (int k = 0; k < 2; k++)
				if (cons[i].body[k] >= 0)
					bodies[cons[i].body[k]].awake = 1;
		}
}

int Phys_ConstraintCount(void) { return numCons; }

static int constrained(s32 ent)
{
	for (int i = 0; i < numCons; i++)
		for (int k = 0; k < 2; k++)
			if (cons[i].body[k] >= 0 && bodies[cons[i].body[k]].ent == ent)
				return 1;
	return 0;
}

int Phys_ConstraintProbe(int i, f32 a[3], f32 b[3], f32 *len, int *on)
{
	if (i < 0 || i >= numCons)
		return 0;
	end_world(&cons[i], 0, a);
	end_world(&cons[i], 1, b);
	*len = cons[i].len;
	*on = cons[i].on && !cons[i].broken;
	return cons[i].type + 1;
}
