/* Movers push what they run into, like Source's pushers: the arrival train on Trainstation 1 drives into
 * a player standing on the track (he must be shoved along, never end up inside the train), and an NPC
 * likewise. Prints what happens. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static Entity *named(const char *n) { u32 ne; Entity *E = Ent_List(&ne); for (u32 i = 1; i < ne; i++) if (E[i].name && !strcmp(E[i].name, n)) return &E[i]; return NULL; }
static int inside(s32 ent, const f32 o[3], const f32 mn[3], const f32 mx[3]) { Trace tr; CM_TraceOnly(&tr, o, o, mn, mx, MASK_PLAYERSOLID, ent); return tr.startsolid; }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	Entity *train = named("intro_train_2");
	s32 ti = (s32)(train - Ent_List(&(u32){0}));
	/* wait for the ride to start */
	while (Ent_Now() < 63) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	/* the player stands on the track a car's length ahead */
	f32 mn[3], mx[3]; Player_Bounds(&pl, mn, mx);
	f32 bmn[3], bmx[3];
	const PackBModel *bm = &P.bmodels[train->bmodel];
	for (int k = 0; k < 3; k++) { bmn[k] = bm->mins[k]; bmx[k] = bm->maxs[k]; }
	f32 ahead = train->origin[0] + bmx[0] + 200;
	f32 spot[3] = {ahead, train->origin[1], train->origin[2] + bmn[2] + 40};
	Trace tr; f32 down[3] = {spot[0], spot[1], spot[2] - 400};
	CM_BoxTrace(&tr, spot, down, mn, mx, MASK_PLAYERSOLID);
	memcpy(pl.origin, tr.endpos, 12);
	pl.frozen = 0;
	printf("player on the track at (%.0f %.0f %.0f), train front at x %.0f\n", pl.origin[0], pl.origin[1], pl.origin[2], train->origin[0] + bmx[0]);
	int inTrain = 0, pushed = 0;
	f32 x0 = pl.origin[0];
	for (int t = 0; t < 66 * 6; t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		if (inside(ti, pl.origin, mn, mx)) inTrain++;
		if (t % 66 == 0)
			printf("  %.1fs player x %.0f (train front %.0f, %s)\n", Ent_Now(), pl.origin[0], train->origin[0] + bmx[0], pl.groundEnt == ti ? "riding" : "not riding");
	}
	pushed = pl.origin[0] > x0 + 50;
	printf("pushed %s (%.0f units), ticks inside the train: %d\n", pushed ? "yes" : "NO", pl.origin[0] - x0, inTrain);
	return inTrain > 3;
}
