/* Host test: the arrival train on Trainstation 1. During the ride the player must be able to walk around the
 * car; once it stops the door opens, the passengers walk out onto the platform (not through the walls), and
 * the player can walk out of the door too. Prints positions and counts. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)l; printf("%6.1fs changelevel %s\n", Ent_Now(), m); }
static Player pl;
static u32 dummyCount;
static Entity *named(const char *n)
{
	u32 c; Entity *e = Ent_List(&c);
	for (u32 i = 1; i < c; i++) if (e[i].name && !strcasecmp(e[i].name, n)) return &e[i];
	return NULL;
}
static void go(f32 secs, f32 fwd, f32 yaw)
{
	UserCmd c = {0};
	c.forwardmove = fwd;
	c.yaw = yaw;
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
}
static int in_solid(const f32 o[3])
{
	Trace tr; f32 mn[3] = {-13, -13, 0}, mx[3] = {13, 13, 64};
	CM_SetSkipBoxes(1);
	CM_BoxTrace(&tr, o, o, mn, mx, CONTENTS_SOLID);
	CM_SetSkipBoxes(0);
	return tr.startsolid;
}
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Entity *train = named("intro_train_2"), *door = named("train_door_1");
	Entity *E2 = named("train_door_2");
	Entity *c1 = named("citizen_train_1"), *c2 = named("citizen_train_2");
	for (int k = 0; k < 40; k++) {
		go(2, 0, 0);
		printf("  ride %4.0fs train (%.0f %.0f) yaw %7.2f | player rel (%.0f %.0f %.0f) ground %d (%s) frozen %d\n", Ent_Now(), train->origin[0], train->origin[1], train->angles[1], pl.origin[0] - train->origin[0], pl.origin[1] - train->origin[1], pl.origin[2] - train->origin[2], pl.groundEnt, pl.groundEnt > 0 ? Ent_List(&dummyCount)[pl.groundEnt].classname : "-", pl.frozen);
	}
	printf("%6.1fs control back; player (%.0f %.0f %.0f), train at (%.0f %.0f)\n", Ent_Now(), pl.origin[0], pl.origin[1], pl.origin[2], train->origin[0], train->origin[1]);
	/* walk around inside the moving car: relative to the train, the player should get somewhere */
	f32 rel0[2] = {pl.origin[0] - train->origin[0], pl.origin[1] - train->origin[1]};
	go(1.5f, 1, 0);
	go(1.5f, 1, 180);
	f32 rel1[2] = {pl.origin[0] - train->origin[0], pl.origin[1] - train->origin[1]};
	f32 walked = fabsf(rel1[0] - rel0[0]);
	go(1.5f, 1, 180);
	f32 rel2 = pl.origin[0] - train->origin[0];
	printf("%6.1fs walking in the moving car: moved %.0f then %.0f units along it (stuck if both ~0)\n", Ent_Now(), walked, fabsf(rel2 - rel1[0]));
	/* wait for the stop and the door */
	f32 lastT[3]; memcpy(lastT, train->origin, 12);
	int stopped = 0;
	for (int k = 0; k < 60 && !stopped; k++) {
		go(1, 0, 0);
		f32 d = fabsf(train->origin[0] - lastT[0]) + fabsf(train->origin[1] - lastT[1]);
		memcpy(lastT, train->origin, 12);
		if (d < 0.1f && Ent_Now() > 70) stopped = 1;
	}
	go(4, 0, 0);
	for (int k = 0; k < 6; k++) {
		Npc *n1 = Npc_OfEntity((s32)(c1 - Ent_List(&dummyCount)));
		if (n1) {
			printf("   %.1fs c1 at (%.0f %.0f %.0f) moving %d goal (%.0f %.0f %.0f) path %d/%d", Ent_Now(), c1->origin[0], c1->origin[1], c1->origin[2], n1->moving, n1->goal[0], n1->goal[1], n1->goal[2], n1->pathPos, n1->pathLen);
			for (int j = 0; j < n1->pathLen && j < 8; j++) printf(" [%.0f %.0f %.0f]", P.navNodes[n1->path[j]].pos[0], P.navNodes[n1->path[j]].pos[1], P.navNodes[n1->path[j]].pos[2]);
			printf("\n");
		}
		go(1.5f, 0, 0);
	}
	{
		u32 ne; Entity *E = Ent_List(&ne);
		for (u32 i = 1; i < ne; i++)
			if ((E[i].bmodel >= 71 && E[i].bmodel <= 73) || i == 138 || i == 369)
				printf("  %s %-14s bm %d parent %d origin (%.1f %.1f %.1f) angles (%.2f %.2f %.2f) R0 (%.3f %.3f %.3f) R2 (%.3f %.3f %.3f) frac %.2f\n", E[i].classname, E[i].name, E[i].bmodel, E[i].parent, E[i].origin[0], E[i].origin[1], E[i].origin[2],
				       E[i].angles[0], E[i].angles[1], E[i].angles[2], E[i].R[0][0], E[i].R[0][1], E[i].R[0][2], E[i].R[2][0], E[i].R[2][1], E[i].R[2][2], E[i].frac);
	}
	printf("%6.1fs train %s at (%.0f %.0f); door 1 frac %.2f (state %d)\n", Ent_Now(), stopped ? "stopped" : "still moving", train->origin[0], train->origin[1], door->frac, door->moveState);
	/* passengers: out of the car, never inside walls */
	int solid1 = 0, solid2 = 0;
	for (int k = 0; k < 40; k++) {
		go(0.5f, 0, 0);
		if (c1 && !(c1->flags & EF_KILLED) && in_solid(c1->origin)) solid1++;
		if (c2 && !(c2->flags & EF_KILLED) && in_solid(c2->origin)) solid2++;
	}
	if (c1) printf("%6.1fs citizen_train_1 at (%.0f %.0f %.0f) parent %d, in solid %d/40 samples\n", Ent_Now(), c1->origin[0], c1->origin[1], c1->origin[2], c1->parent, solid1);
	if (c2) printf("%6.1fs citizen_train_2 at (%.0f %.0f %.0f) parent %d, in solid %d/40 samples\n", Ent_Now(), c2->origin[0], c2->origin[1], c2->origin[2], c2->parent, solid2);
	{
		Trace tr; f32 pm[3], px[3]; Player_Bounds(&pl, pm, px);
		CM_BoxTrace(&tr, pl.origin, pl.origin, pm, px, MASK_PLAYERSOLID);
		printf("   player at (%.0f %.0f %.0f) startsolid %d ent %d onground %d\n", pl.origin[0], pl.origin[1], pl.origin[2], tr.startsolid, tr.ent, pl.onGround);
		for (int k = 0; k < 4; k++) { f32 a[3]; memcpy(a, pl.origin, 12); go(0.5f, 1, k * 90.0f); printf("   walk yaw %d: moved (%.0f %.0f %.0f)\n", k * 90, pl.origin[0]-a[0], pl.origin[1]-a[1], pl.origin[2]-a[2]); }
	}
	/* the player walks out of the door (south, -y) */
	f32 y0 = pl.origin[1];
	go(1.0f, 1, 0);
	f32 dx = door->origin[0] - pl.origin[0];
	go(fabsf(dx) / 190.0f, 1, dx > 0 ? 0 : 180);
	go(3.0f, 1, -90);
	printf("%6.1fs player walked from y %.0f to y %.0f (door at y %.0f): %s\n", Ent_Now(), y0, pl.origin[1], door->origin[1], pl.origin[1] < door->origin[1] - 40 ? "out of the train" : "still inside");
	return 0;
}
