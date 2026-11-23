#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1; CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); pl.noclip = 1; EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	tick(1);
	Entity *E = Ent_List(&cnt);
	for (u32 i = 1; i < cnt; i++) if (E[i].name && !strcmp(E[i].name, argv[2])) {
		Entity *e = &E[i];
		printf("moveLen %.1f speed %.1f wait %.1f\n", e->moveLen, e->speed, e->wait);
		Ent_Input(i, "Press", "", 0);
		for (int k = 0; k < 20; k++) { tick(0.5f); printf("%4.1f state %d frac %.2f locked %d\n", Ent_Now(), e->moveState, e->frac, e->locked); }
		printf("press again\n");
		Ent_Input(i, "Press", "", 0);
		for (int k = 0; k < 10; k++) { tick(0.5f); printf("%4.1f state %d frac %.2f locked %d\n", Ent_Now(), e->moveState, e->frac, e->locked); }
	}
	return 0;
}
