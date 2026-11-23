#include "gibs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "collide.h"
#include "ent.h"
#ifndef HOST_TEST
#include "render.h"
#endif

#define MAX_GIBS 64
#define GIB_GRAVITY 600.0f

typedef struct {
	u8 alive, resting;
	u16 model;
	f32 pos[3], vel[3];
	f32 q[4], w[3];
	f32 half;
	f32 off[3];       /* the piece's centre in its model (pieces share their parent's origin) */
	f32 life, fade;
	u8 tint[4];
} Gib;

static const Pack *P;
static Gib gibs[MAX_GIBS];

void Gibs_Init(const Pack *p)
{
	P = p;
	Gibs_Clear();
}

void Gibs_Clear(void) { memset(gibs, 0, sizeof(gibs)); }

int Gibs_Count(void)
{
	int n = 0;
	for (int i = 0; i < MAX_GIBS; i++)
		n += gibs[i].alive;
	return n;
}

static f32 frand(void) { return (f32)(rand() & 0xFFFF) / 65535.0f; }

static void quat_mul(const f32 a[4], const f32 b[4], f32 o[4])
{
	f32 r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
	            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
	            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
	memcpy(o, r, 16);
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
	} else {
		q[0] = q[1] = q[2] = 0;
		q[3] = 1;
	}
}

static Gib *new_gib(void)
{
	Gib *oldest = NULL;
	for (int i = 0; i < MAX_GIBS; i++) {
		if (!gibs[i].alive)
			return &gibs[i];
		if (!oldest || gibs[i].life > oldest->life)
			oldest = &gibs[i];
	}
	return oldest; /* too many: the oldest piece goes, like Source's gib limit */
}

static void spawn(u16 model, const f32 pos[3], const f32 R[3][3], const f32 vel[3], f32 fade, const u8 tint[4])
{
	if (!P || model >= P->numModels)
		return;
	Gib *g = new_gib();
	memset(g, 0, sizeof(*g));
	g->alive = 1;
	g->model = model;
	memcpy(g->pos, pos, 12);
	if (R)
		R_to_quat(R, g->q);
	else
		g->q[3] = 1;
	const PackModel *m = &P->models[model];
	f32 h = 0;
	for (int k = 0; k < 3; k++) {
		f32 e = (m->maxs[k] - m->mins[k]) * 0.5f;
		h = e > h ? e : h;
		g->off[k] = (m->maxs[k] + m->mins[k]) * 0.5f;
	}
	f32 Rq[3][3];
	quat_to_R(g->q, Rq);
	for (int k = 0; k < 3; k++)
		g->pos[k] = pos[k] + Rq[k][0] * g->off[0] + Rq[k][1] * g->off[1] + Rq[k][2] * g->off[2];
	g->half = h < 1 ? 1 : h > 8 ? 8 : h * 0.6f;
	/* Source's BreakModel: the parent's velocity plus a random burst and spin */
	for (int k = 0; k < 3; k++) {
		g->vel[k] = (vel ? vel[k] : 0) + (frand() - 0.5f) * 160.0f;
		g->w[k] = (frand() - 0.5f) * 12.0f;
	}
	g->vel[2] += 60.0f + frand() * 80.0f;
	g->fade = fade > 0 ? fade : 20.0f;
	memcpy(g->tint, tint ? tint : (const u8[4]){128, 128, 128, 255}, 4);
}

void Gibs_BreakModel(s32 model, const f32 centre[3], const f32 R[3][3], const f32 vel[3], const u8 tint[4])
{
	if (!P || model < 0)
		return;
	for (u32 i = 0; i < P->numGibSets; i++) {
		const PackGibSet *s = &P->gibSets[i];
		if (s->model != model)
			continue;
		for (u32 k = 0; k < s->count; k++) {
			u16 gm = P->gibModels[s->first + k];
			/* each piece where it sat in the whole thing: its model is built around the parent's origin */
			spawn(gm, centre, R, vel, s->fade, tint);
		}
		return;
	}
}

void Gibs_Throw(s32 model, const f32 at[3], const f32 R[3][3], const f32 vel[3], f32 life, const u8 tint[4])
{
	if (!P || model < 0 || (u32)model >= P->numModels)
		return;
	spawn((u16)model, at, R, vel, life, tint);
}

void Gibs_BreakBox(int material, const f32 mins[3], const f32 maxs[3], const f32 vel[3], const u8 tint[4])
{
	if (!P)
		return;
	const PackGibSet *s = NULL;
	for (u32 i = 0; i < P->numGibSets && !s; i++)
		if (P->gibSets[i].model == material)
			s = &P->gibSets[i];
	if (!s || !s->count)
		return;
	/* func_breakable: (xy + yz + zx area) / (3 * 12 * 12) pieces, at most 15 (func_break_max_pieces) */
	f32 sz[3] = {maxs[0] - mins[0], maxs[1] - mins[1], maxs[2] - mins[2]};
	int n = (int)((sz[0] * sz[1] + sz[1] * sz[2] + sz[2] * sz[0]) / (3 * 12 * 12));
	n = n < 1 ? 1 : n > 15 ? 15 : n;
	for (int i = 0; i < n; i++) {
		f32 p[3];
		for (int k = 0; k < 3; k++)
			p[k] = mins[k] + frand() * sz[k];
		f32 ang[3] = {frand() * 360, frand() * 360, frand() * 360}, R[3][3];
		Ent_AngleMatrix(ang, R);
		u16 gm = P->gibModels[s->first + (u32)(rand() % s->count)];
		/* generic chunks are centred on their origin; place the centre in the box */
		const PackModel *m = &P->models[gm];
		for (int k = 0; k < 3; k++)
			p[k] -= R[k][0] * (m->mins[0] + m->maxs[0]) * 0.5f + R[k][1] * (m->mins[1] + m->maxs[1]) * 0.5f +
			        R[k][2] * (m->mins[2] + m->maxs[2]) * 0.5f;
		spawn(gm, p, R, vel, s->fade, tint);
	}
}

void Gibs_Tick(f32 dt)
{
	for (int i = 0; i < MAX_GIBS; i++) {
		Gib *g = &gibs[i];
		if (!g->alive)
			continue;
		g->life += dt;
		if (g->life > g->fade) {
			g->alive = 0;
			continue;
		}
		if (g->resting)
			continue;
		int wet = (CM_PointContents(g->pos) & MASK_WATER) != 0;
		g->vel[2] -= GIB_GRAVITY * dt * (wet ? 0.3f : 1.0f);
		if (wet)
			for (int k = 0; k < 3; k++)
				g->vel[k] *= 1.0f - (dt * 3 > 0.9f ? 0.9f : dt * 3);
		f32 to[3] = {g->pos[0] + g->vel[0] * dt, g->pos[1] + g->vel[1] * dt, g->pos[2] + g->vel[2] * dt};
		f32 e = g->half > 4 ? 4 : g->half;
		f32 mn[3] = {-e, -e, -e}, mx[3] = {e, e, e};
		Trace tr;
		const s32 mask = CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW | CONTENTS_GRATE;
		CM_BoxTrace(&tr, g->pos, to, mn, mx, mask);
		if (tr.startsolid) {
			/* pieces spawn overlapping what they broke off (a wall the board was nailed to, the
			 * crate next to it): Source's physics pushes them apart. Try just the centre, then
			 * just the world; if even the centre is buried, let it drift free until it's out. */
			static const f32 z0[3] = {0, 0, 0};
			CM_BoxTrace(&tr, g->pos, to, z0, z0, mask);
			if (tr.startsolid)
				CM_BoxTrace(&tr, g->pos, to, z0, z0, CONTENTS_SOLID);
			if (tr.startsolid) {
				if (g->life > 1.0f) {
					g->resting = 1;
					continue;
				}
				memcpy(tr.endpos, to, 12);
				tr.fraction = 1;
			}
		}
		memcpy(g->pos, tr.endpos, 12);
		if (tr.fraction < 1) {
			/* bounce: lose most of the push into the surface, slide a bit along it */
			f32 vn = g->vel[0] * tr.normal[0] + g->vel[1] * tr.normal[1] + g->vel[2] * tr.normal[2];
			for (int k = 0; k < 3; k++) {
				f32 n = vn * tr.normal[k];
				g->vel[k] = (g->vel[k] - n) * 0.6f - n * 0.25f;
				g->w[k] *= 0.6f;
			}
			f32 sp = sqrtf(g->vel[0] * g->vel[0] + g->vel[1] * g->vel[1] + g->vel[2] * g->vel[2]);
			if (sp < 30 && tr.normal[2] > 0.7f)
				g->resting = 1;
		}
		/* spin */
		f32 a = sqrtf(g->w[0] * g->w[0] + g->w[1] * g->w[1] + g->w[2] * g->w[2]) * dt;
		if (a > 1e-4f) {
			f32 s = sinf(a * 0.5f) / (a / dt);
			f32 dq[4] = {g->w[0] * s, g->w[1] * s, g->w[2] * s, cosf(a * 0.5f)};
			quat_mul(dq, g->q, g->q);
			f32 l = sqrtf(g->q[0] * g->q[0] + g->q[1] * g->q[1] + g->q[2] * g->q[2] + g->q[3] * g->q[3]);
			for (int k = 0; k < 4; k++)
				g->q[k] /= l;
		}
	}
}

#ifndef HOST_TEST
void Gibs_Draw(Mtx view)
{
	for (int i = 0; i < MAX_GIBS; i++) {
		Gib *g = &gibs[i];
		if (!g->alive)
			continue;
		f32 R[3][3], o[3];
		quat_to_R(g->q, R);
		for (int k = 0; k < 3; k++)
			o[k] = g->pos[k] - (R[k][0] * g->off[0] + R[k][1] * g->off[1] + R[k][2] * g->off[2]);
		Render_Model(P, g->model, o, R, g->tint, view);
	}
}
#endif

/* for tests: piece i's centre, whether it has settled, and its age (0 = no piece there) */
int Gibs_Probe(int i, f32 pos[3], int *resting, f32 *life)
{
	if (i < 0 || i >= MAX_GIBS || !gibs[i].alive)
		return 0;
	memcpy(pos, gibs[i].pos, 12);
	*resting = gibs[i].resting;
	*life = gibs[i].life;
	return 1;
}
