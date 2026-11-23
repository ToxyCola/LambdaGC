/* Host test: Trainstation 1's train passengers (CNPC_Citizen on a func_tracktrain): citizen_train_2 sits,
 * citizen_train_1 stands, neither walks while riding, they stay put relative to the car when the player
 * runs into them, and they get up with the _Exit sequences once the train reaches the station. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)l; (void)m; }
static Player pl;
static Entity *named(const char *n)
{
	u32 c; Entity *e = Ent_List(&c);
	for (u32 i = 1; i < c; i++) if (e[i].name && !strcasecmp(e[i].name, n)) return &e[i];
	return NULL;
}
static const char *seqname(const Npc *n)
{
	static const char *names[] = {"d1_t01_TrainRide_Sit_Idle", "d1_t01_TrainRide_Stand", "d1_t01_TrainRide_Sit_Exit",
	                              "d1_t01_TrainRide_Stand_Exit"};
	for (int k = 0; k < 4; k++) if (Npc_FindSeq(n, names[k]) == n->seq && n->seq >= 0) return names[k];
	if (n->seq == n->skel->seqWalk) return "walk";
	if (n->seq == n->skel->seqRun) return "run";
	if (n->seq == n->skel->seqIdle) return "idle";
	return "other";
}
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Entity *train = named("intro_train_2"), *c[2] = {named("citizen_train_1"), named("citizen_train_2")};
	Npc *n[2] = {Npc_OfEntity((s32)(c[0] - Ent_List(&(u32){0}))), Npc_OfEntity((s32)(c[1] - Ent_List(&(u32){0})))};
	f32 rel0[2][3];
	int walkTicks = 0, shoved = 0;
	UserCmd idle = {0};
	for (int t = 0; t < 66 * 140; t++) {
		/* once the player is on the train, run him into citizen_train_1 now and then */
		UserCmd cmd = idle;
		if (pl.groundEnt > 0 && Ent_List(&(u32){0})[pl.groundEnt].classname && !strcmp(Ent_List(&(u32){0})[pl.groundEnt].classname, "func_tracktrain") && (t / 66) % 6 < 2) {
			f32 dx = c[0]->origin[0] - pl.origin[0], dy = c[0]->origin[1] - pl.origin[1];
			cmd.yaw = atan2f(dy, dx) * 180 / M_PI;
			cmd.forwardmove = 1;
			shoved++;
		}
		Player_Tick(&pl, &cmd);
		Ent_Tick(TICK_INTERVAL);
		for (int k = 0; k < 2; k++)
			if (n[k]->train == 1 && (n[k]->seq == n[k]->skel->seqWalk || n[k]->moving)) walkTicks++;
		if (t == 66) for (int k = 0; k < 2; k++) for (int a = 0; a < 3; a++) rel0[k][a] = c[k]->origin[a] - train->origin[a];
		if (t % (66 * 10) == 0 || (t > 66 * 60 && t % 66 == 0 && t < 66 * 75))
			printf("%6.1fs train (%.0f %.0f) | c1 %-28s state %d rel (%.0f %.0f %.0f) | c2 %-28s state %d rel (%.0f %.0f %.0f)\n", Ent_Now(),
			       train->origin[0], train->origin[1], seqname(n[0]), n[0]->train, c[0]->origin[0] - train->origin[0],
			       c[0]->origin[1] - train->origin[1], c[0]->origin[2] - train->origin[2], seqname(n[1]), n[1]->train,
			       c[1]->origin[0] - train->origin[0], c[1]->origin[1] - train->origin[1], c[1]->origin[2] - train->origin[2]);
	}
	printf("riding: %d ticks walking (want 0); player pushed into citizen_train_1 for %d ticks\n", walkTicks, shoved);
	return 0;
}
