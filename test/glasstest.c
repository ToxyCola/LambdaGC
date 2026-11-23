#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Entity *ents = Ent_List(&cnt); int n = 0; int g=0; for (u32 i = 1; i < cnt; i++) if (strstr(ents[i].classname, "breakable")) g++; printf("%u ents, %d breakables\n", cnt, g);
	for (u32 i = 1; i < cnt && n < 4; i++) {
		if (strcmp(ents[i].classname, "func_breakable_surf")) continue; printf("found %u flags %x\n", i, ents[i].flags);
		f32 mn[3], mx[3]; Ent_Bounds((s32)i, mn, mx);
		f32 c[3] = {(mn[0]+mx[0])/2, (mn[1]+mx[1])/2, (mn[2]+mx[2])/2};
		int ax = (mx[0]-mn[0]) < (mx[1]-mn[1]) ? 0 : 1;
		f32 a[3] = {c[0], c[1], c[2]}; a[ax] -= 64;
		Trace tr; f32 z[3] = {0,0,0}; CM_BoxTrace(&tr, a, c, z, z, MASK_SHOT);
		f32 d[3] = {0,0,0}; d[ax] = 1;
		Ent_Damage((s32)i, 10, d, c);
		printf("surf %u bmodel %d solid %x: trace frac %.2f ent %d contents? killed after damage %d\n", i, ents[i].bmodel, ents[i].flags & EF_SOLID, tr.fraction, tr.ent, (ents[i].flags & EF_KILLED) != 0);
		n++;
	}
	return 0;
}
