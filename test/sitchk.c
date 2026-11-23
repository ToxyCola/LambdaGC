#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	/* "busy": what the map's triggers would do as the player walks around: spawn the template makers'
	 * NPCs and activate every act-busy goal */
	if (argc > 3 && !strcmp(argv[3], "busy")) {
		for (int t = 0; t < 30; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
		u32 ne0; Entity *E0 = Ent_List(&ne0);
		for (u32 i = 1; i < ne0; i++)
			if (!strcmp(E0[i].classname, "npc_template_maker")) Ent_Input((s32)i, "Spawn", "", 0);
		for (u32 i = 1; i < ne0; i++)
			if (!strcmp(E0[i].classname, "ai_goal_actbusy")) Ent_Input((s32)i, "Activate", "", 0);
	}
	int ticks = argc > 2 ? atoi(argv[2]) : 200;
	for (int t = 0; t < ticks; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	u32 nn, ne; Npc *L = Npc_List(&nn); Entity *E = Ent_List(&ne);
	for (u32 i = 0; i < nn; i++) {
		Npc *n = &L[i];
		const char *sn = n->seq >= 0 ? P.entStrings + P.npcSeqs[n->skel->firstSeq + n->seq].name : "-";
		const f32 *o = E[n->ent].origin;
		f32 a[3] = {o[0], o[1], o[2] + 8}, b[3] = {o[0], o[1], o[2] - 200};
		Trace tr; CM_TraceWorld(&tr, a, b, CONTENTS_SOLID);
		printf("%-18s %-28s seq %-34s locked %d script %d at (%.0f %.0f %.1f) floor %.1f%s\n", E[n->ent].classname, E[n->ent].name ? E[n->ent].name : "-", sn, n->locked, n->script,
		       o[0], o[1], o[2], tr.endpos[2], o[2] - tr.endpos[2] > 4 ? "  <-- ABOVE FLOOR" : "");
	}
	return 0;
}
