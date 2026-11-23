/* Host test: the chairs in Barney's office on Trainstation 1 (prop_physics) stay in the room when woken
 * and bumped, instead of falling through the floor or flying off. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	u32 n; Entity *E = Ent_List(&n);
	s32 ch[4]; int nc = 0;
	for (u32 i = 1; i < n && nc < 4; i++) { const char *m = Ent_KV(i, "model"); if (m && strstr(m, "chair") && !strcmp(E[i].classname, "prop_physics")) ch[nc++] = i; }
	for (int k = 0; k < nc; k++) {
		f32 o0[3]; memcpy(o0, E[ch[k]].origin, 12);
		Phys_Wake(ch[k]);
		f32 imp[3] = {200, 100, 50};
		Phys_Impulse(ch[k], E[ch[k]].origin, imp);
		for (int t = 0; t < 66 * 4; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
		printf("%s: from (%.0f %.0f %.0f) to (%.0f %.0f %.0f) flags %x\n", Ent_KV(ch[k], "model"), o0[0], o0[1], o0[2], E[ch[k]].origin[0], E[ch[k]].origin[1], E[ch[k]].origin[2], E[ch[k]].flags);
	}
	return 0;
}
