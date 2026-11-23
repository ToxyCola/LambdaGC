#include "envents.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "collide.h"
#include "ent.h"
#include "gibs.h"
#include "physics.h"
#include "weapons.h"
#include "npc.h"
#include "scene.h"
#include "caption.h"

static f32 frand(void) { return (f32)(rand() & 0xFFFF) / 65535.0f; }
static f32 frange(f32 a, f32 b) { return a + (b - a) * frand(); }

static const char *kv(s32 ent, const char *k)
{
	const char *v = Ent_KV(ent, k);
	return v ? v : "";
}
static f32 kvf(s32 ent, const char *k, f32 def)
{
	const char *v = Ent_KV(ent, k);
	return v && *v ? (f32)atof(v) : def;
}
static Entity *E(s32 ent) { return Ent_List(NULL) + ent; }

/* the entity's sound slot 0 (_esnd: tools/npcs.py, its Source sound) */
static void play_esnd(s32 ent, const f32 at[3], f32 vol)
{
	const Pack *P = Ent_Pack();
	Entity *e = E(ent);
	if (P && e->esnd[0] >= 0 && (u32)e->esnd[0] < P->numSounds)
		Audio_Play(&P->sounds[e->esnd[0]], at, vol, SNDLVL_DEFAULT);
}

static void forward_of(s32 ent, f32 out[3])
{
	Entity *e = E(ent);
	for (int k = 0; k < 3; k++)
		out[k] = e->R[k][0];
}

static void angles_forward(const f32 a[3], f32 out[3])
{
	f32 p = a[0] * (f32)M_PI / 180, y = a[1] * (f32)M_PI / 180;
	out[0] = cosf(p) * cosf(y);
	out[1] = cosf(p) * sinf(y);
	out[2] = -sinf(p);
}

static int parse3(const char *s, f32 o[3]) { return s && sscanf(s, "%f %f %f", &o[0], &o[1], &o[2]) == 3; }

static u32 colour(s32 ent, const char *key, u32 def)
{
	int r, g, b;
	const char *v = kv(ent, key);
	if (sscanf(v, "%d %d %d", &r, &g, &b) == 3)
		return (u32)(r & 255) << 24 | (u32)(g & 255) << 16 | (u32)(b & 255) << 8;
	return def;
}

/* ------------------------------------------------------------------ ropes */
#define ROPE_MAX_NODES 10
#define ROPE_DRAW_PTS ((ROPE_MAX_NODES - 1) * 3 + 1)
#define ROPE_STEP (1.0f / 30.0f)
#define ROPE_GRAVITY -1500.0f
#define ROPE_DAMP 0.98f
#define ROPESLACK_FUDGEFACTOR -100.0f

typedef struct {
	s32 a, b;       /* the keyframe it starts at, the one it ends at */
	u8 n, lockEnd, collide, resize, sub, alive;
	f32 slack, len, spring, width, acc;
	u32 rgba;
	f32 pos[ROPE_MAX_NODES][3], prev[ROPE_MAX_NODES][3];
	f32 draw[ROPE_DRAW_PTS][3];
	int numDraw;
} Rope;

static Rope *ropes;
static int numRopes;

static int is_rope(const char *cn) { return !strcmp(cn, "move_rope") || !strcmp(cn, "keyframe_rope"); }

static void rope_springs(Rope *r)
{
	f32 s = (r->len + r->slack + ROPESLACK_FUDGEFACTOR) / (f32)(r->n - 1);
	r->spring = s > 0 ? s : 0;
}

static void rope_lock(Rope *r)
{
	memcpy(r->pos[0], E(r->a)->origin, 12);
	if (r->lockEnd)
		memcpy(r->pos[r->n - 1], E(r->b)->origin, 12);
}

static void rope_step(Rope *r)
{
	for (int i = 0; i < r->n; i++) {
		f32 t[3];
		memcpy(t, r->pos[i], 12);
		r->pos[i][0] += (r->pos[i][0] - r->prev[i][0]) * ROPE_DAMP;
		r->pos[i][1] += (r->pos[i][1] - r->prev[i][1]) * ROPE_DAMP;
		r->pos[i][2] += (r->pos[i][2] - r->prev[i][2]) * ROPE_DAMP + ROPE_GRAVITY * ROPE_STEP * ROPE_STEP * 0.5f;
		if (r->collide && (CM_PointContents(r->pos[i]) & CONTENTS_SOLID))
			memcpy(r->pos[i], t, 12);
		memcpy(r->prev[i], t, 12);
	}
	/* CBaseRopePhysics::ApplyConstraints: each spring only pulls (when stretched past its length) */
	for (int it = 0; it < 3; it++) {
		for (int i = 0; i + 1 < r->n; i++) {
			f32 d[3] = {r->pos[i + 1][0] - r->pos[i][0], r->pos[i + 1][1] - r->pos[i][1], r->pos[i + 1][2] - r->pos[i][2]};
			f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (l <= r->spring || l < 1e-4f)
				continue;
			f32 s = (1 - r->spring / l) * 0.5f;
			for (int k = 0; k < 3; k++) {
				r->pos[i][k] += d[k] * s;
				r->pos[i + 1][k] -= d[k] * s;
			}
		}
		rope_lock(r);
	}
}

/* the drawn curve: Catmull-Rom through the nodes, "Subdiv" points between each (c_rope.cpp) */
static void rope_curve(Rope *r)
{
	int m = 0;
	for (int i = 0; i + 1 < r->n; i++) {
		const f32 *p0 = r->pos[i ? i - 1 : 0], *p1 = r->pos[i], *p2 = r->pos[i + 1], *p3 = r->pos[i + 2 < r->n ? i + 2 : i + 1];
		for (int s = 0; s <= r->sub; s++) {
			f32 t = (f32)s / (f32)(r->sub + 1), t2 = t * t, t3 = t2 * t;
			for (int k = 0; k < 3; k++)
				r->draw[m][k] = 0.5f * (2 * p1[k] + (-p0[k] + p2[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2 +
				                        (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3);
			m++;
		}
	}
	memcpy(r->draw[m++], r->pos[r->n - 1], 12);
	r->numDraw = m;
}

static void ropes_spawn(void)
{
	u32 n;
	Entity *ents = Ent_List(&n);
	int count = 0;
	for (u32 i = 1; i < n; i++)
		if (is_rope(ents[i].classname))
			count++;
	ropes = count ? calloc((size_t)count, sizeof(Rope)) : NULL;
	numRopes = 0;
	for (u32 i = 1; i < n && ropes; i++) {
		if (!is_rope(ents[i].classname))
			continue;
		const char *next = kv((s32)i, "nextkey");
		s32 b = *next ? Ent_FindActor(next) : -1;
		int dangling = atoi(kv((s32)i, "dangling")) != 0;
		if (b <= 0)
			continue; /* the last keyframe of a chain: no rope comes out of it */
		Rope *r = &ropes[numRopes++];
		r->a = (s32)i;
		r->b = b;
		int type = atoi(kv((s32)i, "type"));
		r->n = type == 0 ? 10 : type == 1 ? 4 : 2; /* ROPE_MAX_SEGMENTS, ROPE_TYPE1/2_NUMSEGMENTS */
		const char *sd = kv((s32)i, "subdiv");
		int sub = *sd ? atoi(sd) : 2;
		r->sub = (u8)(sub < 0 ? 0 : sub > 2 ? 2 : sub);
		r->lockEnd = !dangling;
		r->collide = atoi(kv((s32)i, "collide")) != 0;
		r->resize = (ents[i].spawnflags & 1) != 0; /* SF_ROPE_RESIZE */
		r->slack = kvf((s32)i, "slack", 0);
		r->width = kvf((s32)i, "width", 2);
		r->rgba = colour((s32)i, "_ropecolor", 0x28282800u) | 0xFF;
		const f32 *pa = ents[i].origin, *pb = ents[b].origin;
		f32 d[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
		r->len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		rope_springs(r);
		for (int k = 0; k < r->n; k++) {
			f32 t = (f32)k / (f32)(r->n - 1);
			for (int c = 0; c < 3; c++)
				r->pos[k][c] = r->prev[k][c] = pa[c] + d[c] * t;
		}
		/* ROPE_INITIAL_HANG: hang for a moment before it's seen */
		for (int s = 0; s < 90; s++)
			rope_step(r);
		rope_curve(r);
		r->alive = 1;
	}
}

static void ropes_tick(f32 dt)
{
	const f32 *eye = Ent_List(NULL)[0].origin;
	for (int i = 0; i < numRopes; i++) {
		Rope *r = &ropes[i];
		if (!r->alive)
			continue;
		Entity *a = E(r->a), *b = E(r->b);
		if (a->flags & EF_KILLED) {
			r->alive = 0;
			continue;
		}
		if (b->flags & EF_KILLED)
			r->lockEnd = 0;
		if (r->resize && r->lockEnd) {
			f32 d[3] = {b->origin[0] - a->origin[0], b->origin[1] - a->origin[1], b->origin[2] - a->origin[2]};
			r->len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			rope_springs(r);
		}
		/* (like the PC, only ropes near enough to be seen move) */
		f32 o[3] = {r->pos[r->n / 2][0] - eye[0], r->pos[r->n / 2][1] - eye[1], r->pos[r->n / 2][2] - eye[2]};
		if (o[0] * o[0] + o[1] * o[1] + o[2] * o[2] > 4000.0f * 4000.0f + r->len * r->len) {
			rope_lock(r);
			continue;
		}
		r->acc += dt;
		int steps = 0;
		while (r->acc >= ROPE_STEP && steps < 4) {
			rope_step(r);
			r->acc -= ROPE_STEP;
			steps++;
		}
		if (r->acc > ROPE_STEP)
			r->acc = 0;
		rope_lock(r);
		rope_curve(r);
	}
}

static int rope_input(s32 ent, const char *in)
{
	if (!strcasecmp(in, "break")) {
		for (int i = 0; i < numRopes; i++)
			if (ropes[i].a == ent || ropes[i].b == ent)
				ropes[i].lockEnd = 0;
		return 1;
	}
	return !strcasecmp(in, "setforce") || !strcasecmp(in, "setscrollspeed");
}

int EnvEnts_NumRopes(void) { return numRopes; }

int EnvEnts_Rope(int i, RopeDraw *out)
{
	if (i < 0 || i >= numRopes || !ropes[i].alive || (E(ropes[i].a)->flags & EF_DISABLED))
		return 0;
	out->pts = (const f32(*)[3])ropes[i].draw;
	out->n = ropes[i].numDraw;
	out->width = ropes[i].width;
	out->rgba = ropes[i].rgba;
	return 1;
}

int EnvEnts_RopeNodes(int i, f32 (*out)[3], int max)
{
	if (i < 0 || i >= numRopes)
		return 0;
	int n = ropes[i].n < max ? ropes[i].n : max;
	memcpy(out, ropes[i].pos, (size_t)n * 12);
	return n;
}

/* ------------------------------------------------------------------ lightning (env_beam, point_tesla) */
#define MAX_ARCS 48
typedef struct {
	BeamDraw d;
	f32 until;
	s32 owner;
	u8 alive;
} Arc;
static Arc arcs[MAX_ARCS];

static int tex_of(s32 ent, const char *key)
{
	const char *t = kv(ent, key);
	if (strstr(t, "physbeam"))
		return BEAM_PHYS;
	if (strstr(t, "lgtning") || strstr(t, "lightning"))
		return BEAM_LIGHTNING;
	return BEAM_LASER;
}

/* a jagged bolt from a to b: midpoint displacement, amplitude in units */
static void bolt(BeamDraw *d, const f32 a[3], const f32 b[3], f32 amp, int segs)
{
	if (segs > 15)
		segs = 15;
	if (segs < 1)
		segs = 1;
	f32 dir[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
	f32 up[3] = {0, 0, 1};
	if (fabsf(dir[2]) > fabsf(dir[0]) + fabsf(dir[1]))
		up[0] = 1, up[2] = 0;
	f32 s1[3] = {dir[1] * up[2] - dir[2] * up[1], dir[2] * up[0] - dir[0] * up[2], dir[0] * up[1] - dir[1] * up[0]};
	f32 l = sqrtf(s1[0] * s1[0] + s1[1] * s1[1] + s1[2] * s1[2]);
	if (l > 0)
		for (int k = 0; k < 3; k++)
			s1[k] /= l;
	f32 dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	f32 s2[3] = {0, 0, 0};
	if (dl > 0) {
		s2[0] = (dir[1] * s1[2] - dir[2] * s1[1]) / dl;
		s2[1] = (dir[2] * s1[0] - dir[0] * s1[2]) / dl;
		s2[2] = (dir[0] * s1[1] - dir[1] * s1[0]) / dl;
	}
	d->n = segs + 1;
	for (int i = 0; i <= segs; i++) {
		f32 t = (f32)i / (f32)segs;
		f32 j = (i == 0 || i == segs) ? 0 : amp * sinf(t * (f32)M_PI); /* pinned at the ends */
		f32 x = frange(-1, 1) * j, y = frange(-1, 1) * j;
		for (int k = 0; k < 3; k++)
			d->pts[i][k] = a[k] + dir[k] * t + s1[k] * x + s2[k] * y;
	}
}

static Arc *new_arc(s32 owner)
{
	for (int i = 0; i < MAX_ARCS; i++)
		if (!arcs[i].alive) {
			memset(&arcs[i], 0, sizeof(Arc));
			arcs[i].alive = 1;
			arcs[i].owner = owner;
			return &arcs[i];
		}
	return NULL;
}

static void kill_arcs(s32 owner)
{
	for (int i = 0; i < MAX_ARCS; i++)
		if (arcs[i].owner == owner)
			arcs[i].alive = 0;
}

int EnvEnts_NumBeams(void) { return MAX_ARCS; }

int EnvEnts_Beam(int i, BeamDraw *out)
{
	if (i < 0 || i >= MAX_ARCS || !arcs[i].alive)
		return 0;
	*out = arcs[i].d;
	return 1;
}

/* ------------------------------------------------------------------ effect entities */
enum {
	FK_SPARK = 1, FK_EXPLOSION, FK_PHYSEXPLOSION, FK_PHYSIMPACT, FK_BEAM, FK_TESLA, FK_SHOOTER, FK_FIRE, FK_SPLASH,
	FK_MUZZLE, FK_GUNFIRE, FK_DUST, FK_AR2EXPLOSION, FK_PHYSBOX, FK_CONSTRAINT, FK_ROPE, FK_SPEAKER, FK_HURT
};

typedef struct {
	s32 ent;
	u8 kind, on;
	f32 next;      /* spark / tesla / gunfire / beam: next event */
	int left;      /* shooter: gibs still to throw; gunfire: shots left in the burst */
	f32 fire;      /* env_fire: 0..1 how big it is now */
	f32 fireLeft;  /* seconds it still burns (not infinite) */
	u32 loop;      /* a looping sound */
	f32 hurtNext;
} Fx;

static Fx *fxl;
static int numFx;
static int sparks; /* (tests) */

static int kind_of(const char *cn)
{
	static const struct {
		const char *n;
		u8 k;
	} map[] = {
		{"env_spark", FK_SPARK},
		{"env_explosion", FK_EXPLOSION},
		{"env_physexplosion", FK_PHYSEXPLOSION},
		{"env_physimpact", FK_PHYSIMPACT},
		{"env_beam", FK_BEAM},
		{"env_laser", FK_BEAM},
		{"point_tesla", FK_TESLA},
		{"env_shooter", FK_SHOOTER},
		{"env_fire", FK_FIRE},
		{"env_splash", FK_SPLASH},
		{"env_muzzleflash", FK_MUZZLE},
		{"env_gunfire", FK_GUNFIRE},
		{"env_dustpuff", FK_DUST},
		{"env_ar2explosion", FK_AR2EXPLOSION},
		{"func_physbox", FK_PHYSBOX},
		{"phys_spring", FK_CONSTRAINT},
		{"phys_lengthconstraint", FK_CONSTRAINT},
		{"phys_ballsocket", FK_CONSTRAINT},
		{"phys_constraint", FK_CONSTRAINT},
		{"phys_hinge", FK_CONSTRAINT},
		{"phys_motor", FK_CONSTRAINT},
		{"move_rope", FK_ROPE},
		{"env_speaker", FK_SPEAKER},
		{"point_hurt", FK_HURT},
		{"keyframe_rope", FK_ROPE},
	};
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(cn, map[i].n))
			return map[i].k;
	return 0;
}

static Fx *fx_of(s32 ent)
{
	/* (fxl is in entity order: a binary search) */
	int lo = 0, hi = numFx - 1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (fxl[m].ent == ent)
			return &fxl[m];
		if (fxl[m].ent < ent)
			lo = m + 1;
		else
			hi = m - 1;
	}
	return NULL;
}

void EnvEnts_Free(void)
{
	free(ropes);
	ropes = NULL;
	numRopes = 0;
	free(fxl);
	fxl = NULL;
	numFx = 0;
	memset(arcs, 0, sizeof(arcs));
	sparks = 0;
}

void EnvEnts_Spawn(void)
{
	EnvEnts_Free();
	u32 n;
	Entity *ents = Ent_List(&n);
	int count = 0;
	for (u32 i = 1; i < n; i++)
		if (kind_of(ents[i].classname))
			count++;
	fxl = count ? calloc((size_t)count, sizeof(Fx)) : NULL;
	for (u32 i = 1; i < n && fxl; i++) {
		int k = kind_of(ents[i].classname);
		if (!k)
			continue;
		Fx *f = &fxl[numFx++];
		f->ent = (s32)i;
		f->kind = (u8)k;
		u32 sf = ents[i].spawnflags;
		switch (k) {
		case FK_SPARK:
			f->on = (sf & 64) != 0; /* SF_SPARK_START_ON */
			f->next = 0.1f + frange(0, 1.5f);
			break;
		case FK_BEAM:
			f->on = (sf & 1) != 0 || !strcmp(ents[i].classname, "env_laser"); /* "Start On" */
			if (!strcmp(ents[i].classname, "env_laser"))
				f->on = (sf & 1) != 0;
			break;
		case FK_TESLA:
			f->on = atoi(kv((s32)i, "m_bon")) != 0;
			f->next = frange(kvf((s32)i, "interval_min", 0.5f), kvf((s32)i, "interval_max", 2.0f));
			break;
		case FK_SPEAKER:
			/* CSpeaker::Precache: talking 5-15 s in unless it starts silent (1) */
			f->on = !(sf & 1);
			f->next = Ent_Now() + frange(5, 15);
			f->left = -1; /* (the last item said) */
			break;
		case FK_HURT:
			/* point_hurt (CPointHurt): hurts what's in its radius every "damagedelay" while on (Start On: 1) */
			f->on = (sf & 1) != 0;
			f->next = Ent_Now() + 0.1f;
			f->left = -1; /* (its activator) */
			break;
		case FK_FIRE:
			f->on = (sf & 4) != 0; /* "Start On" */
			f->fire = f->on && (sf & 8) ? 1.0f : 0.0f; /* "Start Full" */
			f->fireLeft = kvf((s32)i, "health", 30);
			break;
		case FK_GUNFIRE:
			f->on = atoi(kv((s32)i, "startdisabled")) == 0;
			f->next = frange(kvf((s32)i, "minburstdelay", 2), kvf((s32)i, "maxburstdelay", 5));
			break;
		default:
			break;
		}
	}
	ropes_spawn();
}

/* ---- the effects themselves */

static void do_spark(Fx *f)
{
	Entity *e = E(f->ent);
	f32 dir[3];
	forward_of(f->ent, dir);
	int mag = atoi(kv(f->ent, "magnitude"));
	int trail = atoi(kv(f->ent, "traillength"));
	Weap_Sparks(e->origin, (e->spawnflags & 512) ? dir : NULL, mag ? mag : 1, trail ? trail : 1);
	if (!(e->spawnflags & 256)) /* SF_SPARK_SILENT */
		play_esnd(f->ent, e->origin, 1.0f);
	sparks++;
	Ent_FireOutput(f->ent, "onspark", f->ent);
}

static void do_explosion(Fx *f)
{
	Entity *e = E(f->ent);
	u32 sf = e->spawnflags;
	f32 mag = kvf(f->ent, "imagnitude", 100);
	f32 rad = kvf(f->ent, "iradiusoverride", 0);
	if (rad <= 0)
		rad = mag * 2.5f;
	int flags = 0;
	if (sf & 4)
		flags |= EXPL_NOFIREBALL;
	if (sf & 8)
		flags |= EXPL_NOSMOKE;
	if (sf & 32)
		flags |= EXPL_NOSPARKS;
	if (sf & 64)
		flags |= EXPL_NOSOUND;
	Weap_Explosion(e->origin, mag / 100.0f, flags);
	if (!(sf & 1)) /* "No Damage" */
		Ent_RadiusDamage(e->origin, mag, rad);
	if (!(sf & 2)) /* not "Repeatable" */
		e->flags |= EF_KILLED;
}

static void do_physexplosion(Fx *f)
{
	Entity *e = E(f->ent);
	f32 mag = kvf(f->ent, "magnitude", 100);
	f32 rad = kvf(f->ent, "radius", 0);
	if (rad <= 0)
		rad = mag * 2.5f;
	Phys_Explosion(e->origin, rad, mag);
	/* weapons and items are physics objects on the PC too: CalculateExplosiveDamageForce, force
	 * (magnitude - distance / 2.5, at least 1) x 300, over a light item's mass (about 5) */
	{
		const char *tn = kv(f->ent, "targetentityname");
		u32 n;
		Entity *ents = Ent_List(&n);
		for (u32 i = 1; i < n; i++) {
			Entity *t = &ents[i];
			if ((t->flags & (EF_KILLED | EF_DISABLED)) ||
			    (strncmp(t->classname, "weapon_", 7) && strncmp(t->classname, "item_", 5)))
				continue;
			if (*tn ? !(t->name && !strcasecmp(t->name, tn)) : 0)
				continue;
			f32 d[3] = {t->origin[0] - e->origin[0], t->origin[1] - e->origin[1], t->origin[2] - e->origin[2]};
			f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (!*tn && l > rad)
				continue;
			f32 adj = mag - l * 0.4f;
			if (adj < 1)
				adj = 1;
			f32 sp = adj * 300.0f / 5.0f;
			f32 v[3];
			for (int k = 0; k < 3; k++)
				v[k] = l > 1e-3f ? d[k] / l * sp : 0;
			Weap_ItemImpulse((s32)i, v);
		}
	}
	if (!(e->spawnflags & 1)) /* SF_PHYSEXPLOSION_NODAMAGE */
		Ent_RadiusDamage(e->origin, mag * 0.5f, rad);
}

static void do_physimpact(Fx *f)
{
	Entity *e = E(f->ent);
	f32 dir[3];
	angles_forward(e->angles, dir);
	/* CPhysImpact: aimed at its direction entity when it names one (the can cop's push at the can) */
	const char *dn = kv(f->ent, "directionentityname");
	if (dn && *dn) {
		s32 t = Ent_FindActor(dn);
		if (t > 0) {
			f32 d[3] = {E(t)->origin[0] - e->origin[0], E(t)->origin[1] - e->origin[1], E(t)->origin[2] - e->origin[2]};
			f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (l > 1e-3f)
				for (int k = 0; k < 3; k++)
					dir[k] = d[k] / l;
		}
	}
	f32 dist = kvf(f->ent, "distance", 1024), mag = kvf(f->ent, "magnitude", 100);
	f32 end[3] = {e->origin[0] + dir[0] * dist, e->origin[1] + dir[1] * dist, e->origin[2] + dir[2] * dist};
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_SetIgnoreEnt(f->ent);
	CM_BoxTrace(&tr, e->origin, end, z, z, MASK_SHOT);
	CM_SetIgnoreEnt(-1);
	s32 hit = tr.fraction < 1 && tr.ent > 0 && Phys_IsBody(tr.ent) ? tr.ent : -1;
	f32 at[3];
	memcpy(at, tr.endpos, 12);
	/* (physics props aren't in the world's collision: the nearest one along the line, by its box) */
	f32 best = tr.fraction * dist;
	u32 n;
	Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		if ((s32)i == f->ent || !Phys_IsBody((s32)i))
			continue;
		f32 mn[3], mx[3], t0 = 0, t1 = best;
		Ent_WorldBounds((s32)i, mn, mx);
		int ok = 1;
		for (int k = 0; k < 3 && ok; k++) {
			if (fabsf(dir[k]) < 1e-6f) {
				ok = e->origin[k] >= mn[k] && e->origin[k] <= mx[k];
				continue;
			}
			f32 a = (mn[k] - e->origin[k]) / dir[k], b = (mx[k] - e->origin[k]) / dir[k];
			if (a > b) { f32 x = a; a = b; b = x; }
			if (a > t0) t0 = a;
			if (b < t1) t1 = b;
			ok = t0 <= t1;
		}
		if (!ok || t0 >= best)
			continue;
		best = t0;
		hit = (s32)i;
		for (int k = 0; k < 3; k++)
			at[k] = e->origin[k] + dir[k] * t0;
	}
	if (hit > 0) {
		/* CPhysImpact::InputImpact: ApplyForceOffset(damage * dir), the damage falling off along the line
		 * unless flagged (1 no falloff, 2 infinite length), times the mass when flagged (4) */
		u32 sf = e->spawnflags;
		f32 dmg = (sf & 3) ? mag : mag * (1.0f - best / (dist > 1 ? dist : 1));
		if (sf & 4)
			dmg *= Phys_Mass(hit);
		f32 imp[3] = {dir[0] * dmg, dir[1] * dmg, dir[2] * dmg};
		Phys_Wake(hit);
		Phys_Impulse(hit, at, imp);
	}
}

static void beam_strike(Fx *f)
{
	Entity *e = E(f->ent);
	const char *sn = kv(f->ent, "lightningstart"), *en = kv(f->ent, "lightningend");
	if (!strcmp(e->classname, "env_laser"))
		en = kv(f->ent, "lasertarget"), sn = "";
	if (!*en)
		en = kv(f->ent, "lasertarget");
	s32 s = *sn ? Ent_FindActor(sn) : f->ent, t = *en ? Ent_FindActor(en) : -1;
	if (s < 0)
		s = f->ent;
	f32 a[3], b[3];
	memcpy(a, E(s)->origin, 12);
	f32 rad = kvf(f->ent, "radius", 256);
	if (t > 0) {
		memcpy(b, E(t)->origin, 12);
	} else {
		/* "Random Strike": somewhere solid within the radius */
		f32 d[3] = {frange(-1, 1), frange(-1, 1), frange(-1, 1)};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
		f32 to[3] = {a[0] + d[0] / l * rad, a[1] + d[1] / l * rad, a[2] + d[2] / l * rad};
		Trace tr;
		CM_TraceWorld(&tr, a, to, MASK_SHOT);
		if (tr.fraction >= 1)
			return;
		memcpy(b, tr.endpos, 12);
	}
	Arc *ar = new_arc(f->ent);
	if (!ar)
		return;
	f32 life = kvf(f->ent, "life", 0);
	ar->until = life > 0 ? Ent_Now() + life : Ent_Now() + 0.1f;
	ar->d.width = kvf(f->ent, "boltwidth", kvf(f->ent, "width", 2));
	f32 amp = kvf(f->ent, "noiseamplitude", 0);
	f32 len = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
	bolt(&ar->d, a, b, amp * 0.5f, amp > 0 ? (int)(len / 24) + 2 : 1);
	ar->d.rgba = colour(f->ent, "rendercolor", 0xFFFFFF00u) | (u32)kvf(f->ent, "renderamt", 255);
	ar->d.tex = tex_of(f->ent, "texture");
	if (e->spawnflags & 16) /* "Start Sparks" */
		Weap_Sparks(a, NULL, 1, 1);
	if (e->spawnflags & 32) /* "End Sparks" */
		Weap_Sparks(b, NULL, 1, 1);
	/* the beam's damage to the player standing in it (DMG_SHOCK, per strike) */
	f32 dmg = kvf(f->ent, "damage", 0);
	if (dmg > 0) {
		const f32 *pp = Ent_List(NULL)[0].origin;
		f32 pc[3] = {pp[0], pp[1], pp[2] + 36};
		f32 ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {pc[0] - a[0], pc[1] - a[1], pc[2] - a[2]};
		f32 t = (ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2]) / (len * len + 1e-3f);
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		f32 c[3] = {a[0] + ab[0] * t - pc[0], a[1] + ab[1] * t - pc[1], a[2] + ab[2] * t - pc[2]};
		if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 40 * 40)
			Ent_HurtPlayer(dmg, DMG_SHOCK, a);
	}
}

static void tesla_spark(Fx *f)
{
	Entity *e = E(f->ent);
	const char *sn = kv(f->ent, "m_sourceentityname");
	s32 s = *sn ? Ent_FindActor(sn) : f->ent;
	const f32 *a = E(s > 0 ? s : f->ent)->origin;
	f32 rad = kvf(f->ent, "m_flradius", 200);
	int cnt = (int)frange(kvf(f->ent, "beamcount_min", 6), kvf(f->ent, "beamcount_max", 6) + 0.99f);
	u32 col = colour(f->ent, "m_color", 0xFFFFFF00u) | 0xFF;
	for (int i = 0; i < cnt; i++) {
		f32 d[3] = {frange(-1, 1), frange(-1, 1), frange(-1, 1)};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
		f32 to[3] = {a[0] + d[0] / l * rad, a[1] + d[1] / l * rad, a[2] + d[2] / l * rad};
		Trace tr;
		CM_TraceWorld(&tr, a, to, MASK_SHOT);
		if (tr.fraction >= 1)
			continue;
		Arc *ar = new_arc(f->ent);
		if (!ar)
			break;
		ar->until = Ent_Now() + frange(kvf(f->ent, "lifetime_min", 0.3f), kvf(f->ent, "lifetime_max", 0.3f));
		ar->d.width = frange(kvf(f->ent, "thick_min", 5), kvf(f->ent, "thick_max", 5));
		bolt(&ar->d, a, tr.endpos, 12, 8);
		ar->d.rgba = col;
		ar->d.tex = tex_of(f->ent, "texture");
		if (ar->d.tex == BEAM_LASER && !*kv(f->ent, "texture"))
			ar->d.tex = BEAM_LIGHTNING;
	}
	play_esnd(f->ent, a, 1.0f);
	(void)e;
}

static void shooter_tick(Fx *f, f32 dt)
{
	(void)dt;
	if (f->left <= 0 || Ent_Now() < f->next)
		return;
	Entity *e = E(f->ent);
	const char *m = Ent_KV(f->ent, "_mdl");
	if (!m) {
		f->left = 0;
		return;
	}
	f32 ga[3] = {0, 0, 0}, dir[3];
	if (!parse3(kv(f->ent, "gibangles"), ga))
		memcpy(ga, e->angles, 12);
	angles_forward(e->angles, dir);
	f32 var = kvf(f->ent, "m_flvariance", 0.15f);
	f32 sp = kvf(f->ent, "m_flvelocity", 200);
	f32 v[3];
	for (int k = 0; k < 3; k++)
		v[k] = (dir[k] + frange(-var, var)) * sp;
	f32 R[3][3];
	Ent_AngleMatrix(ga, R);
	u8 tint[4] = {255, 255, 255, 255};
	Gibs_Throw((s32)atoi(m), e->origin, R, v, kvf(f->ent, "m_flgiblife", 4), tint);
	f->left--;
	f->next = Ent_Now() + kvf(f->ent, "delay", 0);
}

static void fire_tick(Fx *f, f32 dt)
{
	Entity *e = E(f->ent);
	f32 attack = kvf(f->ent, "fireattack", 4);
	if (f->on) {
		f->fire += dt / (attack > 0.1f ? attack : 0.1f);
		if (f->fire > 1)
			f->fire = 1;
		if (!(e->spawnflags & 1)) { /* not "Infinite Duration" */
			f->fireLeft -= dt;
			if (f->fireLeft <= 0) {
				f->on = 0;
				Ent_FireOutput(f->ent, "onextinguished", f->ent);
			}
		}
		/* heat: the player in the flames burns (fire.cpp's damage, scaled by "damagescale") */
		f32 size = kvf(f->ent, "firesize", 64) * f->fire;
		const f32 *pp = Ent_List(NULL)[0].origin;
		f32 dx = pp[0] - e->origin[0], dy = pp[1] - e->origin[1], dz = pp[2] - e->origin[2];
		if (Ent_Now() >= f->hurtNext && dx * dx + dy * dy < size * size * 0.25f && dz > -16 && dz < size) {
			Ent_HurtPlayer(4.0f * kvf(f->ent, "damagescale", 1), DMG_BURN, e->origin);
			f->hurtNext = Ent_Now() + 0.25f;
		}
		if (!(e->spawnflags & 2) && frand() < dt * 4) { /* smoke, unless "Smokeless" */
			f32 at[3] = {e->origin[0], e->origin[1], e->origin[2] + size};
			f32 up[3] = {0, 0, 1};
			Weap_Dust(at, up, size / 64.0f, 0x30302800u, 40, 1);
		}
	} else if (f->fire > 0) {
		f->fire -= dt * 0.5f;
		if (f->fire <= 0) {
			f->fire = 0;
			if (f->loop)
				Audio_Stop(f->loop);
			f->loop = 0;
			if (e->spawnflags & 128) /* "Delete when out" */
				e->flags |= EF_KILLED;
		}
	}
}

static void gunfire_tick(Fx *f, f32 dt)
{
	(void)dt;
	if (!f->on || Ent_Now() < f->next)
		return;
	Entity *e = E(f->ent);
	if (f->left <= 0) {
		f->left = (int)frange(kvf(f->ent, "minburstsize", 2), kvf(f->ent, "maxburstsize", 7) + 0.99f);
	}
	const char *tn = kv(f->ent, "target");
	s32 t = *tn ? Ent_FindActor(tn) : -1;
	f32 dir[3];
	if (t > 0) {
		f32 d[3] = {E(t)->origin[0] - e->origin[0], E(t)->origin[1] - e->origin[1], E(t)->origin[2] - e->origin[2]};
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
		for (int k = 0; k < 3; k++)
			dir[k] = d[k] / l;
	} else {
		forward_of(f->ent, dir);
	}
	f32 spread = kvf(f->ent, "spread", 5) * (f32)M_PI / 180.0f;
	for (int k = 0; k < 3; k++)
		dir[k] += frange(-spread, spread);
	f32 end[3] = {e->origin[0] + dir[0] * 4096, e->origin[1] + dir[1] * 4096, e->origin[2] + dir[2] * 4096};
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_BoxTrace(&tr, e->origin, end, z, z, MASK_SHOT);
	if (tr.fraction < 1)
		Weap_BulletImpact(&tr, dir);
	Arc *ar = new_arc(f->ent); /* the tracer */
	if (ar) {
		ar->until = Ent_Now() + 0.05f;
		ar->d.n = 2;
		memcpy(ar->d.pts[0], e->origin, 12);
		memcpy(ar->d.pts[1], tr.endpos, 12);
		ar->d.width = 1.5f;
		ar->d.rgba = 0xFFE0A0C0u;
		ar->d.tex = BEAM_LASER;
	}
	Weap_Muzzle(e->origin, 0.5f);
	play_esnd(f->ent, e->origin, 1.0f);
	f->left--;
	f32 rate = kvf(f->ent, "rateoffire", 10);
	f->next = Ent_Now() + (f->left > 0 ? 1.0f / (rate > 0.1f ? rate : 0.1f)
	                                   : frange(kvf(f->ent, "minburstdelay", 2), kvf(f->ent, "maxburstdelay", 5)));
}

/* env_speaker: its response group, like Source's: the "displayfirst" one first, then at random without
 * saying the same twice running ("sequential" in order; "norepeat": each once, then it's quiet) */
static void speaker_say(Fx *f)
{
	const char *list = kv(f->ent, "_spk");
	if (!list)
		return;
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", list);
	char *items[48];
	int n = 0;
	for (char *t = strtok(buf, " "); t && n < 48; t = strtok(NULL, " "))
		items[n++] = t;
	if (!n)
		return;
	const char *fl = kv(f->ent, "_spkf");
	int seq = fl && !strcmp(fl, "sequential"), norep = fl && !strcmp(fl, "norepeat");
	int pick = -1;
	if (f->left < 0) {
		for (int i = 0; i < n && pick < 0; i++)
			if (items[i][0] == '!')
				pick = i;
	}
	if (pick < 0) {
		if (seq || norep) {
			pick = f->left + 1;
			if (pick >= n) {
				if (norep) {
					f->on = 0;
					return;
				}
				pick = 0;
			}
		} else {
			pick = rand() % n;
			if (n > 1 && pick == f->left)
				pick = (pick + 1 + rand() % (n - 1)) % n;
		}
	}
	f->left = pick;
	const char *it = items[pick][0] == '!' ? items[pick] + 1 : items[pick];
	const Pack *P = Ent_Pack();
	int idx = atoi(it + 1);
	if (it[0] == 'c')
		Scene_Start(f->ent, (u32)idx, f->ent);
	else if (it[0] == 's' && idx >= 0 && (u32)idx < P->numSounds) {
		Audio_Play(&P->sounds[idx], E(f->ent)->origin, 1.0f, SNDLVL_TALKING);
		if (P->soundCaptions && P->soundCaptions[idx])
			Caption_Add(P->entStrings + P->soundCaptions[idx], Audio_Duration(&P->sounds[idx]));
	}
}

static void speaker_tick(Fx *f)
{
	f32 now = Ent_Now();
	if (!f->on || now < f->next)
		return;
	/* (CSpeaker::SpeakerThink: someone talking already: after they're done, give or take a second) */
	f32 busy = Npc_TalkingUntil();
	if (busy > now) {
		f->next = busy + frange(0, 1);
		return;
	}
	speaker_say(f);
	f->next = now + frange(kvf(f->ent, "delaymin", 15), kvf(f->ent, "delaymax", 135));
}

/* point_hurt (CPointHurt::HurtThink): every damagedelay (0.1 s if none), "damage" (2 if none) to what it
 * names ("damagetarget", !activator too) or to everything within its radius (128 if none) */
static void hurt_now(Fx *f)
{
	f32 dmg = kvf(f->ent, "damage", 0), rad = kvf(f->ent, "damageradius", 0);
	if (dmg <= 0)
		dmg = 2;
	if (rad <= 0)
		rad = 128;
	const char *tn = kv(f->ent, "damagetarget");
	Entity *me = E(f->ent);
	if (tn && *tn) {
		s32 t = !strcasecmp(tn, "!activator") ? f->left : !strcasecmp(tn, "!player") ? 0 : Ent_FindActor(tn);
		if (t >= 0)
			Ent_Damage(t, dmg, NULL, me->origin);
		return;
	}
	Ent_RadiusDamage(me->origin, dmg, rad);
}

static void hurt_tick(Fx *f)
{
	f32 now = Ent_Now();
	if (!f->on || now < f->next)
		return;
	f32 d = kvf(f->ent, "damagedelay", 0);
	f->next = now + (d > 0 ? d : 0.1f);
	hurt_now(f);
}

void EnvEnts_Tick(f32 dt)
{
	f32 now = Ent_Now();
	for (int i = 0; i < numFx; i++) {
		Fx *f = &fxl[i];
		Entity *e = E(f->ent);
		if (e->flags & EF_KILLED) {
			if (f->kind == FK_BEAM || f->kind == FK_TESLA)
				kill_arcs(f->ent);
			continue;
		}
		switch (f->kind) {
		case FK_SPARK:
			if (f->on) {
				f->next -= dt;
				if (f->next <= 0) {
					do_spark(f);
					f32 d = kvf(f->ent, "maxdelay", 0);
					f->next = 0.1f + frange(0, d > 0 ? d : 1.0f);
				}
			}
			break;
		case FK_BEAM:
			if (f->on && now >= f->next) {
				f32 life = kvf(f->ent, "life", 0);
				kill_arcs(f->ent);
				beam_strike(f);
				if (life <= 0) {
					f->next = now + 0.05f; /* continuous: a new bolt shape every frame or so */
				} else {
					f32 st = kvf(f->ent, "striketime", 1);
					f->next = now + life + ((e->spawnflags & 4) ? frange(0, st) : st);
				}
			}
			break;
		case FK_TESLA:
			if (f->on) {
				f->next -= dt;
				if (f->next <= 0) {
					tesla_spark(f);
					f->next = frange(kvf(f->ent, "interval_min", 0.5f), kvf(f->ent, "interval_max", 2.0f));
				}
			}
			break;
		case FK_SHOOTER:
			shooter_tick(f, dt);
			break;
		case FK_FIRE:
			fire_tick(f, dt);
			break;
		case FK_GUNFIRE:
			gunfire_tick(f, dt);
			break;
		case FK_SPEAKER:
			speaker_tick(f);
			break;
		case FK_HURT:
			hurt_tick(f);
			break;
		default:
			break;
		}
	}
	for (int i = 0; i < MAX_ARCS; i++)
		if (arcs[i].alive && now >= arcs[i].until) {
			Fx *f = fx_of(arcs[i].owner);
			/* a continuous beam keeps its bolt until the next one replaces it */
			if (!(f && f->kind == FK_BEAM && f->on && kvf(f->ent, "life", 0) <= 0))
				arcs[i].alive = 0;
		}
	ropes_tick(dt);
}

int EnvEnts_Input(s32 ent, const char *in, const char *param, s32 activator)
{
	Fx *f = fx_of(ent);
	if (!f)
		return 0;
	Entity *e = E(ent);
	switch (f->kind) {
	case FK_SPEAKER:
		if (!strcasecmp(in, "turnon")) {
			f->on = 1;
			f->next = Ent_Now() + 0.1f;
		} else if (!strcasecmp(in, "turnoff"))
			f->on = 0;
		else if (!strcasecmp(in, "toggle")) {
			f->on = !f->on;
			f->next = Ent_Now() + 0.1f;
		} else
			return 0;
		return 1;
	case FK_HURT:
		f->left = activator;
		if (!strcasecmp(in, "turnon")) {
			f->on = 1;
			f->next = Ent_Now() + 0.1f;
		} else if (!strcasecmp(in, "turnoff"))
			f->on = 0;
		else if (!strcasecmp(in, "toggle"))
			f->on = !f->on;
		else if (!strcasecmp(in, "hurt"))
			hurt_now(f);
		else
			return 0;
		return 1;
	case FK_SPARK:
		if (!strcasecmp(in, "startspark") || !strcasecmp(in, "turnon")) {
			f->on = 1;
			f->next = 0.1f;
		} else if (!strcasecmp(in, "stopspark") || !strcasecmp(in, "turnoff"))
			f->on = 0;
		else if (!strcasecmp(in, "togglespark"))
			f->on = !f->on;
		else if (!strcasecmp(in, "sparkonce"))
			do_spark(f);
		else
			return 0;
		return 1;
	case FK_EXPLOSION:
		if (!strcasecmp(in, "explode")) {
			do_explosion(f);
			return 1;
		}
		return 0;
	case FK_PHYSEXPLOSION:
		if (!strcasecmp(in, "explode") || !strcasecmp(in, "explodeandremove")) {
			do_physexplosion(f);
			if (!strcasecmp(in, "explodeandremove"))
				e->flags |= EF_KILLED;
			return 1;
		}
		return 0;
	case FK_PHYSIMPACT:
		if (!strcasecmp(in, "impact")) {
			do_physimpact(f);
			return 1;
		}
		return 0;
	case FK_BEAM:
		if (!strcasecmp(in, "turnon"))
			f->on = 1, f->next = 0;
		else if (!strcasecmp(in, "turnoff"))
			f->on = 0, kill_arcs(ent);
		else if (!strcasecmp(in, "toggle")) {
			f->on = !f->on;
			f->next = 0;
			if (!f->on)
				kill_arcs(ent);
		} else if (!strcasecmp(in, "strikeonce"))
			beam_strike(f);
		else
			return !strcasecmp(in, "amplitude") || !strcasecmp(in, "width") || !strcasecmp(in, "alpha") ||
			       !strcasecmp(in, "color") || !strcasecmp(in, "scrollspeed");
		return 1;
	case FK_TESLA:
		if (!strcasecmp(in, "turnon"))
			f->on = 1;
		else if (!strcasecmp(in, "turnoff"))
			f->on = 0;
		else if (!strcasecmp(in, "dospark"))
			tesla_spark(f);
		else
			return 0;
		return 1;
	case FK_SHOOTER:
		if (!strcasecmp(in, "shoot")) {
			f->left = atoi(kv(ent, "m_igibs"));
			if (f->left <= 0)
				f->left = 1;
			f->next = Ent_Now();
			return 1;
		}
		return 0;
	case FK_FIRE:
		if (!strcasecmp(in, "startfire") || (!strcasecmp(in, "enable") && f->fire > 0)) {
			if (!f->on) {
				f->on = 1;
				f->fireLeft = kvf(ent, "health", 30);
				Ent_FireOutput(ent, "onignited", ent);
				const Pack *P = Ent_Pack();
				if (P && e->esnd[0] >= 0 && (u32)e->esnd[0] < P->numSounds)
					f->loop = Audio_Play(&P->sounds[e->esnd[0]], e->origin, 1.0f, SNDLVL_DEFAULT);
			}
		} else if (!strcasecmp(in, "extinguish") || !strcasecmp(in, "extinguishtemporary") ||
		           !strcasecmp(in, "disable")) {
			if (f->on)
				Ent_FireOutput(ent, "onextinguished", ent);
			f->on = 0;
		} else {
			return 0;
		}
		return 1;
	case FK_SPLASH:
		if (!strcasecmp(in, "splash")) {
			f32 at[3];
			memcpy(at, e->origin, 12);
			if (e->spawnflags & 1) { /* "Automatically find water surface" */
				f32 down[3] = {at[0], at[1], at[2] - 4096};
				for (f32 z = at[2]; z > down[2]; z -= 8) {
					f32 p[3] = {at[0], at[1], z};
					if (CM_PointContents(p) & MASK_WATER) {
						at[2] = z + 8;
						break;
					}
				}
			}
			Weap_Splash(at, kvf(ent, "scale", 8) / 8.0f);
			play_esnd(ent, at, 1.0f);
			return 1;
		}
		return 0;
	case FK_MUZZLE:
		if (!strcasecmp(in, "fire")) {
			Weap_Muzzle(e->origin, kvf(ent, "scale", 1));
			return 1;
		}
		return 0;
	case FK_GUNFIRE:
		if (!strcasecmp(in, "enable"))
			f->on = 1;
		else if (!strcasecmp(in, "disable"))
			f->on = 0;
		else
			return 0;
		return 1;
	case FK_DUST:
		if (!strcasecmp(in, "spawndust")) {
			f32 dir[3];
			forward_of(ent, dir);
			Weap_Dust(e->origin, dir, kvf(ent, "scale", 8) / 8.0f, colour(ent, "color", 0x80806000u), kvf(ent, "speed", 16), 8);
			return 1;
		}
		return 0;
	case FK_AR2EXPLOSION:
		if (!strcasecmp(in, "explode")) {
			for (int k = 0; k < 3; k++) {
				f32 d[3] = {frange(-1, 1), frange(-1, 1), frange(0, 1)};
				Weap_Dust(e->origin, d, 4, 0x40404800u, 120, 6);
			}
			return 1;
		}
		return 0;
	case FK_PHYSBOX:
		if (!strcasecmp(in, "enablemotion")) {
			Phys_SetMotion(ent, 1);
			Ent_FireOutput(ent, "onmotionenabled", activator);
		} else if (!strcasecmp(in, "disablemotion"))
			Phys_SetMotion(ent, 0);
		else if (!strcasecmp(in, "wake"))
			Phys_Wake(ent);
		else if (!strcasecmp(in, "sleep") || !strcasecmp(in, "forcedrop"))
			; /* (asleep until touched; dropped by the player's hands) */
		else
			return 0;
		return 1;
	case FK_CONSTRAINT: {
		int r = Phys_ConstraintInput(ent, in);
		if (r == 2)
			Phys_ConstraintValue(ent, in, (f32)atof(param ? param : "0"));
		return r != 0;
	}
	case FK_ROPE:
		return rope_input(ent, in);
	}
	return 0;
}

int EnvEnts_NumFires(void) { return numFx; }

int EnvEnts_Fire(int i, FireDraw *out)
{
	if (i < 0 || i >= numFx || fxl[i].kind != FK_FIRE || fxl[i].fire <= 0)
		return 0;
	Entity *e = E(fxl[i].ent);
	if (e->flags & EF_KILLED)
		return 0;
	memcpy(out->pos, e->origin, 12);
	out->size = kvf(fxl[i].ent, "firesize", 64) * fxl[i].fire;
	out->alpha = fxl[i].fire;
	out->phase = (f32)fxl[i].ent * 0.37f;
	return 1;
}

int EnvEnts_FireOn(s32 ent)
{
	Fx *f = fx_of(ent);
	return f && f->kind == FK_FIRE && f->on;
}

int EnvEnts_SparkCount(void) { return sparks; }

void EnvEnts_Tracer(s32 owner, const f32 from[3], const f32 to[3], u32 rgba, f32 width)
{
	Arc *ar = new_arc(owner);
	if (!ar)
		return;
	ar->until = Ent_Now() + 0.05f;
	ar->d.n = 2;
	memcpy(ar->d.pts[0], from, 12);
	memcpy(ar->d.pts[1], to, 12);
	ar->d.width = width;
	ar->d.rgba = rgba;
	ar->d.tex = BEAM_LASER;
}
