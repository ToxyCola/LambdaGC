/* Trainstation 1's holding pen: the player walks in through the open razor-train gate (from the north), the
 * trigger shuts the gate, the cops block the way back, Barney's door opens after ~6 s and the player walks
 * out east through it. Prints what happens. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static Entity *named(const char *n) { u32 ne; Entity *E = Ent_List(&ne); for (u32 i = 1; i < ne; i++) if (E[i].name && !strcmp(E[i].name, n)) return &E[i]; return NULL; }
static void walk(f32 yaw, f32 secs) { UserCmd c = {0}; c.yaw = yaw; c.forwardmove = 190; for (int t = 0; t < 66 * secs; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	for (int t = 0; t < 66 * 2; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	f32 start[3] = {argc > 2 ? atof(argv[2]) : -4224, argc > 3 ? atof(argv[3]) : -300, -20};
	f32 yaw = argc > 4 ? atof(argv[4]) : -90;
	memcpy(pl.origin, start, 12); pl.origin[2] = 0;
	for (int t = 0; t < 30; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	printf("start (%.0f %.0f %.0f)\n", pl.origin[0], pl.origin[1], pl.origin[2]);
	walk(yaw, 1.2f);
	Entity *g = named("razor_train_gate_2"), *bd = named("barney_door_1"), *clip = named("cage_playerclip");
	printf("after walking in: player (%.0f %.0f) gate frac %.2f state %d clip flags %x\n", pl.origin[0], pl.origin[1], g->frac, g->moveState, clip->flags);
	for (int t = 0; t < 66 * 20; t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		if (t % 33 == 0) {
			u32 dummyN; Entity *ba = named("barney"); Npc *bn = ba ? Npc_OfEntity((s32)(ba - Ent_List(&dummyN))) : NULL;
			if (bn) printf("  %.1fs barney (%.0f %.0f) moving %d goal (%.0f %.0f) stuck %.1f path %d/%d locked %d door frac %.2f\n", Ent_Now(), ba->origin[0], ba->origin[1], bn->moving, bn->goal[0], bn->goal[1], bn->stuckTime, bn->pathPos, bn->pathLen, bn->locked, named("barney_door_1")->frac);
		}
	}
	printf("8 s later: gate frac %.2f, barney door frac %.2f state %d locked %d\n", g->frac, bd->frac, bd->moveState, bd->locked);
	/* walk to the door, then east through it */
	f32 dx = -4120 - pl.origin[0], dy = -520 - pl.origin[1];
	walk(atan2f(dy, dx) * 180 / (f32)M_PI, sqrtf(dx * dx + dy * dy) / 190 + 0.2f);
	f32 x0 = pl.origin[0];
	walk(0, 1.5f);
	printf("player (%.0f %.0f), walked %.0f east through Barney's door (at x %.0f)\n", pl.origin[0], pl.origin[1], pl.origin[0] - x0, bd->origin[0]);
	{
		f32 mn[3], mx[3]; Player_Bounds(&pl, mn, mx);
		f32 e2[3] = {pl.origin[0] + 200, pl.origin[1], pl.origin[2]};
		Trace tr; CM_BoxTrace(&tr, pl.origin, e2, mn, mx, MASK_PLAYERSOLID);
		u32 ne; Entity *E = Ent_List(&ne);
		printf("east trace: frac %.2f at x %.0f, ent %d %s %s, startsolid %d\n", tr.fraction, tr.endpos[0], tr.ent, tr.ent > 0 ? E[tr.ent].classname : "world/static", tr.ent > 0 && E[tr.ent].name ? E[tr.ent].name : "", tr.startsolid);
	}
	{
		u32 ne; Entity *E = Ent_List(&ne);
		f32 mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72};
		for (f32 y = -580; y <= -400; y += 10) {
			f32 a[3] = {-4140, y, pl.origin[2]}, b[3] = {-3950, y, pl.origin[2]};
			Trace tr; CM_BoxTrace(&tr, a, b, mn, mx, MASK_PLAYERSOLID);
			printf("  y %.0f: to x %.0f ent %d %s %s ss %d\n", y, tr.endpos[0], tr.ent, tr.ent > 0 ? E[tr.ent].classname : "world", tr.ent > 0 && E[tr.ent].name ? E[tr.ent].name : "", tr.startsolid);
		}
		Entity *bd2 = named("barney_door_1");
		u32 dn; Entity *ba = named("barney"); ba->origin[2] -= 10000; /* out of the way */
		for (f32 y = -540; y <= -500; y += 5) {
			f32 a[3] = {-4020, y, -31}, b[3] = {-4130, y, -31}, n0[3] = {-13, -13, 0}, n1[3] = {13, 13, 72};
			Trace tr; CM_SetSkipBoxes(1); CM_BoxTrace(&tr, a, b, n0, n1, MASK_NPCSOLID); CM_SetSkipBoxes(0);
			printf("  npc hull west at y %.0f: to x %.0f ent %d %s ss %d normal (%.2f %.2f)\n", y, tr.endpos[0], tr.ent, tr.ent > 0 ? E[tr.ent].classname : "world", tr.startsolid, tr.normal[0], tr.normal[1]);
		}
		(void)dn;
		printf("door origin (%.0f %.0f) angles %.0f mdl %d\n", bd2->origin[0], bd2->origin[1], bd2->angles[1], bd2->mdl);
	}
	return 0;
}
