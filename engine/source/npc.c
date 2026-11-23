#include "npc.h"
#include "relations.h"
#include "aients.h"
#include "physics.h"

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "collide.h"
#include "ent.h"
#include "scene.h"
#include "weapons.h"

#define MAX_NPCS 256
static int rag_start(Npc *n, const f32 push[3]);
static f32 npcClock; /* NPC time: sight checks, step-aside returns */
#define FADE_TIME 0.2f
#define TURN_RATE 300.0f
#define GRAVITY 600.0f
#define STEP 18.0f
#define WALK_SPEED 80.0f
#define RUN_SPEED 210.0f

static const Pack *P;
static Player *player;
static Npc *npcs;
static u32 numNpcs, npcCap;
#define NPC_SPARE 64
static s16 *entToNpc;
static u32 entCount;

static const f32 hullMins[3] = {-13, -13, 0};

static const char *pstr(u32 off) { return P->entStrings + off; }

Npc *Npc_List(u32 *count)
{
	*count = numNpcs;
	return npcs;
}

Npc *Npc_OfEntity(s32 ent)
{
	if (ent < 0 || (u32)ent >= entCount || entToNpc[ent] < 0)
		return NULL;
	return &npcs[entToNpc[ent]];
}

static Entity *ent_of(const Npc *n)
{
	u32 c;
	Entity *e = Ent_List(&c);
	return &e[n->ent];
}

void Npc_EntPos(s32 ent, f32 out[3])
{
	if (ent == 0) {
		memcpy(out, player->origin, 12);
		return;
	}
	u32 c;
	Entity *e = Ent_List(&c);
	if (ent > 0 && (u32)ent < c)
		memcpy(out, e[ent].origin, 12);
}

static const PackNpcModel *model_of(const Npc *n) { return &P->npcModels[n->model]; }

/* where to look at an entity, like Source's EyePosition: the player's and NPCs' eyes, a point entity's
 * (info_target, a mark) origin itself. (Adding a standing eye height to every target had NPCs
 * looking 64 units over the marks their scenes point them at: up at the ceiling or the sky.) */
void Npc_EyePos(s32 ent, f32 out[3])
{
	Npc_EntPos(ent, out);
	if (ent == 0) {
		out[2] += Player_EyeHeight(player);
		return;
	}
	const Npc *n = Npc_OfEntity(ent);
	if (n && !n->prop)
		out[2] += model_of(n)->eyeHeight > 0 ? model_of(n)->eyeHeight : 64.0f;
}

static int flies(const char *cn)
{
	return !strcmp(cn, "npc_cscanner") || !strcmp(cn, "npc_manhack") || !strcmp(cn, "npc_combinegunship") ||
	       !strcmp(cn, "npc_helicopter") || !strcmp(cn, "npc_combinedropship") || !strcmp(cn, "npc_clawscanner") ||
	       !strcmp(cn, "npc_barnacle") || /* hangs from the ceiling */
	       !strcmp(cn, "npc_combine_camera") || !strcmp(cn, "npc_turret_ceiling") || !strcmp(cn, "npc_turret_ground") ||
	       /* (moved by their own code: objects.c, mines.c) */
	       !strcmp(cn, "env_headcrabcanister") || !strcmp(cn, "combine_mine") || !strcmp(cn, "prop_vehicle_apc");
}

static f32 hull_height(const Npc *n)
{
	f32 h = model_of(n)->height;
	return h < 8 ? 8 : (h > 80 ? 72 : h);
}

/* ------------------------------------------------------------------ sequences */

static const PackSeq *seq_ptr(const Npc *n, s16 s)
{
	if (s < 0 || s >= n->skel->numSeqs)
		return NULL;
	return &P->npcSeqs[n->skel->firstSeq + (u32)s];
}

static f32 seq_duration(const PackSeq *s)
{
	if (!s || s->numFrames < 2 || s->fps <= 0)
		return 0.1f;
	return (f32)(s->numFrames - 1) / s->fps;
}

static s16 find_seq(const Npc *n, const char *name)
{
	if (!name || !*name)
		return -1;
	if (!strncasecmp(name, "ACT_", 4)) {
		if (!strcasecmp(name, "ACT_IDLE"))
			return n->skel->seqIdle;
		if (!strcasecmp(name, "ACT_WALK"))
			return n->skel->seqWalk;
		if (!strcasecmp(name, "ACT_RUN"))
			return n->skel->seqRun;
		if (!strcasecmp(name, "ACT_DIESIMPLE"))
			return n->skel->seqDie;
	}
	for (u16 i = 0; i < n->skel->numSeqs; i++)
		if (!strcasecmp(pstr(P->npcSeqs[n->skel->firstSeq + i].name), name))
			return (s16)i;
	return -1;
}

static void set_seq(Npc *n, s16 s, int restart)
{
	if (s < 0)
		return;
	if (s == n->seq && !restart)
		return;
	n->prevSeq = n->seq;
	n->prevT = n->t;
	n->blend = n->prevSeq >= 0 ? 0.0f : 1.0f;
	n->seq = s;
	n->t = 0;
	const PackSeq *sp = seq_ptr(n, s);
	n->hold = sp && !(sp->flags & SEQF_LOOP);
	n->seqDuration = seq_duration(sp);
}

f32 Npc_PlaySequence(Npc *n, const char *name, int loop)
{
	s16 s = find_seq(n, name);
	if (s < 0)
		return -1;
	set_seq(n, s, 1);
	n->locked = 1;
	if (loop)
		n->hold = 0;
	return n->seqDuration;
}

static f32 npcClockNow(void);
static void locomotion_seq(Npc *n)
{
	/* (blocked: it stands and waits, as the PC's NPCs do, rather than walking on the spot) */
	int going = n->moving && n->moveFrac > 0.2f;
	s16 s = going ? (n->running && n->skel->seqRun >= 0 ? n->skel->seqRun : n->skel->seqWalk) : n->skel->seqIdle;
	if (n->combatUntil > npcClockNow() && n->actsReady) {
		/* fighting: the attack sequence plays out; otherwise the angry idle and the aimed run / walk */
		if (n->fireBase && n->fireDur > 0)
			return;
		s16 c = going ? (n->running ? n->actRunAim : n->actWalkAim) : n->actAngry;
		if (c >= 0)
			s = c;
	}
	if (s < 0)
		s = n->skel->seqIdle;
	if (s >= 0 && s != n->seq)
		set_seq(n, s, 0);
	if (s >= 0)
		n->hold = 0;
}

static void death_drops(Npc *n);

int Npc_Kill(Npc *n)
{
	if (n->dead)
		return 0;
	death_drops(n);
	n->dead = 1;
	n->moving = 0;
	n->followEnt = -1;
	n->script = -1;
	if (n->voice) {
		Audio_Stop(n->voice);
		n->voice = 0;
	}
	Npc_ClearGestures(n);
	n->lookW = 0;
	/* Source: characters become ragdolls (only things like scanners have death animations of their own) */
	{
		f32 push[3] = {n->deathPush[0], n->deathPush[1], n->deathPush[2] + 40.0f};
		if (rag_start(n, push))
			return 1;
	}
	if (n->skel->seqDie >= 0) {
		set_seq(n, n->skel->seqDie, 1);
		n->locked = 1;
		n->hold = 1;
		return 1;
	}
	return 0; /* no death animation: the caller removes it */
}

void Npc_PropAnimation(Npc *n, const char *name, int asDefault)
{
	s16 s = find_seq(n, name);
	if (s < 0)
		return;
	if (asDefault) {
		n->defaultSeq = s;
		if (n->locked == 2)
			return; /* finish the one-shot first */
		set_seq(n, s, 0);
		n->hold = 0;
		n->locked = 1;
		return;
	}
	set_seq(n, s, 1);
	const PackSeq *sp = seq_ptr(n, s);
	n->hold = sp && !(sp->flags & SEQF_LOOP);
	n->locked = n->hold ? 2 : 1;
}

void Npc_ResetSequence(Npc *n)
{
	n->locked = 0;
	locomotion_seq(n);
}

/* ------------------------------------------------------------------ pose */

static void decode_q(const PackBoneFrame *f, f32 q[4])
{
	q[0] = f->q[0] * (1.0f / 32767.0f);
	q[1] = f->q[1] * (1.0f / 32767.0f);
	q[2] = f->q[2] * (1.0f / 32767.0f);
	f32 w = 1.0f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
	q[3] = w > 0 ? sqrtf(w) : 0;
}

static void nlerp(const f32 a[4], const f32 b[4], f32 t, f32 out[4])
{
	f32 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	f32 s = d < 0 ? -t : t;
	f32 r[4], len = 0;
	for (int k = 0; k < 4; k++) {
		r[k] = a[k] * (1 - t) + b[k] * s;
		len += r[k] * r[k];
	}
	len = len > 1e-12f ? 1.0f / sqrtf(len) : 1.0f;
	for (int k = 0; k < 4; k++)
		out[k] = r[k] * len;
}

/* ---- Source's AccumulatePose / SlerpBones / AddSequenceLayers (bone_setup.cpp), on baked frames */

static void qmul(const f32 a[4], const f32 b[4], f32 o[4])
{
	f32 r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
	            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
	            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
	memcpy(o, r, sizeof(r));
}

/* QuaternionScale: the rotation's angle times s */
static void qscale(const f32 q[4], f32 s, f32 o[4])
{
	f32 sinom = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
	if (sinom > 1)
		sinom = 1;
	f32 sinsom = sinf(asinf(sinom) * s);
	f32 k = sinsom / (sinom + 1.192092896e-07f);
	f32 r = 1.0f - sinsom * sinsom;
	r = r > 0 ? sqrtf(r) : 0;
	o[0] = q[0] * k;
	o[1] = q[1] * k;
	o[2] = q[2] * k;
	o[3] = q[3] < 0 ? -r : r;
}

static f32 simple_spline(f32 v)
{
	f32 v2 = v * v;
	return 3 * v2 - 2 * v2 * v;
}

static void accumulate(const Pack *pk, const PackSkel *sk, s16 s, f32 cycle, f32 weight, f32 time,
                       f32 (*q)[4], f32 (*pos)[3], int depth)
{
	if (s < 0 || s >= sk->numSeqs || weight <= 0)
		return;
	const PackSeq *sp = &pk->npcSeqs[sk->firstSeq + (u32)s];
	u32 F = sp->numFrames;
	if (sp->flags & SEQF_REALTIME) {
		f32 cps = (F > 1 && sp->fps > 0) ? sp->fps / (f32)(F - 1) : 0;
		cycle = time * cps;
		cycle -= floorf(cycle);
	}
	if (sp->numBones && F && pk->npcMasks) {
		u32 a = 0, b = 0;
		f32 w = 0;
		if (F > 1) {
			f32 f = cycle * (f32)(F - 1);
			if (f < 0)
				f = 0;
			if (f >= (f32)(F - 1)) {
				a = b = F - 1;
			} else {
				a = (u32)f;
				b = a + 1;
				w = f - (f32)a;
			}
		}
		u32 nm = sp->numBones;
		const PackSeqMask *m = &pk->npcMasks[sp->firstMask];
		const PackBoneFrame *fa = &pk->npcFrames[sp->firstFrame + a * nm];
		const PackBoneFrame *fb = &pk->npcFrames[sp->firstFrame + b * nm];
		for (u32 j = 0; j < nm; j++) {
			u32 bi = m[j].bone;
			if (bi >= sk->numBones || bi >= NPC_MAX_BONES)
				continue;
			f32 s2 = weight * (f32)m[j].weight * (1.0f / 255.0f);
			if (s2 <= 0)
				continue;
			f32 qa[4], qb[4], q2[4], p2[3];
			decode_q(&fa[j], qa);
			decode_q(&fb[j], qb);
			nlerp(qa, qb, w, q2);
			for (int k = 0; k < 3; k++)
				p2[k] = ((f32)fa[j].p[k] * (1 - w) + (f32)fb[j].p[k] * w) * (1.0f / 64.0f);
			if (sp->flags & SEQF_DELTA) {
				f32 d[4];
				qscale(q2, s2 > 1 ? 1 : s2, d);
				if (sp->flags & SEQF_POST)
					qmul(q[bi], d, q[bi]); /* QuaternionMA */
				else
					qmul(d, q[bi], q[bi]); /* QuaternionSM */
				for (int k = 0; k < 3; k++)
					pos[bi][k] += p2[k] * s2;
			} else if (s2 >= 1) {
				memcpy(q[bi], q2, sizeof(q2));
				memcpy(pos[bi], p2, sizeof(p2));
			} else {
				nlerp(q[bi], q2, s2, q[bi]);
				for (int k = 0; k < 3; k++)
					pos[bi][k] = pos[bi][k] * (1 - s2) + p2[k] * s2;
			}
		}
	}
	if (depth >= 4 || !sp->numLayers || !pk->npcLayers)
		return;
	for (u32 i = 0; i < sp->numLayers; i++) {
		const PackSeqLayer *L = &pk->npcLayers[sp->firstLayer + i];
		f32 lw = weight, lc = cycle;
		if (L->start != L->end) {
			f32 idx = cycle, k = 1.0f;
			if (idx < L->start || idx >= L->end)
				continue;
			if (idx < L->peak && L->start != L->peak)
				k = (idx - L->start) / (L->peak - L->start);
			else if (idx > L->tail && L->end != L->tail)
				k = (L->end - idx) / (L->end - L->tail);
			if (L->flags & ALF_SPLINE)
				k = simple_spline(k);
			if ((L->flags & ALF_XFADE) && idx > L->tail)
				lw = (k * weight) / (1 - weight + k * weight);
			else if (L->flags & ALF_NOBLEND)
				lw = k;
			else
				lw = weight * k;
			lc = (cycle - L->start) / (L->end - L->start);
		}
		accumulate(pk, sk, L->seq, lc, lw, time, q, pos, depth + 1);
	}
}

/* main sequence cycle from its time */
static f32 seq_cycle(const PackSeq *sp, f32 t, int hold)
{
	if (!sp || sp->numFrames < 2 || sp->fps <= 0)
		return 0;
	f32 c = t * sp->fps / (f32)(sp->numFrames - 1);
	if (hold)
		return c > 1 ? 1 : (c < 0 ? 0 : c);
	c -= floorf(c);
	return c;
}

static void rest_pose(const Pack *pk, const PackSkel *sk, u32 nb, f32 (*q)[4], f32 (*pos)[3])
{
	for (u32 i = 0; i < nb; i++) {
		if (pk->npcRest) {
			const PackBoneFrame *r = &pk->npcRest[sk->firstBone + i];
			decode_q(r, q[i]);
			for (int k = 0; k < 3; k++)
				pos[i][k] = (f32)r->p[k] * (1.0f / 64.0f);
		} else {
			q[i][0] = q[i][1] = q[i][2] = 0;
			q[i][3] = 1;
			pos[i][0] = pos[i][1] = pos[i][2] = 0;
		}
	}
}

static void seq_pose(const Pack *pk, const PackSkel *sk, s16 s, f32 t, int hold, u32 nb, f32 (*q)[4], f32 (*pos)[3])
{
	rest_pose(pk, sk, nb, q, pos);
	if (s < 0 || s >= sk->numSeqs)
		return;
	const PackSeq *sp = &pk->npcSeqs[sk->firstSeq + (u32)s];
	accumulate(pk, sk, s, seq_cycle(sp, t, hold), 1.0f, t, q, pos, 0);
}

/* rotation by angle (radians) about a unit axis, as a 3x3 */
static void axis_rot(const f32 ax[3], f32 a, f32 R[3][3])
{
	f32 c = cosf(a), s = sinf(a), t = 1 - c, x = ax[0], y = ax[1], z = ax[2];
	R[0][0] = t * x * x + c;     R[0][1] = t * x * y - s * z; R[0][2] = t * x * z + s * y;
	R[1][0] = t * x * y + s * z; R[1][1] = t * y * y + c;     R[1][2] = t * y * z - s * x;
	R[2][0] = t * x * z - s * y; R[2][1] = t * y * z + s * x; R[2][2] = t * z * z + c;
}

static f32 world[NPC_MAX_BONES][3][4];

void Anim_PoseEx(const Pack *pk, const PackSkel *sk, s16 seq, f32 t, int hold, s16 prevSeq, f32 prevT, f32 blend,
                 const AnimExtra *ex, f32 (*out)[3][4])
{
	static f32 q[NPC_MAX_BONES][4], p[NPC_MAX_BONES][3], q2[NPC_MAX_BONES][4], p2[NPC_MAX_BONES][3];
	u32 nb = sk->numBones;
	if (nb > NPC_MAX_BONES)
		nb = NPC_MAX_BONES;
	seq_pose(pk, sk, seq, t, hold, nb, q, p);
	if (blend < 1.0f && prevSeq >= 0 && prevSeq < sk->numSeqs) {
		const PackSeq *ps = &pk->npcSeqs[sk->firstSeq + (u32)prevSeq];
		seq_pose(pk, sk, prevSeq, prevT, !(ps->flags & SEQF_LOOP), nb, q2, p2);
		for (u32 i = 0; i < nb; i++) {
			nlerp(q2[i], q[i], blend, q[i]);
			for (int k = 0; k < 3; k++)
				p[i][k] = p2[i][k] * (1 - blend) + p[i][k] * blend;
		}
	}
	/* gesture layers (CBaseAnimatingOverlay), on top of the main sequence */
	if (ex)
		for (int g = 0; g < ex->numLayers; g++)
			accumulate(pk, sk, ex->layers[g].seq, ex->layers[g].cycle, ex->layers[g].weight, ex->layers[g].time, q, p,
			           0);
	/* aiming (cameras, turrets): aim_yaw / aim_pitch turn their bones through the blend's entries */
	if (ex && ex->aimOn && pk->skelAim && pk->aimCtls && pk->aimQuats && (u32)(sk - pk->skels) < pk->numSkelAim) {
		const u16 *ai = pk->skelAim[sk - pk->skels];
		for (u32 c = ai[0]; c < (u32)ai[0] + ai[1]; c++) {
			const PackAimCtl *ac = &pk->aimCtls[c];
			if (ac->bone >= nb || ac->count < 2)
				continue;
			f32 v = ac->param ? ex->aimPitch : ex->aimYaw;
			f32 t = ac->end != ac->start ? (v - ac->start) / (ac->end - ac->start) : 0;
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			f32 f = t * (f32)(ac->count - 1);
			int k = (int)f;
			if (k > ac->count - 2)
				k = ac->count - 2;
			f32 qa[4];
			nlerp(pk->aimQuats[ac->firstQuat + (u32)k], pk->aimQuats[ac->firstQuat + (u32)k + 1], f - (f32)k, qa);
			if (ac->delta) {
				/* delta layers: before the pose (QuaternionSM), or after it with STUDIO_POST (QuaternionMA) */
				f32 o[4];
				if (ac->delta == 2)
					qmul(q[ac->bone], qa, o);
				else
					qmul(qa, q[ac->bone], o);
				memcpy(q[ac->bone], o, 16);
			} else {
				memcpy(q[ac->bone], qa, 16);
			}
		}
	}
	/* look-at: spine, neck and head share the turn, in model space about each bone */
	s16 lookBone[3] = {-1, -1, -1};
	f32 lookShare[3] = {0.5f, 0.3f, 0.2f};
	f32 lookYaw = 0, lookPitch = 0;
	if (ex && ex->lookWeight > 0.001f && pk->skelLook) {
		const PackSkelLook *L = &pk->skelLook[sk - pk->skels];
		lookBone[0] = L->head;
		lookBone[1] = L->neck;
		lookBone[2] = L->spine;
		lookYaw = ex->lookYaw * ex->lookWeight * (f32)M_PI / 180.0f;
		lookPitch = ex->lookPitch * ex->lookWeight * (f32)M_PI / 180.0f;
		if (L->neck < 0) {
			lookShare[0] += lookShare[1];
			lookShare[1] = 0;
		}
		if (L->spine < 0) {
			lookShare[0] += lookShare[2];
			lookShare[2] = 0;
		}
	}
	const PackBone *bones = &pk->npcBones[sk->firstBone];
	for (u32 i = 0; i < nb; i++) {
		f32 x = q[i][0], y = q[i][1], z = q[i][2], w = q[i][3];
		f32 l[3][4] = {
			{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), p[i][0]},
			{2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), p[i][1]},
			{2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), p[i][2]},
		};
		s32 par = bones[i].parent;
		if (par >= 0 && (u32)par < i) {
			const f32(*pw)[4] = world[par];
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 4; c++)
					world[i][r][c] = pw[r][0] * l[0][c] + pw[r][1] * l[1][c] + pw[r][2] * l[2][c] + (c == 3 ? pw[r][3] : 0);
		} else {
			memcpy(world[i], l, sizeof(l));
		}
		for (int k = 0; k < 3; k++)
			if (lookBone[k] == (s16)i && lookShare[k] > 0) {
				/* model space: X forward, Z up. Yaw about Z, then pitch about the turned right axis */
				f32 R1[3][3], R2[3][3], R[3][3], M[3][3];
				const f32 zax[3] = {0, 0, 1};
				f32 yw = lookYaw * lookShare[k], pt = lookPitch * lookShare[k];
				axis_rot(zax, yw, R1);
				f32 ax[3] = {sinf(lookYaw), -cosf(lookYaw), 0}; /* forward x up: tilts forward toward up */
				axis_rot(ax, pt, R2);
				for (int r = 0; r < 3; r++)
					for (int c = 0; c < 3; c++)
						R[r][c] = R2[r][0] * R1[0][c] + R2[r][1] * R1[1][c] + R2[r][2] * R1[2][c];
				for (int r = 0; r < 3; r++)
					for (int c = 0; c < 3; c++)
						M[r][c] = R[r][0] * world[i][0][c] + R[r][1] * world[i][1][c] + R[r][2] * world[i][2][c];
				for (int r = 0; r < 3; r++)
					for (int c = 0; c < 3; c++)
						world[i][r][c] = M[r][c];
			}
		const f32(*pb)[4] = bones[i].poseToBone;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++)
				out[i][r][c] = world[i][r][0] * pb[0][c] + world[i][r][1] * pb[1][c] + world[i][r][2] * pb[2][c] +
				               (c == 3 ? world[i][r][3] : 0);
	}
}

void Anim_Pose(const Pack *pk, const PackSkel *sk, s16 seq, f32 t, int hold, s16 prevSeq, f32 prevT, f32 blend,
               f32 (*out)[3][4])
{
	Anim_PoseEx(pk, sk, seq, t, hold, prevSeq, prevT, blend, NULL, out);
}

f32 Anim_SeqDuration(const Pack *pk, const PackSkel *sk, s16 seq)
{
	if (seq < 0 || seq >= sk->numSeqs)
		return 0;
	return seq_duration(&pk->npcSeqs[sk->firstSeq + (u32)seq]);
}

s16 Anim_FindSeq(const Pack *pk, const PackSkel *sk, const char *name)
{
	for (u16 i = 0; i < sk->numSeqs; i++)
		if (!strcasecmp(pk->entStrings + pk->npcSeqs[sk->firstSeq + i].name, name))
			return (s16)i;
	return -1;
}

/* ---- ragdolls. Source turns a dead character into a physics ragdoll (CBaseAnimating::BecomeRagdoll):
 * every bone a body that falls and tumbles to rest against the world. Here the skeleton's joints are
 * point masses (Verlet) held at their bone lengths, with stiffening links to the grandparent so limbs
 * fold rather than collapse; each bone turns to follow its chain, the small ones (fingers, face) ride
 * along with their parent. Simulated in the character's own frame, which stays where it died. */
#define MAX_RAGDOLLS 12
#define RAG_MAX_SOLIDS 24
#define RAG_SUBSTEPS 3
#define RAG_ITERS 3
/* one solid: a rigid body (the .phy's solid) moving its bone. Its state is its centre of mass and rotation;
 * its bone sits at a fixed offset from that centre in its own frame */
typedef struct {
	u16 bone;
	s16 par;                       /* parent solid (index into s[]), -1 none */
	f32 x[3], v[3], q[4], w[3];    /* centre (character frame), velocity, rotation (x,y,z,w), angular velocity */
	f32 px[3], pq[4];              /* at the start of the substep */
	f32 tx[3], tq[4];              /* at the start of the tick (the world is met once a tick, from there) */
	f32 cm[3];                     /* centre of mass in the bone's own frame */
	f32 tip[3];                    /* the far end (its primary child's joint, or its own extent), bone frame */
	f32 anchor[3];                 /* its joint (its bone's origin) in the parent solid's bone frame */
	f32 rest[4];                   /* its bone's rotation in its parent's at bind (the limits are about this) */
	f32 lo[3], hi[3];              /* the ragdollconstraint's ranges about its own x, y, z (radians) */
	f32 im, ii;                    /* inverse mass, inverse inertia */
	f32 rad;
	u8 leaf;                       /* nothing hangs from it */
} RagSolid;
typedef struct {
	s32 ent;                       /* -1 free */
	u16 nb, ns;
	RagSolid s[RAG_MAX_SOLIDS];
	s8 solidOf[NPC_MAX_BONES];     /* bone -> its solid, -1 rides on its parent */
	f32 W0[NPC_MAX_BONES][3][4];   /* bone transforms (character frame) when it died */
	f32 W[NPC_MAX_BONES][3][4];    /* now */
	f32 still, age;
	u8 frozen;
} Ragdoll;
static Ragdoll *rags;

static Ragdoll *rag_of(s32 ent)
{
	if (!rags)
		return NULL;
	for (int i = 0; i < MAX_RAGDOLLS; i++)
		if (rags[i].ent == ent)
			return &rags[i];
	return NULL;
}

static void v_sub(const f32 a[3], const f32 b[3], f32 o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static f32 v_len(const f32 a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static f32 v_dot(const f32 a[3], const f32 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v_cross(const f32 a[3], const f32 b[3], f32 o[3])
{
	f32 t[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	memcpy(o, t, 12);
}

/* quaternions (x, y, z, w) */
static void q_mul(const f32 a[4], const f32 b[4], f32 o[4])
{
	f32 t[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
	memcpy(o, t, 16);
}
static void q_norm(f32 q[4])
{
	f32 l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l < 1e-8f) {
		q[0] = q[1] = q[2] = 0;
		q[3] = 1;
		return;
	}
	for (int k = 0; k < 4; k++)
		q[k] /= l;
}
static void q_conj(const f32 a[4], f32 o[4]) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void q_rot(const f32 q[4], const f32 v[3], f32 o[3])
{
	/* v + 2w(u x v) + 2u x (u x v) */
	f32 t[3], u[3] = {q[0], q[1], q[2]};
	v_cross(u, v, t);
	for (int k = 0; k < 3; k++)
		t[k] *= 2;
	f32 c[3];
	v_cross(u, t, c);
	for (int k = 0; k < 3; k++)
		o[k] = v[k] + q[3] * t[k] + c[k];
}
static void q_from_m(const f32 m[3][4], f32 q[4])
{
	f32 tr = m[0][0] + m[1][1] + m[2][2];
	if (tr > 0) {
		f32 s = sqrtf(tr + 1) * 2;
		q[3] = 0.25f * s; q[0] = (m[2][1] - m[1][2]) / s; q[1] = (m[0][2] - m[2][0]) / s; q[2] = (m[1][0] - m[0][1]) / s;
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		f32 s = sqrtf(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
		q[3] = (m[2][1] - m[1][2]) / s; q[0] = 0.25f * s; q[1] = (m[0][1] + m[1][0]) / s; q[2] = (m[0][2] + m[2][0]) / s;
	} else if (m[1][1] > m[2][2]) {
		f32 s = sqrtf(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
		q[3] = (m[0][2] - m[2][0]) / s; q[0] = (m[0][1] + m[1][0]) / s; q[1] = 0.25f * s; q[2] = (m[1][2] + m[2][1]) / s;
	} else {
		f32 s = sqrtf(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
		q[3] = (m[1][0] - m[0][1]) / s; q[0] = (m[0][2] + m[2][0]) / s; q[1] = (m[1][2] + m[2][1]) / s; q[2] = 0.25f * s;
	}
	q_norm(q);
}
static void q_to_m(const f32 q[4], f32 m[3][4])
{
	f32 x = q[0], y = q[1], z = q[2], w = q[3];
	m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
	m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
	m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}
/* turns q by the small rotation vector r (world): q += 0.5 [r,0] q */
static void q_turn(f32 q[4], const f32 r[3])
{
	f32 d[4] = {r[0], r[1], r[2], 0}, o[4];
	q_mul(d, q, o);
	for (int k = 0; k < 4; k++)
		q[k] += 0.5f * o[k];
	q_norm(q);
}
/* rotation vector (axis * angle) of q, the shorter way round */
static void q_log(const f32 q[4], f32 r[3])
{
	f32 s = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), w = q[3];
	f32 sg = w < 0 ? -1.0f : 1.0f;
	f32 a = 2 * atan2f(s, w * sg);
	f32 k = s > 1e-6f ? a / s * sg : 2 * sg;
	r[0] = q[0] * k; r[1] = q[1] * k; r[2] = q[2] * k;
}

/* the bone's transform from its solid's state */
static void solid_bone(const RagSolid *s, f32 B[3][4])
{
	q_to_m(s->q, B);
	f32 o[3];
	q_rot(s->q, s->cm, o);
	for (int k = 0; k < 3; k++)
		B[k][3] = s->x[k] - o[k];
}
/* a point fixed in the solid's bone frame, now (character frame) and relative to its centre */
static void solid_point(const RagSolid *s, const f32 local[3], f32 world_[3], f32 r[3])
{
	f32 d[3] = {local[0] - s->cm[0], local[1] - s->cm[1], local[2] - s->cm[2]};
	q_rot(s->q, d, r);
	for (int k = 0; k < 3; k++)
		world_[k] = s->x[k] + r[k];
}
/* the generalised inverse mass of a point at r for a push along unit n */
static f32 solid_w(const RagSolid *s, const f32 r[3], const f32 n[3])
{
	f32 c[3];
	v_cross(r, n, c);
	return s->im + s->ii * v_dot(c, c);
}
/* moves the solid by the positional impulse p at r (XPBD: x += p/m, q += 0.5 [I^-1 (r x p), 0] q) */
static void solid_push(RagSolid *s, const f32 r[3], const f32 p[3], f32 sign)
{
	f32 c[3];
	v_cross(r, p, c);
	for (int k = 0; k < 3; k++) {
		s->x[k] += sign * p[k] * s->im;
		c[k] *= sign * s->ii;
	}
	q_turn(s->q, c);
}

/* starts a ragdoll from the pose it died in; push: initial velocity (units/s, world) */
static int rag_start(Npc *n, const f32 push[3])
{
	Entity *e = ent_of(n);
	if (!n->skel || n->skel->numBones < 8 || flies(e->classname))
		return 0;
	if (!rags) {
		rags = malloc(MAX_RAGDOLLS * sizeof(Ragdoll));
		if (!rags)
			return 0;
		for (int i = 0; i < MAX_RAGDOLLS; i++)
			rags[i].ent = -1;
	}
	Ragdoll *r = NULL;
	for (int i = 0; i < MAX_RAGDOLLS && !r; i++)
		if (rags[i].ent < 0)
			r = &rags[i];
	if (!r) { /* all in use: the oldest settled one stays as it lies, for good */
		for (int i = 0; i < MAX_RAGDOLLS; i++)
			if ((!r || rags[i].frozen > r->frozen || (rags[i].frozen == r->frozen && rags[i].age > r->age)))
				r = &rags[i];
		/* (the body that loses its ragdoll is taken away, as Source fades out old ragdolls) */
		Ent_List(&(u32){0})[r->ent].flags |= EF_KILLED;
	}
	static f32 skin[NPC_MAX_BONES][3][4];
	Npc_Pose(n, skin); /* fills world[] */
	u32 nb = n->skel->numBones < NPC_MAX_BONES ? n->skel->numBones : NPC_MAX_BONES;
	const PackBone *bones = &P->npcBones[n->skel->firstBone];
	memset(r, 0, sizeof(*r));
	r->ent = n->ent;
	r->nb = (u16)nb;
	for (u32 i = 0; i < nb; i++) {
		memcpy(r->W0[i], world[i], sizeof(world[i]));
		memcpy(r->W[i], world[i], sizeof(world[i]));
		r->solidOf[i] = -1;
	}
	/* the solids: the model's .phy (CRagdollProp / RagdollCreate), else every bone of some length */
	u32 si = (u32)(n->skel - P->skels);
	const PackRagSolid *rs = NULL;
	const PackRagLimit *rl = NULL;
	u32 nrs = 0;
	if (P->skelRag && P->ragSolids && si < P->numSkelRag && P->skelRag[si][1] >= 2) {
		rs = &P->ragSolids[P->skelRag[si][0]];
		nrs = P->skelRag[si][1];
		if (P->ragLimits)
			rl = &P->ragLimits[P->skelRag[si][0]];
	}
	s16 bpar[NPC_MAX_BONES];
	for (u32 i = 0; i < nb; i++)
		bpar[i] = (s16)(bones[i].parent >= 0 && (u32)bones[i].parent < i ? bones[i].parent : -1);
	if (rs) {
		for (u32 k = 0; k < nrs && r->ns < RAG_MAX_SOLIDS; k++) {
			if (rs[k].bone >= nb)
				continue;
			RagSolid *s = &r->s[r->ns];
			s->bone = rs[k].bone;
			s->rad = rs[k].radius;
			s->par = (s16)(rs[k].parent >= 0 && (u32)rs[k].parent < nb ? rs[k].parent : -1); /* (a bone, for now) */
			f32 sw = rs[k].swing * (f32)M_PI / 180.0f;
			for (int a = 0; a < 3; a++) {
				s->lo[a] = rl ? rl[k].lo[a] * (f32)M_PI / 180.0f : -sw;
				s->hi[a] = rl ? rl[k].hi[a] * (f32)M_PI / 180.0f : sw;
			}
			r->solidOf[s->bone] = (s8)r->ns++;
		}
	} else {
		for (u32 i = 0; i < nb && r->ns < RAG_MAX_SOLIDS; i++) {
			f32 d[3] = {0, 0, 0};
			if (bpar[i] >= 0)
				for (int k = 0; k < 3; k++)
					d[k] = world[i][k][3] - world[bpar[i]][k][3];
			/* bones a few units from their parent (fingers, face, twist bones) just ride along */
			if (bpar[i] >= 0 && v_len(d) < 3.0f)
				continue;
			RagSolid *s = &r->s[r->ns];
			s->bone = (u16)i;
			s->rad = 2.0f;
			s->par = bpar[i];
			for (int a = 0; a < 3; a++) {
				s->lo[a] = -2.0f;
				s->hi[a] = 2.0f;
			}
			r->solidOf[i] = (s8)r->ns++;
		}
	}
	if (r->ns < 2) {
		r->ent = -1;
		return 0;
	}
	/* each solid hangs from the nearest solid above it in the skeleton */
	for (u32 k = 0; k < r->ns; k++) {
		RagSolid *s = &r->s[k];
		s16 a = s->par;
		while (a >= 0 && r->solidOf[a] < 0)
			a = bpar[a];
		s->par = a >= 0 ? r->solidOf[a] : -1;
	}
	/* the push in the character's frame (its rotation is a yaw) */
	f32 lp[3];
	for (int k = 0; k < 3; k++)
		lp[k] = e->R[0][k] * push[0] + e->R[1][k] * push[1] + e->R[2][k] * push[2];
	for (u32 k = 0; k < r->ns; k++) {
		RagSolid *s = &r->s[k];
		const f32(*B)[4] = r->W0[s->bone];
		/* its far end: the farthest solid hanging from it, else as far again as it is from its parent */
		f32 far[3] = {0, 0, 0}, best = -1;
		for (u32 j = 0; j < r->ns; j++)
			if (r->s[j].par == (s16)k) {
				f32 d[3];
				for (int c = 0; c < 3; c++)
					d[c] = r->W0[r->s[j].bone][c][3] - B[c][3];
				f32 l = v_len(d);
				if (l > best) {
					best = l;
					memcpy(far, d, 12);
				}
			}
		s->leaf = best < 0;
		if (best < 0 && s->par >= 0) {
			f32 d[3];
			for (int c = 0; c < 3; c++)
				d[c] = B[c][3] - r->W0[r->s[s->par].bone][c][3];
			f32 l = v_len(d);
			f32 ext = s->rad * 2.0f;
			if (l > 1e-3f)
				for (int c = 0; c < 3; c++)
					far[c] = d[c] / l * ext;
			best = ext;
		}
		/* (in its own frame) */
		for (int a = 0; a < 3; a++) {
			s->tip[a] = B[0][a] * far[0] + B[1][a] * far[1] + B[2][a] * far[2];
			s->cm[a] = s->tip[a] * 0.5f;
		}
		f32 len = best > 0 ? best : s->rad * 2;
		f32 m = len * s->rad * s->rad * 0.05f;
		if (m < 0.5f)
			m = 0.5f;
		s->im = 1.0f / m;
		s->ii = 1.0f / (m * (len * len / 12.0f + s->rad * s->rad * 0.4f));
		q_from_m(B, s->q);
		f32 o[3];
		q_rot(s->q, s->cm, o);
		for (int c = 0; c < 3; c++) {
			s->x[c] = B[c][3] + o[c];
			s->v[c] = lp[c];
		}
		s->w[0] = s->w[1] = s->w[2] = 0;
		/* its joint in the parent's frame, as it died (Source: the bone-to-bone transform) */
		if (s->par >= 0) {
			const f32(*A)[4] = r->W0[r->s[s->par].bone];
			f32 d[3];
			for (int c = 0; c < 3; c++)
				d[c] = B[c][3] - A[c][3];
			for (int a = 0; a < 3; a++)
				s->anchor[a] = A[0][a] * d[0] + A[1][a] * d[1] + A[2][a] * d[2];
			/* the limits are about the bind pose: the child's rotation in its parent's then
			 * (poseToBone is the inverse of the bind transform: rest = ptb_parent * ptb_child^T) */
			const f32(*pp)[4] = bones[r->s[s->par].bone].poseToBone;
			const f32(*pc)[4] = bones[s->bone].poseToBone;
			f32 M[3][4];
			for (int a = 0; a < 3; a++)
				for (int c = 0; c < 3; c++)
					M[a][c] = pp[a][0] * pc[c][0] + pp[a][1] * pc[c][1] + pp[a][2] * pc[c][2];
			q_from_m(M, s->rest);
		}
	}
	return 1;
}

/* the joint: the child's bone origin stays where it is fixed in its parent's frame (ball and socket) */
static void rag_joint(Ragdoll *r, RagSolid *s)
{
	RagSolid *pa = &r->s[s->par];
	f32 a1[3], r1[3], a2[3], r2[3], zero[3] = {0, 0, 0};
	solid_point(pa, s->anchor, a1, r1);
	solid_point(s, zero, a2, r2);
	f32 d[3];
	v_sub(a2, a1, d);
	f32 c = v_len(d);
	if (c < 1e-4f)
		return;
	for (int k = 0; k < 3; k++)
		d[k] /= c;
	f32 w = solid_w(pa, r1, d) + solid_w(s, r2, d);
	if (w < 1e-8f)
		return;
	f32 p[3];
	for (int k = 0; k < 3; k++)
		p[k] = d[k] * c / w;
	solid_push(pa, r1, p, 1.0f);
	solid_push(s, r2, p, -1.0f);
}

/* the joint's limits: the child's rotation away from its rest in the parent, about each of its own axes,
 * kept within the ragdollconstraint's ranges */
static void rag_limits(Ragdoll *r, RagSolid *s)
{
	RagSolid *pa = &r->s[s->par];
	f32 ref[4], inv[4], rel[4], rv[3];
	q_mul(pa->q, s->rest, ref);
	q_conj(ref, inv);
	q_mul(inv, s->q, rel); /* child = ref * rel; rel is in the child's own frame */
	q_log(rel, rv);
	f32 cl[3];
	int over = 0;
	for (int a = 0; a < 3; a++) {
		cl[a] = rv[a] < s->lo[a] ? s->lo[a] : rv[a] > s->hi[a] ? s->hi[a] : rv[a];
		if (cl[a] != rv[a])
			over = 1;
	}
	if (!over)
		return;
	/* the turn back: from rel to the clamped one, in the child's frame, then into the character's */
	f32 ang = v_len(cl), want[4];
	if (ang > 1e-6f) {
		f32 sh = sinf(ang * 0.5f) / ang;
		want[0] = cl[0] * sh; want[1] = cl[1] * sh; want[2] = cl[2] * sh; want[3] = cosf(ang * 0.5f);
	} else {
		want[0] = want[1] = want[2] = 0; want[3] = 1;
	}
	f32 cur[4], fix[4], fv[3], wl[3];
	q_conj(rel, cur);
	q_mul(want, cur, fix); /* (want = fix * rel, in the frame of ref) */
	q_log(fix, fv);
	q_rot(ref, fv, wl);
	f32 sum = pa->ii + s->ii;
	if (sum < 1e-8f)
		return;
	f32 a1[3], a2[3];
	for (int k = 0; k < 3; k++) {
		a2[k] = wl[k] * s->ii / sum;
		a1[k] = -wl[k] * pa->ii / sum;
	}
	q_turn(s->q, a2);
	q_turn(pa->q, a1);
}

/* the world: the solid's centre and far end, as thick as it is, stop at what they meet */
static f32 rag_collide(RagSolid *s, const Entity *e)
{
	f32 mv = 0;
	static const f32 origin_[3] = {0, 0, 0};
	const f32 *pts[3] = {s->cm, origin_, s->tip};
	/* its centre and its joint; a solid's far end is its child's joint, which the child keeps off the world,
	 * so only the ends of the chains (hands, feet, head) need theirs */
	int np = s->leaf ? 3 : 2;
	for (int k = 0; k < np; k++) {
		f32 now[3], rn[3], was[3], d[3];
		solid_point(s, pts[k], now, rn);
		/* (where it was at the substep's start) */
		v_sub(pts[k], s->cm, d);
		q_rot(s->tq, d, was);
		for (int c = 0; c < 3; c++)
			was[c] += s->tx[c];
		f32 a[3], b[3];
		for (int c = 0; c < 3; c++) {
			a[c] = e->origin[c] + e->R[c][0] * was[0] + e->R[c][1] * was[1] + e->R[c][2] * was[2];
			b[c] = e->origin[c] + e->R[c][0] * now[0] + e->R[c][1] * now[1] + e->R[c][2] * now[2];
		}
		f32 dm[3];
		v_sub(b, a, dm);
		if (v_dot(dm, dm) < 1e-4f)
			continue; /* (not moving: nothing new to meet) */
		f32 rr = s->rad * (k ? 0.6f : 0.8f);
		if (k == 1 && rr > 3.0f)
			rr = 3.0f; /* (a joint is narrower than the solid's middle) */
		f32 mn[3] = {-rr, -rr, -rr}, mx[3] = {rr, rr, rr};
		Trace tr;
		CM_BoxTrace(&tr, a, b, mn, mx, MASK_NPCSOLID & ~CONTENTS_MONSTERCLIP);
		if (tr.startsolid && rr > 1.0f) {
			/* (already touching something: its core still collides) */
			f32 m1[3] = {-1, -1, -1}, x1[3] = {1, 1, 1};
			CM_BoxTrace(&tr, a, b, m1, x1, MASK_NPCSOLID & ~CONTENTS_MONSTERCLIP);
		}
		int deep = 0;
		if (tr.startsolid) {
			deep = 1;
			/* (it started inside: out along the line to it from another point of the same solid, its
			 * centre or its joint, if that one is clear) */
			f32 o[3], ro[3];
			solid_point(s, k ? s->cm : s->tip, o, ro);
			for (int c = 0; c < 3; c++)
				a[c] = e->origin[c] + e->R[c][0] * o[0] + e->R[c][1] * o[1] + e->R[c][2] * o[2];
			f32 m1[3] = {-1, -1, -1}, x1[3] = {1, 1, 1};
			CM_BoxTrace(&tr, a, b, m1, x1, MASK_NPCSOLID & ~CONTENTS_MONSTERCLIP);
			if (tr.startsolid || tr.fraction >= 1)
				continue;
			memcpy(was, now, 12); /* (no friction from this) */
		}
		if (tr.fraction >= 1)
			continue;
		/* the stop and the normal, in the character's frame */
		f32 hw[3], nl[3], hit[3];
		for (int c = 0; c < 3; c++)
			hw[c] = tr.endpos[c] + tr.normal[c] * 0.1f - e->origin[c];
		for (int c = 0; c < 3; c++) {
			hit[c] = e->R[0][c] * hw[0] + e->R[1][c] * hw[1] + e->R[2][c] * hw[2];
			nl[c] = e->R[0][c] * tr.normal[0] + e->R[1][c] * tr.normal[1] + e->R[2][c] * tr.normal[2];
		}
		/* out along the normal to the surface, and friction: most of its slide along it undone */
		f32 pen = v_dot(hit, nl) - v_dot(now, nl);
		f32 corr[3];
		f32 slide[3];
		v_sub(now, was, slide);
		f32 sn = v_dot(slide, nl);
		for (int c = 0; c < 3; c++)
			corr[c] = nl[c] * (pen > 0 ? pen : 0) - (slide[c] - nl[c] * sn) * 0.6f;
		f32 cm = v_len(corr);
		if (cm < 1e-5f)
			continue;
		if (cm > 8.0f) { /* (out a little at a time: a deep start isn't a launch) */
			for (int c = 0; c < 3; c++)
				corr[c] *= 8.0f / cm;
			cm = 8.0f;
		}
		f32 dn[3] = {corr[0] / cm, corr[1] / cm, corr[2] / cm};
		f32 w = solid_w(s, rn, dn);
		f32 p[3];
		for (int c = 0; c < 3; c++)
			p[c] = corr[c] / w;
		f32 bx[3], bq[4];
		memcpy(bx, s->x, 12);
		memcpy(bq, s->q, 16);
		solid_push(s, rn, p, 1.0f);
		if (deep) {
			/* (getting out of something isn't a throw: the move out carries no speed with it) */
			f32 iq[4], dq[4], t[4];
			for (int c = 0; c < 3; c++)
				s->px[c] += s->x[c] - bx[c];
			q_conj(bq, iq);
			q_mul(s->q, iq, dq);
			q_mul(dq, s->pq, t);
			memcpy(s->pq, t, 16);
		}
		if (cm > mv)
			mv = cm;
	}
	return mv;
}

static void rag_step(Ragdoll *r, f32 dt)
{
	Entity *e = &((Entity *)Ent_List(&(u32){0}))[r->ent];
	r->age += dt;
	if (r->frozen)
		return;
	const f32 h = dt / RAG_SUBSTEPS;
	f32 moved = 0;
	for (u32 k = 0; k < r->ns; k++) {
		memcpy(r->s[k].tx, r->s[k].x, 12);
		memcpy(r->s[k].tq, r->s[k].q, 16);
	}
	for (int sub = 0; sub < RAG_SUBSTEPS; sub++) {
		/* integrate: sv_gravity, a little air drag (Source's ragdolls are damped too) */
		for (u32 k = 0; k < r->ns; k++) {
			RagSolid *s = &r->s[k];
			memcpy(s->px, s->x, 12);
			memcpy(s->pq, s->q, 16);
			s->v[2] -= 600.0f * h;
			for (int c = 0; c < 3; c++) {
				s->v[c] *= 0.999f;
				s->w[c] *= 0.995f;
				s->x[c] += s->v[c] * h;
			}
			f32 rv[3] = {s->w[0] * h, s->w[1] * h, s->w[2] * h};
			q_turn(s->q, rv);
		}
		for (int it = 0; it < RAG_ITERS; it++) {
			for (u32 k = 0; k < r->ns; k++)
				if (r->s[k].par >= 0)
					rag_joint(r, &r->s[k]);
			for (u32 k = 0; k < r->ns; k++)
				if (r->s[k].par >= 0)
					rag_limits(r, &r->s[k]);
		}
		/* the world last, once a tick (each point swept from where it was then): nothing ends a tick inside it.
		 * (Every substep would cost the GameCube three times the traces for little difference.) */
		if (sub == RAG_SUBSTEPS - 1)
			for (u32 k = 0; k < r->ns; k++)
				rag_collide(&r->s[k], e);
		/* velocities from what the substep did */
		for (u32 k = 0; k < r->ns; k++) {
			RagSolid *s = &r->s[k];
			for (int c = 0; c < 3; c++)
				s->v[c] = (s->x[c] - s->px[c]) / h;
			f32 iq[4], dq[4];
			q_conj(s->pq, iq);
			q_mul(s->q, iq, dq);
			f32 rv[3];
			q_log(dq, rv);
			for (int c = 0; c < 3; c++)
				s->w[c] = rv[c] / h;
			/* (Source's physics caps speeds too: nothing a pile-up of corrections can fling) */
			f32 sv = v_len(s->v), sw = v_len(s->w);
			if (sv > 1500.0f)
				for (int c = 0; c < 3; c++)
					s->v[c] *= 1500.0f / sv;
			if (sw > 25.0f)
				for (int c = 0; c < 3; c++)
					s->w[c] *= 25.0f / sw;
			f32 d[3];
			v_sub(s->x, s->px, d);
			f32 m = v_len(d) + v_len(rv) * 6.0f;
			if (m > moved)
				moved = m;
		}
	}
	/* the bones: each solid's from its body; the rest keep their place on their parent (as they died) */
	const PackBone *bones = &P->npcBones[Npc_OfEntity(r->ent)->skel->firstBone];
	for (u32 i = 0; i < r->nb; i++) {
		if (r->solidOf[i] >= 0) {
			solid_bone(&r->s[r->solidOf[i]], r->W[i]);
			continue;
		}
		s32 bp = bones[i].parent;
		s16 q = (s16)(bp >= 0 && (u32)bp < i ? bp : -1);
		if (q < 0) {
			/* (a root that isn't a solid: it goes where the first solid goes) */
			const RagSolid *s0 = &r->s[0];
			f32 B0[3][4], rel[3][4];
			solid_bone(s0, B0);
			const f32(*A)[4] = r->W0[s0->bone];
			for (int a = 0; a < 3; a++) {
				for (int c = 0; c < 3; c++)
					rel[a][c] = A[0][a] * r->W0[i][0][c] + A[1][a] * r->W0[i][1][c] + A[2][a] * r->W0[i][2][c];
				f32 t[3] = {r->W0[i][0][3] - A[0][3], r->W0[i][1][3] - A[1][3], r->W0[i][2][3] - A[2][3]};
				rel[a][3] = A[0][a] * t[0] + A[1][a] * t[1] + A[2][a] * t[2];
			}
			for (int a = 0; a < 3; a++)
				for (int c = 0; c < 4; c++)
					r->W[i][a][c] = B0[a][0] * rel[0][c] + B0[a][1] * rel[1][c] + B0[a][2] * rel[2][c] + (c == 3 ? B0[a][3] : 0);
			continue;
		}
		/* rel = inv(W0[q]) * W0[i]; W[i] = W[q] * rel */
		f32 rel[3][4];
		for (int a = 0; a < 3; a++) {
			for (int c = 0; c < 3; c++)
				rel[a][c] = r->W0[q][0][a] * r->W0[i][0][c] + r->W0[q][1][a] * r->W0[i][1][c] + r->W0[q][2][a] * r->W0[i][2][c];
			f32 t[3] = {r->W0[i][0][3] - r->W0[q][0][3], r->W0[i][1][3] - r->W0[q][1][3], r->W0[i][2][3] - r->W0[q][2][3]};
			rel[a][3] = r->W0[q][0][a] * t[0] + r->W0[q][1][a] * t[1] + r->W0[q][2][a] * t[2];
		}
		for (int a = 0; a < 3; a++)
			for (int c = 0; c < 4; c++)
				r->W[i][a][c] = r->W[q][a][0] * rel[0][c] + r->W[q][a][1] * rel[1][c] + r->W[q][a][2] * rel[2][c] +
				                (c == 3 ? r->W[q][a][3] : 0);
	}
	/* at rest for a while: it stops (Source puts sleeping ragdolls to sleep too) */
	if (moved < 0.02f) {
		r->still += dt;
		if (r->still > 1.0f)
			r->frozen = 1;
	} else {
		r->still = 0;
	}
}

void Npc_Pose(const Npc *n, f32 (*out)[3][4])
{
	Ragdoll *rg = n->dead ? rag_of(n->ent) : NULL;
	if (rg) {
		const PackBone *bones = &P->npcBones[n->skel->firstBone];
		for (u32 i = 0; i < rg->nb; i++) {
			memcpy(world[i], rg->W[i], sizeof(world[i]));
			const f32(*pb)[4] = bones[i].poseToBone;
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 4; c++)
					out[i][r][c] = world[i][r][0] * pb[0][c] + world[i][r][1] * pb[1][c] + world[i][r][2] * pb[2][c] +
					               (c == 3 ? world[i][r][3] : 0);
		}
		return;
	}
	AnimLayer L[NPC_MAX_GESTURES];
	AnimExtra ex = {L, 0, n->lookYaw, n->lookPitch, n->lookW, n->aimOn, n->aimYaw, n->aimPitch};
	for (int g = 0; g < NPC_MAX_GESTURES; g++)
		if (n->gest[g].seq >= 0 && n->gest[g].weight > 0)
			L[ex.numLayers++] = n->gest[g];
	Anim_PoseEx(P, n->skel, n->seq, n->t, n->hold, n->prevSeq, n->prevT, n->blend, &ex, out);
}

s16 Npc_FindSeq(const Npc *n, const char *name) { return find_seq(n, name); }

/* (tests) the bones' own transforms (model space), not the skinning matrices */
void Npc_BonePose(const Npc *n, f32 (*out)[3][4])
{
	static f32 tmp[NPC_MAX_BONES][3][4];
	Npc_Pose(n, tmp);
	u32 nb = n->skel->numBones < NPC_MAX_BONES ? n->skel->numBones : NPC_MAX_BONES;
	memcpy(out, world, nb * sizeof(world[0]));
}

f32 Npc_Cycle(const Npc *n, u32 *globalSeq)
{
	const PackSeq *sp = seq_ptr(n, n->seq);
	if (globalSeq)
		*globalSeq = sp ? n->skel->firstSeq + (u32)n->seq : 0xFFFFFFFFu;
	return seq_cycle(sp, n->t, n->hold);
}

int Npc_SetGesture(Npc *n, int slot, s16 seq, f32 cycle, f32 weight, f32 time)
{
	if (slot < 0)
		for (int g = 0; g < NPC_MAX_GESTURES && slot < 0; g++)
			if (n->gest[g].seq < 0)
				slot = g;
	if (slot < 0 || slot >= NPC_MAX_GESTURES)
		return -1;
	n->gest[slot].seq = seq;
	n->gest[slot].cycle = cycle;
	n->gest[slot].weight = weight;
	n->gest[slot].time = time;
	return slot;
}

void Npc_ClearGestures(Npc *n)
{
	for (int g = 0; g < NPC_MAX_GESTURES; g++)
		n->gest[g].seq = -1;
}

/* Head turning. A scene's LOOKAT target wins; otherwise idle characters glance at the player when he is close
 * and in front of them, like Source's AI does with its "look at the player" behaviour. Body-relative yaw is
 * clamped to the range of HL2's head_yaw/body_yaw pose parameters; the turn eases in and out. */
#define LOOK_YAW_MAX 75.0f
#define LOOK_PITCH_MAX 40.0f
static void update_look(Npc *n, f32 dt)
{
	Entity *e = ent_of(n);
	s32 target = n->lookEnt;
	if (target < 0 && !n->moving && !n->dead && !n->locked && player) {
		f32 dx = player->origin[0] - e->origin[0], dy = player->origin[1] - e->origin[1];
		f32 d2 = dx * dx + dy * dy, dz = player->origin[2] - e->origin[2];
		/* (only a player it can see on its own level: not one on the floor above or below, which had
		 * NPCs gazing at the ceiling) */
		if (d2 < 160.0f * 160.0f && fabsf(dz) < 64.0f) {
			f32 rel = atan2f(dy, dx) * 180.0f / (f32)M_PI - n->yaw;
			rel = fmodf(rel + 540.0f, 360.0f) - 180.0f;
			const PackNpcModel *m = model_of(n);
			f32 eye[3] = {e->origin[0], e->origin[1], e->origin[2] + (m->eyeHeight > 0 ? m->eyeHeight : 64.0f)};
			f32 dist;
			if (fabsf(rel) < 100.0f) {
				if (npcClock >= n->lookSenseNext) { /* (a few times a second is plenty for where it looks) */
					n->lookSenseNext = npcClock + 0.25f;
					n->lookSenseSeen = (u8)Ent_PlayerVisible(eye, &dist);
				}
				if (n->lookSenseSeen)
					target = 0;
			}
		}
	}
	f32 wantYaw = 0, wantPitch = 0, wantW = 0;
	if (target >= 0) {
		f32 tp[3];
		Npc_EyePos(target, tp);
		const PackNpcModel *m = model_of(n);
		f32 eye = m->eyeHeight > 0 ? m->eyeHeight : 64.0f;
		f32 dx = tp[0] - e->origin[0], dy = tp[1] - e->origin[1], dz = tp[2] - (e->origin[2] + eye);
		f32 rel = atan2f(dy, dx) * 180.0f / (f32)M_PI - n->yaw;
		rel = fmodf(rel + 540.0f, 360.0f) - 180.0f;
		if (fabsf(rel) <= 120.0f || n->sceneLook) {
			wantYaw = rel > LOOK_YAW_MAX ? LOOK_YAW_MAX : (rel < -LOOK_YAW_MAX ? -LOOK_YAW_MAX : rel);
			wantPitch = atan2f(dz, sqrtf(dx * dx + dy * dy)) * 180.0f / (f32)M_PI;
			if (wantPitch > LOOK_PITCH_MAX)
				wantPitch = LOOK_PITCH_MAX;
			if (wantPitch < -LOOK_PITCH_MAX)
				wantPitch = -LOOK_PITCH_MAX;
			wantW = 1;
		}
	}
	/* heads turn at up to ~200 degrees a second, easing the last part */
	f32 k = 1.0f - expf(-dt * 8.0f);
	f32 maxStep = 200.0f * dt;
	f32 dyaw = (wantYaw - n->lookYaw) * k, dpitch = (wantPitch - n->lookPitch) * k;
	if (dyaw > maxStep)
		dyaw = maxStep;
	if (dyaw < -maxStep)
		dyaw = -maxStep;
	if (dpitch > maxStep)
		dpitch = maxStep;
	if (dpitch < -maxStep)
		dpitch = -maxStep;
	if (wantW > 0 || n->lookW <= 0) {
		n->lookYaw += dyaw;
		n->lookPitch += dpitch;
	}
	n->lookW += (wantW - n->lookW) * (1.0f - expf(-dt * 5.0f));
	if (n->lookW < 0.001f)
		n->lookW = 0;
}

int Npc_Attachment(const Npc *n, const char *name, f32 pos[3], f32 R[3][3])
{
	if (!P->npcFaces || !P->attachments)
		return 0;
	const PackNpcFace *f = &P->npcFaces[n->model];
	for (u32 i = 0; i < f->numAtt; i++) {
		const PackAttachment *a = &P->attachments[f->firstAtt + i];
		if (strcasecmp(pstr(a->name), name))
			continue;
		static f32 skin[NPC_MAX_BONES][3][4];
		Npc_Pose(n, skin); /* fills world[] */
		u32 b = (a->bone >= 0 && (u32)a->bone < n->skel->numBones) ? (u32)a->bone : 0;
		f32 m[3][4];
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++)
				m[r][c] = world[b][r][0] * a->local[0][c] + world[b][r][1] * a->local[1][c] +
				          world[b][r][2] * a->local[2][c] + (c == 3 ? world[b][r][3] : 0);
		const Entity *e = ent_of(n);
		for (int r = 0; r < 3; r++) {
			pos[r] = e->origin[r];
			for (int c = 0; c < 3; c++) {
				pos[r] += e->R[r][c] * m[c][3];
				R[r][c] = e->R[r][0] * m[0][c] + e->R[r][1] * m[1][c] + e->R[r][2] * m[2][c];
			}
		}
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ movement */

/* Source's AutoMovement: a scripted / scene / act-busy sequence with motion in it (sitting down onto a
 * bench, stepping through a door) moves the NPC along with it, so its next sequence starts where this one
 * left the body */
static void auto_move(Npc *n, f32 dt)
{
	if (!P->npcSeqMove || n->seq < 0)
		return;
	u32 gi = n->skel->firstSeq + (u32)n->seq;
	if (gi >= P->numNpcSeqMove)
		return;
	const f32 *mv = P->npcSeqMove[gi];
	if (mv[0] == 0 && mv[1] == 0 && mv[2] == 0 && mv[3] == 0)
		return;
	f32 dur = n->seqDuration;
	if (dur <= 0)
		return;
	f32 f;
	if (n->hold) {
		f32 a = n->t < dur ? n->t : dur, b = n->t + dt < dur ? n->t + dt : dur;
		f = (b - a) / dur;
	} else
		f = dt / dur;
	if (f <= 0)
		return;
	Entity *e = ent_of(n);
	f32 y = n->yaw * (f32)M_PI / 180.0f, c = cosf(y), sn = sinf(y);
	f32 dx = mv[0] * f, dy = mv[1] * f;
	e->origin[0] += c * dx - sn * dy;
	e->origin[1] += sn * dx + c * dy;
	e->origin[2] += mv[2] * f;
	n->yaw += mv[3] * f;
	n->idealYaw = n->yaw;
}

static void sync_entity(Npc *n)
{
	Entity *e = ent_of(n);
	e->angles[0] = 0;
	e->angles[1] = n->yaw;
	e->angles[2] = 0;
	Ent_AngleMatrix(e->angles, e->R);
}

static s32 passProp = -1; /* a physics prop the moving NPC is shoving: its moves ignore it */

/* func_brush "excludednpc": brushes (NPC clips) that let a named NPC or class through, like Barney's
 * hallway on Trainstation 1 */
#define MAX_EXCL 16
static s32 exclBrush[MAX_EXCL];
static int numExcl;

/* logic_navigation "navprop" "Ignore": things NPCs walk through as if they weren't there (Trainstation
 * 1's turnstile: their physics shadow turns it as they go) */
#define MAX_NAVIGNORE 4
static s32 navIgnore[MAX_NAVIGNORE];
static int numNavIgnore;

/* logic_collision_pair: two things that pass through each other while it's disabled (an apartment
 * citizen sits down on his couch, a prop physics object) */
#define MAX_COLPAIRS 32
static s32 colPair[MAX_COLPAIRS];
static int numColPairs;

static void find_exclusions(void)
{
	u32 ne;
	Entity *E = Ent_List(&ne);
	numExcl = 0;
	numNavIgnore = 0;
	numColPairs = 0;
	for (u32 i = 1; i < ne; i++) {
		if (!strcmp(E[i].classname, "logic_collision_pair") && numColPairs < MAX_COLPAIRS) {
			const char *sd = Ent_KV((s32)i, "startdisabled");
			E[i].value = (!sd || atoi(sd) != 0) ? 1.0f : 0.0f; /* (1: collisions between them disabled) */
			colPair[numColPairs++] = (s32)i;
		}
		const char *x = Ent_KV((s32)i, "excludednpc");
		if (numExcl < MAX_EXCL && x && *x && !strcmp(E[i].classname, "func_brush"))
			exclBrush[numExcl++] = (s32)i;
		if (!strcmp(E[i].classname, "logic_navigation")) {
			const char *np = Ent_KV((s32)i, "navprop"), *tg = Ent_KV((s32)i, "target");
			if (np && !strcasecmp(np, "ignore") && tg && *tg)
				for (u32 j = 1; j < ne && numNavIgnore < MAX_NAVIGNORE; j++)
					if (E[j].name && !strcasecmp(E[j].name, tg))
						navIgnore[numNavIgnore++] = (s32)j;
		}
	}
}

static int excluded_for(const Npc *n, s32 *out)
{
	int k = 0;
	for (int i = 0; i < numNavIgnore; i++)
		out[k++] = navIgnore[i];
	u32 ne;
	Entity *E = Ent_List(&ne);
	const Entity *me = &E[n->ent];
	for (int i = 0; i < numColPairs && k < 8; i++) {
		const Entity *p = &E[colPair[i]];
		if (p->value == 0 || (p->flags & EF_KILLED) || !me->name)
			continue;
		const char *a = Ent_KV(colPair[i], "attach1"), *b = Ent_KV(colPair[i], "attach2");
		const char *other = a && !strcasecmp(a, me->name) ? b : b && !strcasecmp(b, me->name) ? a : NULL;
		if (other && *other) {
			s32 o = Ent_FindActor(other);
			if (o > 0)
				out[k++] = o;
		}
	}
	if (!numExcl)
		return k;
	for (int i = 0; i < numExcl && k < 8; i++) {
		const char *x = Ent_KV(exclBrush[i], "excludednpc");
		int invert = Ent_KV(exclBrush[i], "invert_exclusion") && atoi(Ent_KV(exclBrush[i], "invert_exclusion"));
		int match = x && (!strcasecmp(x, me->classname) || (me->name && !strcasecmp(x, me->name)));
		if (match != invert)
			out[k++] = exclBrush[i];
	}
	return k;
}

static void trace(Trace *tr, const Npc *n, const f32 a[3], const f32 b[3])
{
	f32 mx[3] = {13, 13, hull_height(n)};
	s32 ex[8];
	int nex = excluded_for(n, ex);
	CM_SetSkipBoxes(1);
	if (passProp > 0)
		CM_SetIgnoreEnt(passProp);
	if (nex)
		CM_SetIgnoreExtra(ex, nex);
	CM_BoxTrace(tr, a, b, hullMins, mx, MASK_NPCSOLID);
	if (nex)
		CM_SetIgnoreExtra(NULL, 0);
	if (passProp > 0)
		CM_SetIgnoreEnt(-1);
	CM_SetSkipBoxes(0);
}

/* Source's NPCs shove the physics props in their way (the shadow controller pushes them): a light one
 * gets pushed along and the NPC keeps walking; a heavy one blocks like a wall (the path goes round) */
#define NPC_PUSH_MASS 250.0f
static int shove(const Npc *n, const Trace *tr, f32 dx, f32 dy)
{
	/* a turnstile: turn it and walk on through (it swings out of the way) */
	if (tr->ent > 0 && Ent_HingePush(tr->ent, ent_of(n)->origin, dx / TICK_INTERVAL, dy / TICK_INTERVAL))
		return 1;
	if (tr->ent <= 0 || !Phys_IsBody(tr->ent) || Phys_Mass(tr->ent) > NPC_PUSH_MASS)
		return 0;
	f32 len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f)
		return 0;
	f32 m = Phys_Mass(tr->ent);
	f32 v = 140.0f; /* in/s: a firm push */
	f32 imp[3] = {dx / len * m * v * 0.1f, dy / len * m * v * 0.1f, m * 20.0f * 0.1f};
	f32 at[3] = {tr->endpos[0], tr->endpos[1], tr->endpos[2] + hull_height(n) * 0.4f};
	Phys_Impulse(tr->ent, at, imp);
	return 1;
}

/* Source's local navigator: when the way ahead is blocked (a table, a crate, another NPC), try heading
 * off to one side and then the other, keeping to the side that worked for a moment so it walks round the
 * obstacle instead of into it */
static f32 walk(Npc *n, f32 dx, f32 dy);
static f32 walk_around(Npc *n, f32 fx, f32 fy, f32 step)
{
	if (Ent_Now() < n->avoidUntil && n->avoidSide) {
		/* still going round: the side that worked, opening back to straight as the way clears */
		static const f32 angs[3] = {35, 60, 85};
		for (int i = 0; i < 3; i++) {
			f32 a = angs[i] * n->avoidSide * (f32)M_PI / 180.0f, c = cosf(a), sn = sinf(a);
			f32 ex = fx * c - fy * sn, ey = fx * sn + fy * c;
			f32 *o = ent_of(n)->origin, save[3];
			memcpy(save, o, 12);
			f32 m = walk(n, ex * step, ey * step);
			if (m >= step * 0.5f) {
				/* try straight again next time if it's clear */
				f32 probe[3] = {o[0] + fx * 24, o[1] + fy * 24, o[2] + STEP}, from[3] = {o[0], o[1], o[2] + STEP};
				Trace t;
				trace(&t, n, from, probe);
				if (!t.startsolid && t.fraction >= 1.0f)
					n->avoidUntil = 0;
				return m;
			}
			memcpy(o, save, 12);
		}
		n->avoidUntil = 0;
	}
	f32 m = walk(n, fx * step, fy * step);
	if (m >= step * 0.5f || step < 0.01f)
		return m;
	/* blocked: the side with room, nearer the straight line first */
	static const f32 tries[6] = {35, -35, 65, -65, 95, -95};
	f32 *o = ent_of(n)->origin, save[3];
	memcpy(save, o, 12);
	for (int i = 0; i < 6; i++) {
		f32 a = tries[i] * (f32)M_PI / 180.0f, c = cosf(a), sn = sinf(a);
		f32 ex = fx * c - fy * sn, ey = fx * sn + fy * c;
		f32 mm = walk(n, ex * step, ey * step);
		if (mm >= step * 0.5f) {
			n->avoidSide = tries[i] > 0 ? 1 : -1;
			n->avoidUntil = Ent_Now() + 1.0f;
			return mm;
		}
		memcpy(o, save, 12);
	}
	return m;
}

/* a place near p where the NPC's hull is free (not inside the table it was stuck on), at p's height */
static void free_spot(Npc *n, f32 p[3])
{
	Trace t;
	f32 a[3] = {p[0], p[1], p[2] + 1};
	trace(&t, n, a, a);
	if (!t.startsolid)
		return;
	for (f32 r = 16; r <= 96; r += 16)
		for (int k = 0; k < 8; k++) {
			f32 ang = k * (f32)M_PI / 4;
			f32 q[3] = {p[0] + cosf(ang) * r, p[1] + sinf(ang) * r, p[2] + 1};
			trace(&t, n, q, q);
			if (!t.startsolid) {
				memcpy(p, q, 12);
				return;
			}
		}
}

void Npc_Teleport(Npc *n, const f32 pos[3], f32 yaw)
{
	Entity *e = ent_of(n);
	memcpy(e->origin, pos, 12);
	n->yaw = n->idealYaw = yaw;
	n->fallSpeed = 0;
	n->moving = 0;
	sync_entity(n);
}

/* ---- pathfinding on the map's AI node graph (like Source's CAI_Pathfinder, without the local
 * navigation): A* over the ground nodes and the links a human-sized hull can walk */
static f32 *navG;
static s16 *navFrom;
static u8 *navState; /* 0 unseen, 1 open, 2 closed */

static int clear_line(const Npc *n, const f32 a[3], const f32 b[3])
{
	f32 a2[3] = {a[0], a[1], a[2] + STEP}, b2[3] = {b[0], b[1], b[2] + STEP};
	Trace tr;
	trace(&tr, n, a2, b2);
	return !tr.startsolid && tr.fraction >= 1.0f;
}

/* the nearest ground node that can be walked to from p in a straight line */
static s32 nearest_node(const Npc *n, const f32 p[3], int needClear)
{
	s32 best[8];
	f32 bd[8];
	int nb = 0;
	for (u32 i = 0; i < P->numNavNodes; i++) {
		const PackNavNode *nd = &P->navNodes[i];
		if (nd->type != NAVN_GROUND || !nd->numLinks)
			continue;
		f32 dx = nd->pos[0] - p[0], dy = nd->pos[1] - p[1], dz = (nd->pos[2] - p[2]) * 3.0f;
		f32 d = dx * dx + dy * dy + dz * dz;
		int k;
		if (nb < 8) {
			k = nb++;
		} else {
			if (d >= bd[7])
				continue;
			k = 7;
		}
		while (k > 0 && bd[k - 1] > d) {
			bd[k] = bd[k - 1];
			best[k] = best[k - 1];
			k--;
		}
		bd[k] = d;
		best[k] = (s32)i;
	}
	for (int k = 0; k < nb; k++)
		if (!needClear || clear_line(n, p, P->navNodes[best[k]].pos))
			return best[k];
	/* none in plain view (a step or a doorframe edge in the way): the closest one if it's really close;
	 * otherwise (riding a train away from where its nodes are) walk straight */
	if (nb && bd[0] < 160.0f * 160.0f)
		return best[0];
	return -1;
}

static void plan_path(Npc *n)
{
	n->pathLen = n->pathPos = 0;
	if (!P->numNavNodes || !navG || flies(ent_of(n)->classname))
		return;
	const f32 *o = ent_of(n)->origin;
	if (clear_line(n, o, n->goal))
		return; /* straight there */
	s32 s = nearest_node(n, o, 1), g = nearest_node(n, n->goal, 1);
#ifdef NAV_DEBUG
	printf("plan ent %d from (%.0f %.0f %.0f) node %d to (%.0f %.0f %.0f) node %d\n", n->ent, o[0], o[1], o[2], s,
	       n->goal[0], n->goal[1], n->goal[2], g);
#endif
	if (s < 0 || g < 0 || s == g) {
		if (s >= 0 && s == g) {
			n->path[0] = (u16)s;
			n->pathLen = 1;
		}
		return;
	}
	u32 N = P->numNavNodes;
	memset(navState, 0, N);
	const f32 *gp = P->navNodes[g].pos;
	navG[s] = 0;
	navFrom[s] = -1;
	navState[s] = 1;
	for (;;) {
		/* the open node with the lowest cost + straight-line estimate */
		s32 cur = -1;
		f32 cf = 1e30f;
		for (u32 i = 0; i < N; i++)
			if (navState[i] == 1) {
				const f32 *q = P->navNodes[i].pos;
				f32 h = sqrtf((q[0] - gp[0]) * (q[0] - gp[0]) + (q[1] - gp[1]) * (q[1] - gp[1]) +
				              (q[2] - gp[2]) * (q[2] - gp[2]));
				if (navG[i] + h < cf) {
					cf = navG[i] + h;
					cur = (s32)i;
				}
			}
		if (cur < 0)
			return; /* no route: walk straight and let the stuck rule handle it */
		if (cur == g)
			break;
		navState[cur] = 2;
		const PackNavNode *cn = &P->navNodes[cur];
		for (u32 l = 0; l < cn->numLinks; l++) {
			const PackNavLink *L = &P->navLinks[cn->firstLink + l];
			if (!(L->moves & NAVM_WALK) || L->node >= N || navState[L->node] == 2)
				continue;
			const f32 *q = P->navNodes[L->node].pos;
			f32 c = navG[cur] + sqrtf((q[0] - cn->pos[0]) * (q[0] - cn->pos[0]) + (q[1] - cn->pos[1]) * (q[1] - cn->pos[1]) +
			                          (q[2] - cn->pos[2]) * (q[2] - cn->pos[2]));
			if (navState[L->node] == 0 || c < navG[L->node]) {
				navG[L->node] = c;
				navFrom[L->node] = (s16)cur;
				navState[L->node] = 1;
			}
		}
	}
	/* back from the goal node; keep the first NPC_MAX_PATH steps (it replans when it gets there) */
	u16 rev[512];
	int m = 0;
	for (s32 k = g; k >= 0 && m < 512; k = navFrom[k])
		rev[m++] = (u16)k;
	int len = m < NPC_MAX_PATH ? m : NPC_MAX_PATH;
	for (int i = 0; i < len; i++)
		n->path[i] = rev[m - 1 - i];
	n->pathLen = (u8)len;
}

/* where to head now: the next node of the route (skipping ahead when the one after is in plain view), or the goal */
static const f32 *steer_target(Npc *n)
{
	const f32 *o = ent_of(n)->origin;
	while (n->pathPos < n->pathLen) {
		const f32 *w = P->navNodes[n->path[n->pathPos]].pos;
		f32 dx = w[0] - o[0], dy = w[1] - o[1];
		int reached = dx * dx + dy * dy < 20.0f * 20.0f && fabsf(w[2] - o[2]) < 72.0f;
		const f32 *next = n->pathPos + 1 < n->pathLen ? P->navNodes[n->path[n->pathPos + 1]].pos : n->goal;
		if (reached || clear_line(n, o, next)) {
			n->pathPos++;
			continue;
		}
		return w;
	}
	return n->goal;
}

void Npc_MoveTo(Npc *n, const f32 pos[3], int run, f32 stopDist)
{
	memcpy(n->goal, pos, 12);
	n->running = (u8)run;
	n->stopDist = stopDist;
	n->moving = 1;
	n->moveFrac = 1;
	n->progBest = 0;
	n->progT = 0;
	n->stuckTime = 0;
	n->replanned = 0;
	plan_path(n);
	if (!n->locked)
		locomotion_seq(n);
}

void Npc_Stop(Npc *n)
{
	n->moving = 0;
	n->pathLen = n->pathPos = 0;
	if (!n->locked)
		locomotion_seq(n);
}

void Npc_FacePoint(Npc *n, const f32 pos[3])
{
	const f32 *o = ent_of(n)->origin;
	f32 dx = pos[0] - o[0], dy = pos[1] - o[1];
	if (dx * dx + dy * dy > 1)
		n->idealYaw = atan2f(dy, dx) * 180.0f / (f32)M_PI;
}

void Npc_Follow(Npc *n, s32 ent, f32 dist)
{
	n->followEnt = ent;
	n->followDist = dist;
	if (ent < 0)
		Npc_Stop(n);
}

static f32 angle_diff(f32 a, f32 b)
{
	f32 d = fmodf(a - b, 360.0f);
	if (d > 180)
		d -= 360;
	if (d < -180)
		d += 360;
	return d;
}

/* one horizontal step with step-up and a single slide along whatever blocks it */
static f32 walk(Npc *n, f32 dx, f32 dy)
{
	f32 *o = ent_of(n)->origin;
	for (int i = 0; i < numNavIgnore; i++) {
		f32 mn[3], mx[3];
		Ent_WorldBounds(navIgnore[i], mn, mx);
		if (o[0] > mn[0] - 16 && o[0] < mx[0] + 16 && o[1] > mn[1] - 16 && o[1] < mx[1] + 16 && o[2] < mx[2] && o[2] + 64 > mn[2])
			Ent_HingePush(navIgnore[i], o, dx / TICK_INTERVAL, dy / TICK_INTERVAL);
	}
	f32 start[3] = {o[0], o[1], o[2]};
	Trace tr;
	f32 end[3] = {o[0] + dx, o[1] + dy, o[2]};
	trace(&tr, n, o, end);
	if (tr.startsolid && tr.ent > 0 && shove(n, &tr, dx, dy)) {
		/* overlapping a prop it's pushing: walk on through it */
		passProp = tr.ent;
		trace(&tr, n, o, end);
		passProp = -1;
	}
	if (tr.startsolid) {
		/* overlapping something (sat in a seat, let go by a train): find free space close by first, so it
		 * doesn't walk through walls; only if there's none, just move */
		static const s8 nudge[][3] = {{0, 0, 4}, {0, 0, 12}, {0, 0, 24}, {0, 0, 36}, {16, 0, 4}, {-16, 0, 4}, {0, 16, 4},
		                              {0, -16, 4}, {32, 0, 8}, {-32, 0, 8}, {0, 32, 8}, {0, -32, 8}};
		for (u32 k = 0; k < sizeof(nudge) / sizeof(nudge[0]); k++) {
			f32 p[3] = {o[0] + nudge[k][0], o[1] + nudge[k][1], o[2] + nudge[k][2]};
			Trace t0;
			trace(&t0, n, p, p);
			if (!t0.startsolid) {
				memcpy(o, p, 12);
				return 0;
			}
		}
		memcpy(o, end, 12);
		return sqrtf(dx * dx + dy * dy);
	}
	if (tr.fraction < 1.0f && shove(n, &tr, dx, dy)) {
		/* through the prop it's pushing (it moves out of the way over the next ticks) */
		passProp = tr.ent;
		trace(&tr, n, o, end);
		passProp = -1;
	}
	if (tr.fraction < 1.0f) {
		f32 len = sqrtf(dx * dx + dy * dy);
		if (len > 0.001f) {
			f32 dir[3] = {dx / len, dy / len, 0};
			Ent_NpcOpenDoor(n->ent, o, dir); /* Source NPCs open doors in their way */
		}
		/* try stepping up */
		f32 up[3] = {o[0], o[1], o[2] + STEP}, upEnd[3] = {end[0], end[1], end[2] + STEP};
		Trace t2;
		trace(&t2, n, o, up);
		if (!t2.startsolid) {
			trace(&t2, n, t2.endpos, upEnd);
			if (t2.fraction > tr.fraction + 0.05f) {
				f32 down[3] = {t2.endpos[0], t2.endpos[1], t2.endpos[2] - STEP};
				Trace t3;
				trace(&t3, n, t2.endpos, down);
				if (!t3.startsolid && (t3.fraction == 1.0f || t3.normal[2] > 0.7f)) {
					memcpy(o, t3.endpos, 12);
					goto done;
				}
			}
		}
		memcpy(o, tr.endpos, 12);
		/* slide along the wall */
		f32 left = 1.0f - tr.fraction;
		f32 rx = dx * left, ry = dy * left;
		f32 dot = rx * tr.normal[0] + ry * tr.normal[1];
		rx -= tr.normal[0] * dot;
		ry -= tr.normal[1] * dot;
		f32 e2[3] = {o[0] + rx, o[1] + ry, o[2]};
		trace(&tr, n, o, e2);
		if (!tr.startsolid)
			memcpy(o, tr.endpos, 12);
	} else {
		memcpy(o, tr.endpos, 12);
	}
done:;
	f32 mx = o[0] - start[0], my = o[1] - start[1];
	return sqrtf(mx * mx + my * my);
}

static void gravity(Npc *n, f32 dt)
{
	f32 *o = ent_of(n)->origin;
	f32 drop = n->onGround ? STEP : (n->fallSpeed * dt + 1.0f);
	f32 end[3] = {o[0], o[1], o[2] - drop};
	Trace tr;
	trace(&tr, n, o, end);
	if (tr.startsolid)
		return;
	if (tr.fraction < 1.0f && tr.normal[2] > 0.7f) {
		memcpy(o, tr.endpos, 12);
		n->onGround = 1;
		n->groundEnt = tr.ent;
		n->fallSpeed = 0;
	} else if (n->onGround && tr.fraction == 1.0f) {
		n->groundEnt = -1;
		/* walked off a ledge taller than a step */
		n->onGround = 0;
		n->fallSpeed = 0;
	} else if (!n->onGround) {
		memcpy(o, tr.endpos, 12);
		n->fallSpeed += GRAVITY * dt;
		if (n->fallSpeed > 1500)
			n->fallSpeed = 1500;
	}
}

static f32 move_speed(const Npc *n)
{
	const PackSeq *s = seq_ptr(n, n->running ? n->skel->seqRun : n->skel->seqWalk);
	f32 v = s && s->speed > 1 ? s->speed : (n->running ? RUN_SPEED : WALK_SPEED);
	return v;
}

static void prop_think(Npc *n, f32 dt)
{
	n->t += dt;
	if (n->blend < 1.0f) {
		n->blend += dt / FADE_TIME;
		n->prevT += dt;
		if (n->blend > 1.0f)
			n->blend = 1.0f;
	}
	/* a one-shot animation finished: Source's prop_dynamic says so and goes back to its default */
	if (n->hold && n->seqDuration > 0 && n->t >= n->seqDuration && n->locked == 2) {
		n->locked = 1;
		Ent_FireOutput(n->ent, "onanimationdone", 0);
		if (n->defaultSeq >= 0 && n->defaultSeq != n->seq) {
			set_seq(n, n->defaultSeq, 1);
			n->hold = 0;
		}
	}
}


static int friendly(const char *cn)
{
	return !strcasecmp(cn, "npc_citizen") || !strcasecmp(cn, "npc_barney") || !strcasecmp(cn, "npc_alyx") ||
	       !strcasecmp(cn, "npc_vortigaunt") || !strcasecmp(cn, "npc_kleiner") || !strcasecmp(cn, "npc_eli") ||
	       !strcasecmp(cn, "npc_mossman") || !strcasecmp(cn, "npc_monk") || !strcasecmp(cn, "npc_dog");
}

/* Source's friendly NPCs get out of the player's way when he walks into them, then go back */
static void make_way(Npc *n)
{
	Entity *e = ent_of(n);
	/* Source (CAI_PlayerAlly::Touch, TestPlayerPushing): not with SF_NPC_NO_PLAYER_PUSHAWAY, not while
	 * speaking, and only the idle/alert schedules give way: never during a script, scene, act-busy or a
	 * follow. Nor on a moving train or lift: there's no route off it (the riders walked in place) */
	if (!player || n->locked || n->script >= 0 || n->followEnt >= 0 || !friendly(e->classname) ||
	    (e->spawnflags & (1u << 14)) || Scene_NpcBusy(n) || n->groundEnt > 0 || n->train || Ent_NpcActBusy(n->ent)) {
		if (n->aside && n->groundEnt > 0) /* (a home left behind on the platform means nothing now) */
			n->aside = 0;
		return;
	}
	f32 dx = e->origin[0] - player->origin[0], dy = e->origin[1] - player->origin[1];
	f32 d = sqrtf(dx * dx + dy * dy);
	if (n->aside && !n->moving && npcClock >= n->asideReturn && d > 80.0f) {
		n->aside = 0;
		Npc_MoveTo(n, n->asideHome, 0, 4.0f);
		return;
	}
	if (n->moving || d > 44.0f || d < 0.01f || fabsf(e->origin[2] - player->origin[2]) > 48.0f)
		return;
	f32 vx = player->velocity[0], vy = player->velocity[1];
	f32 sp = sqrtf(vx * vx + vy * vy);
	if (sp < 30.0f || (vx * dx + vy * dy) / (sp * d) < 0.5f)
		return;
	/* sideways from the player's path, to whichever side the NPC already leans to */
	f32 sx = -vy / sp, sy = vx / sp;
	if (sx * dx + sy * dy < 0) {
		sx = -sx;
		sy = -sy;
	}
	if (!n->aside) {
		memcpy(n->asideHome, e->origin, 12);
		n->aside = 1;
	}
	n->asideReturn = npcClock + 4.0f;
	f32 to[3] = {e->origin[0] + sx * 56.0f + vx / sp * 16.0f, e->origin[1] + sy * 56.0f + vy / sp * 16.0f, e->origin[2]};
	Npc_MoveTo(n, to, 0, 2.0f);
}


/* ---- combat: a cut-down version of Source's combat AI. Combine soldiers and Metro Police shoot (only once
 * the "gordon_precriminal" global is off, as in HL2), zombies, headcrabs and antlions close in and hit.
 * They face the player, keep at their weapon's range using the node graph, fire in bursts with Source's
 * NPC damage (sk_npc_dmg_*), and hit more often the closer and slower the player is. */
enum { NW_NONE, NW_PISTOL, NW_SMG1, NW_AR2, NW_SHOTGUN, NW_STUNSTICK, NW_CLAW, NW_BITE, NW_ANTLION };

static int combine_class(const char *cn)
{
	return !strcasecmp(cn, "npc_metropolice") || !strcasecmp(cn, "npc_combine_s") || !strcasecmp(cn, "npc_combine");
}

static u8 npc_weapon(const Npc *n)
{
	Entity *e = ent_of(n);
	const char *cn = e->classname;
	if (!strncasecmp(cn, "npc_zombie", 10) || !strcasecmp(cn, "npc_fastzombie") || !strcasecmp(cn, "npc_poisonzombie") ||
	    !strcasecmp(cn, "npc_zombine"))
		return NW_CLAW;
	if (!strncasecmp(cn, "npc_headcrab", 12))
		return NW_BITE;
	if (!strcasecmp(cn, "npc_antlion"))
		return NW_ANTLION;
	const char *w = n->held;
	if (!combine_class(cn) && !w[0])
		return NW_NONE; /* (rebels, Alyx, Barney fight with what they carry) */
	if (!w[0])
		return !strcasecmp(cn, "npc_metropolice") ? NW_STUNSTICK : NW_SMG1;
	if (!strcasecmp(w, "weapon_pistol"))
		return NW_PISTOL;
	if (!strcasecmp(w, "weapon_smg1"))
		return NW_SMG1;
	if (!strcasecmp(w, "weapon_ar2"))
		return NW_AR2;
	if (!strcasecmp(w, "weapon_shotgun"))
		return NW_SHOTGUN;
	if (!strcasecmp(w, "weapon_stunstick"))
		return NW_STUNSTICK;
	return NW_NONE;
}

static f32 frand(void) { return (f32)(rand() & 0x7FFF) / 32767.0f; }

/* on the player's side (likes him): Alyx, Barney, rebels, vortigaunts */
static int is_ally_npc(const Npc *n) { return Rel_Disposition(n->ent, 0) == REL_LIKE; }

static int is_hostile(const Npc *n) { return n->weapon && !n->dead && !n->prop; }

/* whether n would attack the player now */
static int fights_player(const Npc *n)
{
	Entity *e = ent_of(n);
	if (combine_class(e->classname) && Ent_GlobalState("gordon_precriminal") == 1)
		return 0; /* before Gordon is a fugitive the Combine only push him around */
	return Rel_Disposition(n->ent, 0) == REL_HATE && !Ent_NoTarget();
}

/* where an enemy is aimed at: the player's chest, an NPC's middle, a bullseye's origin */
static void enemy_pos(s32 en, f32 out[3])
{
	if (en == 0) {
		out[0] = player->origin[0];
		out[1] = player->origin[1];
		out[2] = player->origin[2];
		return;
	}
	const f32 *o = Ent_Origin(en);
	memcpy(out, o, 12);
}

static int enemy_alive(s32 en)
{
	if (en == 0)
		return 1;
	if (en < 0 || (u32)en >= entCount)
		return 0;
	Entity *e = Ent_List(NULL) + en;
	if (e->flags & (EF_KILLED | EF_DISABLED))
		return 0;
	Npc *o = Npc_OfEntity(en);
	return !o || (!o->dead && !o->prop);
}

/* CAI_BaseNPC::GetEnemies / BestEnemy: the nearest thing it hates that it can see (the player, another
 * NPC, an npc_bullseye a script put up), looked for a few times a second */
/* CAI_BaseNPC::FInViewCone: m_flFieldOfView by class (a dot product with its facing) */
static f32 view_field(const Npc *n)
{
	const char *cn = ent_of(n)->classname;
	if (!strcmp(cn, "npc_combine_s") || !strcmp(cn, "npc_metropolice"))
		return -0.2f;
	if (!strncmp(cn, "npc_zombie", 10) || !strncmp(cn, "npc_fastzombie", 14) || !strncmp(cn, "npc_poisonzombie", 16) ||
	    !strcmp(cn, "npc_zombine"))
		return 0.2f;
	if (!strncmp(cn, "npc_antlion", 11))
		return -0.5f;
	if (!strcmp(cn, "npc_citizen") || !strcmp(cn, "npc_alyx") || !strcmp(cn, "npc_barney") ||
	    !strcmp(cn, "npc_vortigaunt") || !strcmp(cn, "npc_monk"))
		return 0.02f;
	if (!strcmp(cn, "npc_manhack"))
		return -1.0f;
	return 0.5f; /* (headcrabs and the rest) */
}

static int in_view(const Npc *n, const f32 eye[3], const f32 p[3])
{
	if (n->alertUntil > npcClock)
		return 1; /* (alerted: looking all around) */
	f32 d[2] = {p[0] - eye[0], p[1] - eye[1]};
	f32 l = sqrtf(d[0] * d[0] + d[1] * d[1]);
	if (l < 1)
		return 1;
	f32 yaw = n->yaw * (f32)M_PI / 180.0f;
	return (d[0] * cosf(yaw) + d[1] * sinf(yaw)) / l >= view_field(n);
}

void Npc_Alert(Npc *n, f32 seconds)
{
	if (n && npcClock + seconds > n->alertUntil)
		n->alertUntil = npcClock + seconds;
}

/* until when someone is still saying something (the AI talk semaphores env_speaker waits on); 0 none */
f32 Npc_TalkingUntil(void)
{
	f32 t = 0;
	for (u32 i = 0; i < numNpcs; i++)
		if (!npcs[i].dead && npcs[i].speakUntil > t)
			t = npcs[i].speakUntil;
	return t; /* (scene time, which runs with the entity clock) */
}

void Npc_HearCombat(const f32 pos[3], f32 radius)
{
	for (u32 i = 0; i < numNpcs; i++) {
		Npc *n = &npcs[i];
		if (n->dead || n->prop || n->inAScript) /* (CAI_Senses: nothing heard while in a script) */
			continue;
		const f32 *o = ent_of(n)->origin;
		f32 d[3] = {o[0] - pos[0], o[1] - pos[1], o[2] - pos[2]};
		if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < radius * radius)
			Npc_Alert(n, 5.0f);
	}
}

static s32 pick_enemy(Npc *n, const f32 eye[3], f32 sight, f32 *outDist)
{
	s32 best = -1;
	f32 bestD = sight;
	/* an enemy a script gave it (aiscripted_schedule "enemy is goal"): that one */
	s32 forced = AiEnts_ForcedEnemy(n->ent);
	if (forced >= 0) {
		const f32 *o = forced == 0 ? player->origin : Ent_Origin(forced);
		f32 d[3] = {o[0] - eye[0], o[1] - eye[1], o[2] - eye[2]};
		*outDist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		return forced;
	}
	if (fights_player(n)) {
		f32 d;
		f32 pd[3] = {player->origin[0] - eye[0], player->origin[1] - eye[1], player->origin[2] + 64 - eye[2]};
		d = sqrtf(pd[0] * pd[0] + pd[1] * pd[1] + pd[2] * pd[2]);
		/* it has to be looking his way to notice him (once it's fighting him, it keeps track) */
		if (d < bestD && (n->enemy == 0 || in_view(n, eye, player->origin)) && Ent_PlayerVisible(eye, &d)) {
			best = 0;
			bestD = d;
		}
	}
	/* other NPCs and bullseyes: the nearest few that it hates, then whether it sees them */
	s32 cand[4];
	f32 cd[4];
	int nc = 0;
	u32 cnt;
	Entity *ents = Ent_List(&cnt);
	for (u32 i = 1; i < cnt; i++) {
		if ((s32)i == n->ent || (ents[i].flags & (EF_KILLED | EF_DISABLED)))
			continue;
		int bull = !strcmp(ents[i].classname, "npc_bullseye");
		if (!bull && ents[i].npcModel < 0)
			continue;
		if (!bull && !enemy_alive((s32)i))
			continue;
		const f32 *o = ents[i].origin;
		f32 d[3] = {o[0] - eye[0], o[1] - eye[1], o[2] + 32 - eye[2]};
		f32 dd = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dd >= bestD || Rel_Disposition(n->ent, (s32)i) != REL_HATE)
			continue;
		if ((s32)i != n->enemy && !bull && !in_view(n, eye, o))
			continue;
		int k;
		if (nc < 4)
			k = nc++;
		else if (dd < cd[3])
			k = 3; /* (full: it replaces the farthest of the four) */
		else
			continue;
		cand[k] = (s32)i;
		cd[k] = dd;
		for (int a = k; a > 0 && cd[a] < cd[a - 1]; a--) {
			f32 tf = cd[a]; cd[a] = cd[a - 1]; cd[a - 1] = tf;
			s32 ti = cand[a]; cand[a] = cand[a - 1]; cand[a - 1] = ti;
		}
	}
	for (int k = 0; k < nc; k++) {
		if (cd[k] >= bestD)
			break;
		const f32 *o = ents[cand[k]].origin;
		f32 to[3] = {o[0], o[1], o[2] + 32};
		Trace tr;
		CM_SetIgnoreEnt(n->ent);
		CM_TraceWorld(&tr, eye, to, MASK_SHOT);
		CM_SetIgnoreEnt(-1);
		if (tr.fraction >= 0.97f || tr.ent == cand[k]) {
			best = cand[k];
			bestD = cd[k];
			break;
		}
	}
	*outDist = bestD;
	return best;
}

/* the hit lands on the enemy: the player through the HUD's damage, anything else through Ent_Damage */
static void hurt_enemy(Npc *n, s32 en, f32 dmg, u32 type, const f32 eye[3])
{
	if (en == 0) {
		/* Point Insertion's knockout cops only club him down for the scripted blackout (they never kill) */
		const char *nm = ent_of(n)->name;
		if (nm && !strncmp(nm, "npc_knockout_cop", 16) && Weap_PlayerHealth() <= dmg + 5)
			return;
		Ent_HurtPlayer(dmg, type, eye);
		return;
	}
	f32 at[3];
	enemy_pos(en, at);
	at[2] += 32;
	f32 d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
	f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
	for (int k = 0; k < 3; k++)
		d[k] /= l;
	Ent_Damage(en, dmg, d, at);
	(void)n;
}

/* the sequence's sound events between last tick's cycle and this one: footsteps (the run variant when it
 * moves fast, like C_BaseAnimating's CL_EVENT_FOOTSTEP), AE_CL_PLAYSOUND / SCRIPT_EVENT_SOUND */
static void play_event_sound(const Npc *n, const PackSeqEvent *ev)
{
	Entity *e = ent_of(n);
	u16 snd = (n->running && ev->pad != 0xFFFF) ? ev->pad : ev->option;
	if (snd < P->numSounds) {
		f32 at[3] = {e->origin[0], e->origin[1], e->origin[2] + 4};
		Audio_Play(&P->sounds[snd], at, 1.0f, SNDLVL_DEFAULT);
	}
}

static void anim_sounds(Npc *n)
{
	if (!P->seqSounds || !P->numSeqSounds || n->seq < 0)
		return;
	u32 gs;
	f32 c = Npc_Cycle(n, &gs);
	if (gs != n->sndSeq) {
		n->sndSeq = gs;
		n->sndCycle = c;
		return;
	}
	f32 last = n->sndCycle;
	n->sndCycle = c;
	if (c == last)
		return;
	/* the events are in sequence order: find this sequence's */
	u32 lo = 0, hi = P->numSeqSounds;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (P->seqSounds[mid].seq < gs)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (u32 k = lo; k < P->numSeqSounds && P->seqSounds[k].seq == gs; k++) {
		f32 at = P->seqSounds[k].cycle;
		int hit = c > last ? (at > last && at <= c) : (at > last || at <= c); /* (wrapped round a loop) */
		if (hit)
			play_event_sound(n, &P->seqSounds[k]);
	}
}

/* a shot that misses the player hits something near him: its bullet impact sound (concrete, metal...) */
static void miss_impact(const f32 eye[3], const f32 tpos[3])
{
	f32 at[3] = {tpos[0] + (frand() - 0.5f) * 96.0f, tpos[1] + (frand() - 0.5f) * 96.0f, tpos[2] + frand() * 72.0f};
	f32 d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
	f32 end[3] = {eye[0] + d[0] * 2, eye[1] + d[1] * 2, eye[2] + d[2] * 2};
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_BoxTrace(&tr, eye, end, z, z, MASK_SHOT);
	if (tr.fraction < 1 && !tr.startsolid && tr.ent <= 0) {
		Weap_SurfaceSound(tr.surface, 2, tr.endpos, 0.7f);
		f32 l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		f32 dir[3] = {d[0] / l, d[1] / l, d[2] / l};
		Weap_BulletImpact(&tr, dir);
	}
}

void Npc_SetWeapon(Npc *n, const char *cls)
{
	snprintf(n->held, sizeof(n->held), "%s", cls ? cls : "");
	n->heldItem = -2;
	n->weapon = npc_weapon(n);
	n->actsReady = 0;
}

static s16 first_seq(const Npc *n, const char *const *names)
{
	for (; *names; names++) {
		s16 s = find_seq(n, *names);
		if (s >= 0)
			return s;
	}
	return -1;
}

/* the activities Source's NPCs fight with, for the weapon in hand (ACT_IDLE_ANGRY_SMG1, ACT_RANGE_ATTACK_*,
 * the fire gesture layered over walking, ACT_RUN_AIM_RIFLE...): the converter stores each under its name */
static void resolve_acts(Npc *n)
{
	static const char *const angry[][4] = {
		{"ACT_IDLE_ANGRY_PISTOL", "ACT_IDLE_ANGRY", 0}, {"ACT_IDLE_ANGRY_SMG1", "ACT_IDLE_ANGRY", 0},
		{"ACT_IDLE_ANGRY_SMG1", "ACT_IDLE_ANGRY", 0}, {"ACT_IDLE_ANGRY_SHOTGUN", "ACT_IDLE_ANGRY_SMG1", "ACT_IDLE_ANGRY", 0},
		{"ACT_IDLE_ANGRY_MELEE", "ACT_IDLE_ANGRY", 0}, {"ACT_IDLE_ANGRY", 0}};
	static const char *const fire[][4] = {
		{"ACT_RANGE_ATTACK_PISTOL", "ACT_RANGE_ATTACK1", 0}, {"ACT_RANGE_ATTACK_SMG1", "ACT_RANGE_ATTACK1", 0},
		{"ACT_RANGE_ATTACK_AR2", "ACT_RANGE_ATTACK1", 0}, {"ACT_RANGE_ATTACK_SHOTGUN", "ACT_RANGE_ATTACK1", 0},
		{"ACT_MELEE_ATTACK_SWING", "ACT_MELEE_ATTACK1", 0}, {"ACT_MELEE_ATTACK1", 0}};
	static const char *const gest[][4] = {
		{"ACT_GESTURE_RANGE_ATTACK_PISTOL", "ACT_GESTURE_RANGE_ATTACK1", 0},
		{"ACT_GESTURE_RANGE_ATTACK_SMG1", "ACT_GESTURE_RANGE_ATTACK1", 0},
		{"ACT_GESTURE_RANGE_ATTACK_AR2", "ACT_GESTURE_RANGE_ATTACK1", 0},
		{"ACT_GESTURE_RANGE_ATTACK_SHOTGUN", "ACT_GESTURE_RANGE_ATTACK1", 0},
		{"ACT_GESTURE_MELEE_ATTACK_SWING", "ACT_GESTURE_MELEE_ATTACK1", 0}, {0}};
	static const char *const run[][4] = {
		{"ACT_RUN_AIM_PISTOL", "ACT_RUN_AIM", 0}, {"ACT_RUN_AIM_RIFLE", "ACT_RUN_AIM", 0},
		{"ACT_RUN_AIM_RIFLE", "ACT_RUN_AIM", 0}, {"ACT_RUN_AIM_SHOTGUN", "ACT_RUN_AIM_RIFLE", "ACT_RUN_AIM", 0}, {0}, {0}};
	static const char *const walk[][4] = {
		{"ACT_WALK_AIM_PISTOL", "ACT_WALK_AIM", 0}, {"ACT_WALK_AIM_RIFLE", "ACT_WALK_AIM", 0},
		{"ACT_WALK_AIM_RIFLE", "ACT_WALK_AIM", 0}, {"ACT_WALK_AIM_SHOTGUN", "ACT_WALK_AIM_RIFLE", "ACT_WALK_AIM", 0}, {0}, {0}};
	int w = n->weapon == NW_PISTOL ? 0 : n->weapon == NW_SMG1 ? 1 : n->weapon == NW_AR2 ? 2 : n->weapon == NW_SHOTGUN ? 3
	      : n->weapon == NW_STUNSTICK ? 4 : 5;
	n->actAngry = first_seq(n, angry[w]);
	n->actFire = first_seq(n, fire[w]);
	n->actGest = first_seq(n, gest[w]);
	n->actRunAim = first_seq(n, run[w]);
	n->actWalkAim = first_seq(n, walk[w]);
	n->actsReady = 1;
}

/* a shot or a swing: the fire gesture over whatever it's doing, or the attack sequence itself */
static void fire_anim(Npc *n)
{
	if (!n->actsReady)
		resolve_acts(n);
	n->muzzleUntil = npcClock + 0.06f;
	if (n->actGest >= 0) {
		n->fireBase = 0;
		n->fireT = 0;
		n->fireDur = Anim_SeqDuration(P, n->skel, n->actGest);
	} else if (n->actFire >= 0 && !n->moving && !n->locked) {
		set_seq(n, n->actFire, 1);
		n->fireBase = 1;
		n->fireT = 0;
		n->fireDur = n->seqDuration;
	}
}

static void combat_anim(Npc *n, f32 dt)
{
	if (n->fireDur > 0) {
		n->fireT += dt;
		if (!n->fireBase) {
			int done = n->fireT >= n->fireDur;
			Npc_SetGesture(n, NPC_MAX_GESTURES - 1, done ? -1 : n->actGest, done ? 0 : n->fireT / n->fireDur, 1.0f,
			               n->fireT);
		}
		if (n->fireT >= n->fireDur)
			n->fireDur = 0;
	}
}

static f32 npcClockNow(void) { return npcClock; }
int Npc_InCombat(const Npc *n) { return npcClock < n->combatUntil; }
int Npc_MuzzleFlash(const Npc *n) { return npcClock < n->muzzleUntil; }

static void combat(Npc *n, f32 dt)
{
	Entity *e = ent_of(n);
	if (!is_hostile(n) || n->locked || n->script >= 0 || Scene_NpcBusy(n) || !player)
		return;
	const PackNpcModel *m = model_of(n);
	f32 eye[3] = {e->origin[0], e->origin[1], e->origin[2] + (m->eyeHeight > 0 ? m->eyeHeight : 64.0f)};
	int melee = n->weapon >= NW_STUNSTICK;
	f32 sight = melee ? 1024.0f : 2048.0f;
	/* Source's NPCs look ten times a second (CAI_Senses at their think rate), not every tick, and only
	 * as far as they can see: the line-of-sight trace is the costly part */
	f32 dist = sight;
	if (npcClock >= n->senseNext) {
		n->senseNext = npcClock + (n->enemy > 0 ? 0.3f : 0.1f);
		s32 en = pick_enemy(n, eye, sight, &dist);
		n->senseSeen = en >= 0;
		if (en >= 0) {
			if (en != n->enemy && en > 0 && !strcmp(Ent_List(NULL)[en].classname, "npc_bullseye"))
				Ent_FireOutput(en, "ontargeted", n->ent);
			if (n->enemy < 0) { /* CAI_BaseNPC: OnFoundEnemy, OnFoundPlayer */
				Ent_FireOutput(n->ent, "onfoundenemy", en);
				if (en == 0)
					Ent_FireOutput(n->ent, "onfoundplayer", 0);
			}
			n->enemy = en;
			n->enemyDist = dist;
		}
	}
	/* an enemy it no longer hates (an ai_relationship changed it, the player stopped being a target) */
	if (n->enemy >= 0 && AiEnts_ForcedEnemy(n->ent) != n->enemy &&
	    (n->enemy == 0 ? !fights_player(n) : Rel_Disposition(n->ent, n->enemy) != REL_HATE)) {
		n->enemy = -1;
		n->senseSeen = 0;
		n->lastSeen = 0;
	}
	if (n->enemy >= 0 && !enemy_alive(n->enemy)) {
		n->enemy = -1;
		n->senseSeen = 0;
		n->lastSeen = 0;
	}
	if (n->enemy < 0)
		return;
	s32 en = n->enemy;
	f32 tpos[3];
	enemy_pos(en, tpos);
	{
		f32 pd[3] = {tpos[0] - eye[0], tpos[1] - eye[1], tpos[2] + (en == 0 ? 64 : 32) - eye[2]};
		dist = sqrtf(pd[0] * pd[0] + pd[1] * pd[1] + pd[2] * pd[2]);
	}
	int seen = n->senseSeen;
	if (seen && dist < sight) {
		n->lastSeen = npcClock;
		memcpy(n->seenPos, tpos, 12);
	}
	if (npcClock - n->lastSeen > 8.0f || n->lastSeen == 0) {
		Ent_FireOutput(n->ent, n->enemy == 0 ? "onlostplayerlos" : "onlostenemylos", n->enemy);
		n->enemy = -1;
		return; /* lost it */
	}
	n->combatUntil = npcClock + 0.5f; /* (combat stance: angry idle, aimed running) */
	if (!n->actsReady)
		resolve_acts(n);
	combat_anim(n, dt);
	f32 dx = tpos[0] - e->origin[0], dy = tpos[1] - e->origin[1];
	f32 flat = sqrtf(dx * dx + dy * dy);
	/* keep the right distance: melee closes in, guns stay at 300-700 and in sight */
	f32 want = melee ? 40.0f : 450.0f;
	int needMove = melee ? flat > 56.0f : (!seen || flat > 750.0f);
	/* allies hold where they are (following the player, a script's spot) and shoot from there */
	if (!melee && is_ally_npc(n))
		needMove = 0;
	if (needMove && npcClock >= n->rechase) {
		n->rechase = npcClock + 1.0f;
		n->chasing = 1;
		Npc_MoveTo(n, seen ? tpos : n->seenPos, 1, seen ? want : 16.0f);
	} else if (!needMove && n->chasing && n->moving) {
		Npc_Stop(n);
		n->chasing = 0;
	}
	if (!seen)
		return;
	if (!n->moving)
		n->idealYaw = atan2f(dy, dx) * 180.0f / (f32)M_PI;
	if (npcClock < n->nextAttack)
		return;
	/* facing roughly the player? */
	f32 rel = atan2f(dy, dx) * 180.0f / (f32)M_PI - n->yaw;
	rel = fmodf(rel + 540.0f, 360.0f) - 180.0f;
	if (fabsf(rel) > 30.0f && !n->moving)
		return;
	f32 pv = en == 0 ? sqrtf(player->velocity[0] * player->velocity[0] + player->velocity[1] * player->velocity[1]) : 0;
	f32 hit = 1.0f - dist / 1600.0f;
	hit = hit < 0.12f ? 0.12f : (hit > 0.85f ? 0.85f : hit);
	if (pv > 150.0f)
		hit *= 0.65f; /* running targets are harder to hit */
	switch (n->weapon) {
	case NW_PISTOL:
		Weap_PlaySound(X_NPC_PISTOL, eye, 1.0f);
		fire_anim(n);
		if (frand() < hit)
			hurt_enemy(n, en, 3.0f, DMG_BULLET, eye);
		else
			miss_impact(eye, tpos);
		n->nextAttack = npcClock + 0.5f + frand() * 0.6f;
		break;
	case NW_SMG1:
	case NW_AR2:
		Weap_PlaySound(n->weapon == NW_AR2 ? X_NPC_AR2 : X_NPC_SMG1, eye, 1.0f);
		fire_anim(n);
		if (frand() < hit * 0.8f)
			hurt_enemy(n, en, n->weapon == NW_AR2 ? 8.0f : 3.0f, DMG_BULLET, eye);
		else
			miss_impact(eye, tpos);
		if (n->burst == 0)
			n->burst = (u8)(3 + (rand() % 3));
		n->burst--;
		n->nextAttack = npcClock + (n->burst ? (n->weapon == NW_AR2 ? 0.1f : 0.075f) : 0.8f + frand() * 0.8f);
		break;
	case NW_SHOTGUN:
		if (dist > 900)
			break;
		Weap_PlaySound(X_NPC_SHOTGUN, eye, 1.0f);
		fire_anim(n);
		for (int k = 0; k < 8; k++)
			if (frand() < hit * (dist < 256 ? 1.0f : 0.5f))
				hurt_enemy(n, en, 3.0f, DMG_BULLET, eye);
			else if (k < 2)
				miss_impact(eye, tpos);
		n->nextAttack = npcClock + 1.1f + frand() * 0.5f;
		break;
	default: /* melee */
		if (flat > 64.0f || fabsf(tpos[2] - e->origin[2]) > 56.0f)
			break;
		{
			static const s8 snd[] = {X_NPC_STUNSTICK, X_ZOMBIE_HIT, X_HEADCRAB_BITE, X_ANTLION_HIT};
			static const u8 dmg[] = {10, 10, 5, 5};
			int k = n->weapon - NW_STUNSTICK;
			Weap_PlaySound(snd[k], eye, 1.0f);
			fire_anim(n);
			const char *cn = e->classname;
			if (en == 0 && n->weapon == NW_BITE && (!strcasecmp(cn, "npc_headcrab_black") || !strcasecmp(cn, "npc_headcrab_poison"))) {
				/* CBlackHeadcrab: the bite takes the player down to 1 health with poison, which the suit
				 * gives back over time */
				f32 hp = Weap_PlayerHealth();
				if (hp > 1)
					hurt_enemy(n, en, hp - 1, DMG_POISON, eye);
			} else {
				hurt_enemy(n, en, dmg[k], n->weapon == NW_STUNSTICK ? DMG_CLUB : DMG_SLASH, eye);
			}
		}
		n->nextAttack = npcClock + (n->weapon == NW_BITE ? 1.0f : 1.4f);
		break;
	}
}

/* npc_citizen parented to a func_tracktrain (CNPC_Citizen::Spawn / SCHED_CITIZEN_SIT_ON_TRAIN): it can't
 * move (MOVETYPE_NONE) and holds d1_t01_TrainRide_Sit_Idle if it's citizen_train_2, d1_t01_TrainRide_Stand
 * otherwise, nothing interrupting it; once unparented it waits up to a second and plays the matching
 * _Exit sequence, then it's an ordinary citizen. Returns 1 while it is riding (no other thinking). */
static int train_think(Npc *n, Entity *e, f32 dt)
{
	if (n->train == 0 && strcasecmp(e->classname, "npc_citizen"))
		return 0;
	u32 cnt;
	Entity *all = Ent_List(&cnt);
	int onTrain = e->parent > 0 && (u32)e->parent < cnt && all[e->parent].classname &&
	              !strcasecmp(all[e->parent].classname, "func_tracktrain");
	int sitter = e->name && !strcasecmp(e->name, "citizen_train_2");
	if (onTrain) {
		/* (and again if a scene that ended reset it to the idle: in Source the face and gesture scenes
		 * played on the riders leave the schedule's sequence alone) */
		if (n->train != 1 || (n->skel && (n->seq == n->skel->seqIdle || n->seq == n->skel->seqWalk))) {
			Npc_Stop(n);
			n->script = -1;
			n->followEnt = -1;
			n->aside = 0;
			Npc_PlaySequence(n, sitter ? "d1_t01_TrainRide_Sit_Idle" : "d1_t01_TrainRide_Stand", 1);
			n->train = 1;
		}
		/* the train carries the entity (follow_parents); the body keeps its place on it */
		n->yaw = n->idealYaw = e->angles[1];
		n->moving = 0;
		n->onGround = 1;
		n->fallSpeed = 0;
		n->t += dt;
		if (n->blend < 1.0f) {
			n->blend += dt / FADE_TIME;
			n->prevT += dt;
			if (n->blend > 1.0f)
				n->blend = 1.0f;
		}
		return 1;
	}
	if (n->train == 1) {
		n->train = 2;
		n->trainWait = (f32)(rand() % 1000) / 1000.0f; /* TASK_WAIT_RANDOM 1 */
	} else if (n->train == 2) {
		n->trainWait -= dt;
		if (n->trainWait <= 0) {
			f32 d = Npc_PlaySequence(n, sitter ? "d1_t01_TrainRide_Sit_Exit" : "d1_t01_TrainRide_Stand_Exit", 0);
			n->train = 3;
			n->trainWait = d > 0 ? d : 0;
		}
	} else if (n->train == 3) {
		n->trainWait -= dt;
		if (n->trainWait <= 0) {
			n->train = 0;
			if (n->script < 0 && !Scene_NpcBusy(n))
				Npc_ResetSequence(n);
		}
	}
	return 0;
}

static void think(Npc *n, f32 dt)
{
	Entity *e = ent_of(n);
	int fly = flies(e->classname);
	if (!n->prop && !n->dead && train_think(n, e, dt)) {
		sync_entity(n);
		return;
	}
	if (n->prop) {
		prop_think(n, dt);
		return;
	}

	if (n->dead) {
		n->moving = 0;
		Ragdoll *rg = rag_of(n->ent);
		if (rg) {
			rag_step(rg, dt);
			return;
		}
		n->t += dt;
		if (n->blend < 1.0f) {
			n->blend += dt / 0.6f; /* slump into the death pose */
			n->prevT += dt;
			if (n->blend > 1.0f)
				n->blend = 1.0f;
		}
		if (!fly)
			gravity(n, dt);
		sync_entity(n);
		return;
	}

	/* following */
	if (n->followEnt >= 0 && n->script < 0 && !n->locked) {
		f32 tp[3];
		Npc_EntPos(n->followEnt, tp);
		f32 dx = tp[0] - e->origin[0], dy = tp[1] - e->origin[1];
		f32 d = sqrtf(dx * dx + dy * dy);
		if (d > n->followDist + 64.0f)
			Npc_MoveTo(n, tp, d > 300.0f, n->followDist);
		else if (n->moving && d <= n->followDist) {
			Npc_Stop(n);
		}
		if (!n->moving)
			n->faceEnt = n->followEnt;
	}

	if (n->moving) {
		f32 gx = n->goal[0] - e->origin[0], gy = n->goal[1] - e->origin[1];
		f32 gd = sqrtf(gx * gx + gy * gy);
		const f32 *tgt = (!fly && n->pathPos < n->pathLen) ? steer_target(n) : n->goal;
		f32 dx = tgt[0] - e->origin[0], dy = tgt[1] - e->origin[1];
		f32 d = sqrtf(dx * dx + dy * dy);
		int toGoal = tgt == n->goal;
		if (gd <= n->stopDist + 1.0f) {
			Npc_Stop(n);
		} else if (d < 0.5f) {
			/* on a waypoint exactly: the next one is picked next tick */
		} else {
			n->idealYaw = atan2f(dy, dx) * 180.0f / (f32)M_PI;
			f32 step = move_speed(n) * dt;
			if (toGoal && step > d - n->stopDist)
				step = d - n->stopDist;
			if (step > d)
				step = d;
			f32 moved;
			if (fly) {
				f32 *o = e->origin;
				o[0] += dx / d * step;
				o[1] += dy / d * step;
				o[2] += (n->goal[2] - o[2]) * (step / d);
				moved = step;
			} else {
				moved = walk_around(n, dx / d, dy / d, step);
			}
			/* still blocked after steering round it and replanning: as a last resort (so the scripts
			 * waiting on this NPC keep going) put it at the goal, on free floor */
			/* how much of its stride it covered: on the PC the walk animation's own motion moves the NPC
			 * (its legs never move faster than it goes), so a blocked NPC stands rather than walking on
			 * the spot (smoothed over about a fifth of a second) */
			/* (and whether it's getting anywhere: sliding to and fro against a corner or another NPC
			 * covers ground each tick but no distance) */
			if (n->progPos != n->pathPos || n->progBest <= 0) {
				n->progPos = n->pathPos;
				n->progBest = d;
				n->progT = 0;
			} else if (d < n->progBest - 8.0f) {
				n->progBest = d;
				n->progT = 0;
			} else {
				n->progT += dt;
			}
			if (n->progT > 0.75f)
				moved = 0;
			/* blocked right by its mark (a console, a railing, someone standing there): it's there, as near
			 * as the navigator counts it (CAI_Navigator's goal tolerance is about a hull width) */
			if (n->progT > 0.75f && gd <= n->stopDist + 24.0f) {
				Npc_Stop(n);
				goto moved_done;
			}
			f32 frac = step > 1e-4f ? moved / step : 1.0f;
			n->moveFrac += (frac - n->moveFrac) * (dt * 6 > 1 ? 1 : dt * 6);
			if (moved < step * 0.25f)
				n->stuckTime += dt;
			else
				n->stuckTime = 0;
			if (n->stuckTime > 1.5f && !n->replanned && !fly) {
				n->replanned = 1; /* blocked: find a route around (Source's pathfinder would have) */
				plan_path(n);
				n->stuckTime = 0;
			}
			/* still blocked a good while after the replan (Source fails the move and tries again; the
			 * scripts waiting on this NPC must still go on): it's put at the goal, on free floor, but only
			 * where the player can't see it happen */
			if (n->stuckTime > 8.0f) {
				f32 eye[3] = {e->origin[0], e->origin[1], e->origin[2] + 64}, pd;
				if (!Ent_PlayerVisible(eye, &pd) || n->stuckTime > 30.0f) {
					f32 g[3] = {n->goal[0] - gx / gd * n->stopDist, n->goal[1] - gy / gd * n->stopDist, n->goal[2]};
					free_spot(n, g);
					Npc_Teleport(n, g, n->idealYaw);
					n->stuckTime = 0;
					n->moveFrac = 1;
				} else if (n->stuckTime > 8.0f + 3.0f * (n->replanned ? 1 : 0)) {
					n->replanned = 0; /* (and try a route again every few seconds while it waits) */
					n->stuckTime = 1.5f;
				}
			}
		}
	moved_done:;
	} else if (n->faceEnt >= 0) {
		f32 tp[3];
		Npc_EntPos(n->faceEnt, tp);
		Npc_FacePoint(n, tp);
	}

	if (!fly)
		gravity(n, dt);

	f32 diff = angle_diff(n->idealYaw, n->yaw);
	f32 maxTurn = TURN_RATE * dt;
	if (diff > maxTurn)
		diff = maxTurn;
	if (diff < -maxTurn)
		diff = -maxTurn;
	n->yaw += diff;

	if (!n->locked)
		locomotion_seq(n);
	else if (!n->moving)
		auto_move(n, dt);
	/* (a walk / run plays at the pace it's really going: legs and ground agree, as with Source's
	 * movement-driven locomotion) */
	if (!n->locked && n->moving && n->moveFrac < 0.95f && n->seq >= 0 &&
	    (n->seq == n->skel->seqWalk || n->seq == n->skel->seqRun || n->seq == n->actRunAim || n->seq == n->actWalkAim))
		n->t += dt * (n->moveFrac < 0.3f ? 0.3f : n->moveFrac);
	else
		n->t += dt;
	if (n->blend < 1.0f) {
		n->blend += dt / FADE_TIME;
		n->prevT += dt;
		if (n->blend > 1.0f)
			n->blend = 1.0f;
	}
	sync_entity(n);
	if (n->voice)
		Audio_SetPos(n->voice, e->origin);
}

void Npc_Tick(f32 dt)
{
	npcClock += dt;
	u32 c;
	Entity *ents = Ent_List(&c);
	for (u32 i = 0; i < numNpcs; i++) {
		Npc *n = &npcs[i];
		const Entity *e = &ents[n->ent];
		memset(n->flexCtl, 0, sizeof(n->flexCtl)); /* scenes set them again this tick */
		if (e->flags & (EF_KILLED | EF_DISABLED))
			continue;
		think(n, dt);
		if (!n->prop && !n->dead) {
			make_way(n);
			combat(n, dt);
		}
		if (!n->prop && n->skel && P->skelLook)
			update_look(n, dt);
		if (!n->dead && !(e->flags & EF_DISABLED))
			anim_sounds(n);
	}
}

/* ------------------------------------------------------------------ spawn */

/* Half-Life 2's skill-config health on Normal (sk_*_health) */
static f32 npc_health(const char *cls)
{
	static const struct {
		const char *c;
		f32 hp;
	} t[] = {
		{"npc_metropolice", 40}, {"npc_combine_s", 50}, {"npc_cscanner", 30}, {"npc_clawscanner", 30},
		{"npc_manhack", 25}, {"prop_vehicle_apc", 750}, {"prop_vehicle_jeep", 1e9f}, {"prop_vehicle_airboat", 1e9f},
		{"prop_vehicle_prisoner_pod", 1e9f}, {"env_headcrabcanister", 1e9f}, {"combine_mine", 1},
		{"npc_turret_ceiling", 1000}, {"npc_combine_camera", 50}, {"npc_headcrab", 10}, {"npc_headcrab_fast", 10}, {"npc_headcrab_black", 35},
		{"npc_headcrab_poison", 35}, {"npc_zombie", 50}, {"npc_zombie_torso", 50}, {"npc_fastzombie", 50},
		{"npc_fastzombie_torso", 50}, {"npc_poisonzombie", 175}, {"npc_zombine", 100}, {"npc_antlion", 30},
		{"npc_antlionguard", 500}, {"npc_pigeon", 1}, {"npc_crow", 1}, {"npc_seagull", 1}, {"npc_barnacle", 35},
		{"npc_rollermine", 1000}, {"npc_hunter", 210}, {"npc_strider", 350}, {"npc_citizen", 40},
		{"npc_turret_floor", 1000}, {"npc_stalker", 50}, {"npc_combinegunship", 100}, {"npc_helicopter", 5600},
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(cls, t[i].c))
			return t[i].hp;
	return 100;
}

static Npc *npc_init(u32 i)
{
	const Pack *p = P;
	Entity *e = Ent_List(NULL) + i;
	if (e->npcModel < 0 || (u32)e->npcModel >= p->numNpcModels || numNpcs >= npcCap)
		return NULL;
	const PackNpcModel *m = &p->npcModels[e->npcModel];
	if (m->skel >= p->numSkels)
		return NULL;
	{
		Npc *np = &npcs[numNpcs];
		np->ent = (s32)i;
		np->model = (u16)e->npcModel;
		np->skel = &p->skels[m->skel];
		np->yaw = np->idealYaw = e->angles[1];
		np->seq = np->prevSeq = -1;
		np->blend = 1;
		np->followEnt = -1;
		np->faceEnt = -1;
		np->lookEnt = -1;
		np->enemy = -1;
		Npc_ClearGestures(np);
		{
			const char *eq = Ent_KV((s32)i, "additionalequipment");
			np->held[0] = 0;
			if (eq && eq[0] && strcmp(eq, "0") && strncasecmp(eq, "none", 4))
				snprintf(np->held, sizeof(np->held), "%s", eq);
			np->heldItem = -2;
		}
		np->weapon = npc_weapon(np);
		np->actsReady = 0;
		np->script = -1;
		np->speakSound = -1;
		np->health = npc_health(e->classname);
		np->defaultSeq = -1;
		entToNpc[i] = (s16)numNpcs;
		numNpcs++;
		const char *actor = Ent_KV((s32)i, "_actor");
		if (actor && actor[0] == '1') {
			/* a prop with bones: its default animation, or the pose a ragdoll comes to rest in */
			np->prop = 1;
			np->locked = 1;
			const char *def = Ent_KV((s32)i, "defaultanim");
			const char *pose = Ent_KV((s32)i, "_pose");
			s16 ds = def && def[0] ? find_seq(np, def) : -1;
			if (ds >= 0) {
				np->defaultSeq = ds;
				set_seq(np, ds, 1);
				np->hold = 0;
			} else {
				s16 ps = pose ? find_seq(np, pose) : -1;
				if (ps < 0)
					ps = np->skel->numSeqs ? 0 : -1;
				set_seq(np, ps, 1);
				np->hold = 1;
				np->t = 100.0f; /* rest at the end of it */
			}
			np->blend = 1;
			np->prevSeq = -1;
			return np;
		}
		e->angles[0] = e->angles[2] = 0;
		locomotion_seq(np);
		/* desynchronise idles so crowds don't move in lockstep */
		np->t = (f32)(i * 37 % 100) * 0.05f;
		if (!flies(e->classname)) {
			/* settle onto the floor */
			f32 up[3] = {e->origin[0], e->origin[1], e->origin[2] + 8};
			f32 dn[3] = {e->origin[0], e->origin[1], e->origin[2] - 64};
			Trace tr;
			trace(&tr, np, up, dn);
			if (!tr.startsolid && tr.fraction < 1.0f) {
				memcpy(e->origin, tr.endpos, 12);
				np->onGround = 1;
			}
		}
		sync_entity(np);
	}
	return &npcs[numNpcs - 1];
}

/* an NPC entity made while playing (Ent_Clone: npc_maker, template makers): its brain */
Npc *Npc_Add(s32 ent)
{
	if (!npcs || ent < 0 || (u32)ent >= entCount || entToNpc[ent] >= 0)
		return ent >= 0 && (u32)ent < entCount && entToNpc && entToNpc[ent] >= 0 ? &npcs[entToNpc[ent]] : NULL;
	return npc_init((u32)ent);
}

int Npc_Spawn(const Pack *p, Player *pl)
{
	Npc_Free();
	if (rags)
		for (int i = 0; i < MAX_RAGDOLLS; i++)
			rags[i].ent = -1;
	P = p;
	player = pl;
	find_exclusions();
	u32 n;
	Entity *ents = Ent_List(&n);
	entCount = n;
	if (p->numNavNodes && p->navNodes && p->navLinks) {
		navG = malloc(p->numNavNodes * sizeof(f32));
		navFrom = malloc(p->numNavNodes * sizeof(s16));
		navState = malloc(p->numNavNodes);
		if (!navG || !navFrom || !navState) {
			free(navG);
			free(navFrom);
			free(navState);
			navG = NULL;
			navFrom = NULL;
			navState = NULL;
		}
	}
	entToNpc = malloc(n * sizeof(s16));
	if (!entToNpc)
		return -1;
	u32 count = 0;
	for (u32 i = 0; i < n; i++) {
		entToNpc[i] = -1;
		if (ents[i].npcModel >= 0 && (u32)ents[i].npcModel < p->numNpcModels)
			count++;
	}
	if (count > MAX_NPCS)
		count = MAX_NPCS;
	npcCap = count + NPC_SPARE; /* room for what npc_makers and template makers bring in later */
	npcs = calloc(npcCap, sizeof(Npc));
	if (!npcs)
		return -1;
	for (u32 i = 0; i < n && numNpcs < count; i++)
		npc_init(i);
	return 0;
}

void Npc_Free(void)
{
	free(navG);
	free(navFrom);
	free(navState);
	navG = NULL;
	navFrom = NULL;
	navState = NULL;
	free(npcs);
	free(entToNpc);
	npcs = NULL;
	entToNpc = NULL;
	numNpcs = entCount = 0;
}

/* what a dying NPC leaves for the player (CBaseCombatCharacter::Event_Killed drops its weapon; CHalfLife2's
 * NPC_ShouldDropHealth / NPC_ShouldDropGrenade let cops and soldiers drop a health vial or a grenade, at most
 * one of each every 30 seconds: sk_plr_health_drop_time, sk_plr_grenade_drop_time) */
static f32 lastHealthDrop = -100, lastGrenadeDrop = -100;

static void death_drops(Npc *n)
{
	if (n->prop)
		return;
	Entity *e = ent_of(n);
	f32 at[3] = {e->origin[0], e->origin[1], e->origin[2] + 40};
	f32 v[3] = {n->deathPush[0] * 0.3f, n->deathPush[1] * 0.3f, 80};
	static const char *const wcls[] = {NULL, "weapon_pistol", "weapon_smg1", "weapon_ar2", "weapon_shotgun"};
	if (n->weapon >= NW_PISTOL && n->weapon <= NW_SHOTGUN)
		Weap_DropItem(wcls[n->weapon], at, v);
	const char *cn = e->classname;
	int soldier = !strcmp(cn, "npc_combine_s"), cop = !strcmp(cn, "npc_metropolice");
	if ((soldier || cop) && Weap_PlayerHealth() < 100 && npcClock - lastHealthDrop >= 30) {
		f32 hv[3] = {v[0] + 30, v[1], v[2] + 20};
		if (Weap_DropItem("item_healthvial", at, hv))
			lastHealthDrop = npcClock;
	}
	if (soldier && !(e->spawnflags & 0x20000) && npcClock - lastGrenadeDrop >= 30) { /* SF_COMBINE_NO_GRENADEDROP */
		f32 gv[3] = {v[0] - 30, v[1], v[2] + 20};
		if (Weap_DropItem("weapon_frag", at, gv))
			lastGrenadeDrop = npcClock;
	}
}
