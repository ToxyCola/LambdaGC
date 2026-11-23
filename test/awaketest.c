/* Host test: props settle and go to sleep: after the level has been running a while few are still being
 * simulated (an awake prop costs the GameCube a dozen traces a tick) */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static int in_water(const f32 *c) { f32 p[3] = {c[0], c[1], c[2] - 12}; return (CM_PointContents(c) | CM_PointContents(p)) & MASK_WATER; }
int main(int argc, char **argv)
{
	int bad = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		UserCmd c = {0};
		int at0 = 0, at10 = 0;
		for (int t = 0; t < (int)(10.0f / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); if (t == 1) at0 = Phys_AwakeCount(); }
		u32 n; Entity *E = Ent_List(&n);
		/* (floating in water bobs for good, as on PC) */
		at10 = 0;
		for (u32 i = 1; i < n; i++) if (Phys_IsBody((s32)i) && Phys_IsAwake((s32)i) && !in_water(Phys_Com((s32)i))) at10++;
		printf("  %s: awake at start %d, after 10 s %d\n", argv[a], at0, at10);
		if (at10 > 8) {
			bad++;
			if (getenv("V")) for (u32 i = 1; i < n; i++) if (Phys_IsBody((s32)i) && Phys_IsAwake((s32)i) && !in_water(Phys_Com((s32)i))) printf("    %u %s %s at %.0f %.0f %.0f\n", i, E[i].classname, E[i].name ? E[i].name : "-", E[i].origin[0], E[i].origin[1], E[i].origin[2]);
		}
	}
	printf("%s\n", bad ? "FAILED" : "awaketest: ok");
	return bad != 0;
}
