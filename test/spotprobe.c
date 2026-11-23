#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	for (int t = 0; t < 10; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	u32 n; Entity *E = Ent_List(&n);
	for (u32 i = 1; i < n; i++) if (!strcmp(E[i].classname, "point_spotlight"))
		printf("(%.0f %.0f %.0f) angles %.0f %.0f %.0f dir %.2f %.2f %.2f len %.0f w %.0f on %d tex2 %d parent %d\n", E[i].origin[0], E[i].origin[1], E[i].origin[2], E[i].angles[0], E[i].angles[1], E[i].angles[2], E[i].R[0][0], E[i].R[1][0], E[i].R[2][0], E[i].beamLen, E[i].beamWidth, E[i].enabled, E[i].tex2, E[i].parent);
	return 0;
}
