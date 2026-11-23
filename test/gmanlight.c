/* the G-Man in the intro is lit by light_dynamic lights riding small tracktrains around his head: his
 * lighting cube (average brightness) over the first seconds, with and without them */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "light.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Light_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	u32 n; Entity *E = Ent_List(&n); s32 g = -1;
	for (u32 i = 1; i < n; i++) if (E[i].name && !strcmp(E[i].name, "gman")) g = (s32)i;
	if (g < 0) return 1;
	for (int t = 0; t < 66 * 30; t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		if (t % (66 * 3) == 0) {
			f32 head[3] = {E[g].origin[0], E[g].origin[1], E[g].origin[2] + 64};
			u8 lit[18], base[18];
			int ok = Light_EntityCube(&P, g, head, lit);
			memcpy(base, lit, 18);
			int sl = 0, mx = 0; for (int k = 0; k < 18; k++) { sl += lit[k]; if (lit[k] > mx) mx = lit[k]; }
			printf("%5.1fs gman cube ok %d: average %d, brightest face %d (u8, x2 on screen)\n", Ent_Now(), ok, sl / 18, mx);
		}
	}
	return 0;
}
