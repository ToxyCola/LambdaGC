/* A closing func_door shoves the player out of its way (razor_train_gate_2 on Trainstation 1, or the
 * door named on the command line). Prints what happens. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	const char *name = argc > 2 ? argv[2] : "razor_train_gate_2";
	u32 ne; Entity *E = Ent_List(&ne); s32 di = -1;
	for (u32 i = 1; i < ne; i++) if (E[i].name && !strcmp(E[i].name, name) && E[i].bmodel > 0) { di = (s32)i; break; }
	if (di < 0) return 1;
	Entity *d = &E[di];
	Ent_Input(di, "Close", "", 0);
	for (int t = 0; t < 66 * 6; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	const PackBModel *bm = &P.bmodels[d->bmodel];
	f32 closedC[3]; for (int k = 0; k < 3; k++) closedC[k] = d->origin[k] + (bm->mins[k] + bm->maxs[k]) * 0.5f;
	Ent_Input(di, "Open", "", 0);
	for (int t = 0; t < 66 * 6; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	f32 mn[3], mx[3]; Player_Bounds(&pl, mn, mx);
	Trace tr; f32 a[3] = {closedC[0], closedC[1], closedC[2]}, b[3] = {closedC[0], closedC[1], closedC[2] - 300};
	CM_BoxTrace(&tr, a, b, mn, mx, MASK_PLAYERSOLID);
	memcpy(pl.origin, tr.endpos, 12);
	f32 p0[3]; memcpy(p0, pl.origin, 12);
	printf("door %s open (frac %.2f); player at its closed spot (%.0f %.0f %.0f)\n", name, d->frac, pl.origin[0], pl.origin[1], pl.origin[2]);
	Ent_Input(di, "Close", "", 0);
	int in = 0;
	for (int t = 0; t < 66 * 6; t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		CM_TraceOnly(&tr, pl.origin, pl.origin, mn, mx, MASK_PLAYERSOLID, di);
		in += tr.startsolid;
	}
	printf("after closing: door frac %.2f state %d, player moved (%.0f %.0f %.0f), ticks inside the door %d\n", d->frac, d->moveState, pl.origin[0] - p0[0], pl.origin[1] - p0[1], pl.origin[2] - p0[2], in);
	return in > 3;
}
