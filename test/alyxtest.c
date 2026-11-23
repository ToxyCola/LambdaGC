/* Host test: Red Letter Day's Alyx exists, is visible and is the one "alyx" names */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static void report(const char *when)
{
	Entity *ents = Ent_List(&cnt);
	s32 a = Ent_FindActor("alyx");
	printf("%s: \"alyx\" -> %d", when, a);
	for (u32 i = 1; i < cnt; i++)
		if (!strcmp(ents[i].classname, "npc_alyx")) {
			Npc *n = Npc_OfEntity((s32)i);
			printf(" | ent %u name %s flags %x npc %d dead %d pos %.0f %.0f %.0f", i, ents[i].name ? ents[i].name : "-", ents[i].flags, n != NULL, n ? n->dead : -1, ents[i].origin[0], ents[i].origin[1], ents[i].origin[2]);
		}
	printf("\n");
}
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	report("spawn");
	UserCmd c = {0};
	for (int t = 0; t < (int)(atof(argc > 2 ? argv[2] : "30") / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL);
		if (t % 667 == 0) { char b[32]; snprintf(b, sizeof b, "t=%ds", (int)(t * TICK_INTERVAL)); report(b); } }
	report("end");
	return 0;
}
