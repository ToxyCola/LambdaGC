#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P);
	static Player pl; Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	u32 nn, ne; Npc *n = Npc_List(&nn); Entity *e = Ent_List(&ne);
	for (int pass = 0; pass < 2; pass++) {
		for (u32 i = 0; i < nn; i++) {
			const Entity *x = &e[n[i].ent];
			if (x->flags & (EF_DISABLED | EF_KILLED)) continue;
			f32 c[3] = {x->origin[0], x->origin[1], x->origin[2] + 36};
			if (CM_PointContents(c) & CONTENTS_SOLID)
				printf("%s %-20s %-22s (%6.0f %6.0f %5.0f) seq %d locked %d script %d moving %d\n", pass ? "end  " : "start",
				       x->classname, x->name ? x->name : "-", x->origin[0], x->origin[1], x->origin[2], n[i].seq, n[i].locked, n[i].script, n[i].moving);
		}
		for (int t = 0; t < 2000; t++) { Ent_Tick(TICK_INTERVAL); }
	}
	return 0;
}
