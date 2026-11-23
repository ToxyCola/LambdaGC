#include "ent.h"
#include "collide.h"
#define main sim_main
#include "sim.c"
#undef main
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P);
	static Player pl; Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	u32 n; Entity *e = Ent_List(&n);
	for (u32 i = 0; i < n; i++) if (!strcmp(e[i].classname, argv[2]))
		printf("%u name %s npcModel %d flags %x _npc %s\n", i, e[i].name ? e[i].name : "-", e[i].npcModel, e[i].flags, Ent_KV((s32)i, "_npc") ? Ent_KV((s32)i, "_npc") : "none");
	return 0;
}
