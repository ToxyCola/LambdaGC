/* Host test: prop_door_rotating against CPropDoorRotating's rules. For every door:
 *  - shut, its leaf lies in its doorway (open along the leaf, wall past both ends): the model's pose
 *  - opened by the player from in front and from behind (along the door's forward axis), it swings away
 *    from them, unless that way runs into the world and the other doesn't
 *  - "Starts Open" doors start open, and the Open input (no opener) opens forward
 * usage: doortest map.hgm */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include <math.h>
f32 Ent_DoorOpenYaw(s32 idx, const f32 from[3]);
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static void leaf(const PackModel *m, f32 *lx, f32 *ly)
{
	*lx = fabsf(m->mins[0]) > fabsf(m->maxs[0]) ? m->mins[0] : m->maxs[0];
	*ly = fabsf(m->mins[1]) > fabsf(m->maxs[1]) ? m->mins[1] : m->maxs[1];
	if (fabsf(*lx) > fabsf(*ly)) *ly = 0; else *lx = 0;
}
static void leaf_end(const Entity *e, const PackModel *m, f32 yawDeg, f32 t, f32 out[3])
{
	f32 lx, ly; leaf(m, &lx, &ly);
	f32 y = yawDeg * (f32)M_PI / 180, c = cosf(y), s = sinf(y);
	out[0] = e->pos1[0] + (c * lx - s * ly) * t; out[1] = e->pos1[1] + (s * lx + c * ly) * t;
	out[2] = e->pos1[2] + (m->mins[2] + m->maxs[2]) * 0.5f;
}
static int swing_hits(const Entity *e, const PackModel *m, f32 ang)
{
	for (f32 t = 0.2f; t <= 1.0f; t += 0.1f) {
		f32 a[3], b[3], y = e->ang1[1] + (ang - e->ang1[1]) * t;
		leaf_end(e, m, y, 0.35f, a); leaf_end(e, m, y, 0.9f, b);
		Trace tr; CM_TraceWorld(&tr, a, b, CONTENTS_SOLID);
		if (tr.fraction < 0.95f || tr.startsolid) return 1;
	}
	return 0;
}
int main(int argc, char **argv)
{
	int doors = 0, inFrame = 0, opens = 0, away = 0, blockedFlip = 0, wrong = 0, startOpen = 0, startOpenOk = 0, alongY = 0;
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	u32 n; Entity *E = Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		Entity *e = &E[i];
		if (e->cls != EC_PROP_DOOR || e->mdl < 0) continue;
		const PackModel *m = &P.models[e->mdl];
		const char *tn = Ent_KV((s32)i, "targetname"); if (!tn) tn = "-";
		doors++;
		f32 lx, ly; leaf(m, &lx, &ly); alongY += ly != 0;
		/* in its frame when shut */
		int open = 1, walls = 0; f32 p[3];
		for (f32 t = 0.15f; t <= 0.9f; t += 0.15f) { leaf_end(e, m, e->ang1[1], t, p); if (CM_PointContents(p) & CONTENTS_SOLID) open = 0; }
		leaf_end(e, m, e->ang1[1], -0.25f, p); walls += (CM_PointContents(p) & CONTENTS_SOLID) != 0;
		leaf_end(e, m, e->ang1[1], 1.25f, p); walls += (CM_PointContents(p) & CONTENTS_SOLID) != 0;
		inFrame += open && walls == 2;
		if (getenv("DBG") && !(open && walls == 2)) printf("  %s: shut leaf %s, %d of 2 jambs\n", tn, open ? "in the open" : "IN SOLID", walls);
		if (e->spawnflags & 1) {
			startOpen++;
			int ok = e->moveState != 0 && fabsf(e->angles[1] - e->ang1[1]) > 45;
			startOpenOk += ok;
			if (getenv("DBG")) printf("  %s: starts open: state %d yaw %.0f (shut %.0f)\n", tn, e->moveState, e->angles[1], e->ang1[1]);
			continue;
		}
		if (e->moveState != 0) continue;
		f32 yaw = e->ang1[1] * (f32)M_PI / 180, fx = cosf(yaw), fy = sinf(yaw);
		for (int side = -1; side <= 1; side += 2) {
			f32 mid[3]; leaf_end(e, m, e->ang1[1], 0.5f, mid);
			f32 opener[3] = {mid[0] + fx * 36 * side, mid[1] + fy * 36 * side, e->pos1[2] + m->mins[2]};
			f32 ang = Ent_DoorOpenYaw((s32)i, opener);
			f32 end[3]; leaf_end(e, m, ang, 1.0f, end);
			/* which side of the doorway did the leaf's end go to? */
			f32 d = (end[0] - e->pos1[0]) * fx + (end[1] - e->pos1[1]) * fy;
			int isAway = d * side < 0;
			int hit = swing_hits(e, m, ang), other = swing_hits(e, m, 2 * e->ang1[1] - ang);
			opens++;
			if (isAway) away++;
			else if (other && !hit) blockedFlip++; /* toward them because away was blocked: Source's rule */
			else { wrong++; if (getenv("DBG")) printf("  %s side %d: opens TOWARD the opener (to %.0f; this way %s, away %s)\n", tn, side, ang, hit ? "blocked" : "clear", other ? "blocked" : "clear"); }
		}
	}
	printf("%d doors (%d with the leaf along +Y), %d shut in their frames; %d openings: %d away from the opener, %d toward because away is blocked, %d wrong; starts-open %d of %d\n",
	       doors, alongY, inFrame, opens, away, blockedFlip, wrong, startOpenOk, startOpen);
	return 0;
}
