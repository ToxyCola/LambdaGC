/* Barney's office chairs: which BSP cluster their origins and their models' middles fall in, and
 * whether those are visible from a standing eye in the room. Numbers only. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "world.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl};
	Ent_Spawn(&P, &pl, 1, &cb);
	u32 ne; Entity *E = Ent_List(&ne);
	for (u32 i = 1; i < ne; i++) {
		const char *m = Ent_KV((s32)i, "model");
		if (!m || !strstr(m, "chair") || E[i].origin[0] > -3300 || E[i].origin[0] < -3600 || E[i].origin[1] < -600 || E[i].origin[1] > -250) continue;
		const PackModel *pm = &P.models[E[i].mdl];
		f32 mid[3];
		for (int k = 0; k < 3; k++) mid[k] = E[i].origin[k] + E[i].R[k][0] * (pm->mins[0] + pm->maxs[0]) / 2 + E[i].R[k][1] * (pm->mins[1] + pm->maxs[1]) / 2 + E[i].R[k][2] * (pm->mins[2] + pm->maxs[2]) / 2;
		f32 eye[3] = {mid[0] + 40, mid[1], mid[2] + 40};
		int co = World_PointCluster(&P, E[i].origin), cm = World_PointCluster(&P, mid), ce = World_PointCluster(&P, eye);
		printf("%s cls %d flags %x fade %.0f: origin z %.1f cluster %d, middle z %.1f cluster %d, eye cluster %d\n", m, E[i].cls, E[i].flags, E[i].fadeMax, E[i].origin[2], co, mid[2], cm, ce);
	}
	return 0;
}
