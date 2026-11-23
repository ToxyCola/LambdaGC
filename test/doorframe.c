/* which way does a prop door's leaf lie in its doorway: along the entity's +X or +Y (Source's hull says +Y)?
 * For each: is the line 4..44 along that axis open, with wall just past both ends (-10 and +58)? */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	int fitX = 0, fitY = 0, n0 = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free();
		if (load(&P, argv[a])) continue;
		CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
		EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		u32 n; Entity *E = Ent_List(&n);
		for (u32 i = 1; i < n; i++) {
			if (E[i].cls != EC_PROP_DOOR) continue;
			f32 yaw = E[i].ang1[1] * 3.14159265f / 180;
			f32 ax[2][2] = {{cosf(yaw), sinf(yaw)}, {-sinf(yaw), cosf(yaw)}};
			int fit[2];
			for (int k = 0; k < 2; k++) {
				int open = 1, walls = 0;
				for (f32 t = 6; t <= 42; t += 6) { f32 p[3] = {E[i].pos1[0] + ax[k][0] * t, E[i].pos1[1] + ax[k][1] * t, E[i].pos1[2]}; if (CM_PointContents(p) & CONTENTS_SOLID) open = 0; }
				f32 p1[3] = {E[i].pos1[0] - ax[k][0] * 10, E[i].pos1[1] - ax[k][1] * 10, E[i].pos1[2]}, p2[3] = {E[i].pos1[0] + ax[k][0] * 58, E[i].pos1[1] + ax[k][1] * 58, E[i].pos1[2]};
				walls = ((CM_PointContents(p1) & CONTENTS_SOLID) != 0) + ((CM_PointContents(p2) & CONTENTS_SOLID) != 0);
				fit[k] = open && walls == 2;
			}
			n0++; fitX += fit[0] && !fit[1]; fitY += fit[1] && !fit[0];
		}
	}
	printf("%d prop doors: leaf along +X fits %d, along +Y fits %d\n", n0, fitX, fitY);
	return 0;
}
