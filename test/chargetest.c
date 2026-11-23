/* Host test: standing in front of every health / suit charger and pressing use finds the charger */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
int main(int argc, char **argv)
{
	int tried = 0, ok = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, 0); EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		Entity *E = Ent_List(&cnt);
		for (u32 i = 1; i < cnt; i++) {
			const char *c = E[i].classname;
			if (strcmp(c, "item_healthcharger") && strcmp(c, "item_suitcharger")) continue;
			/* in front of it: along its facing (yaw) */
			f32 y = E[i].angles[1] * (f32)M_PI / 180, f[3] = {cosf(y), sinf(y), 0};
			f32 eye[3] = {E[i].origin[0] + f[0] * 40, E[i].origin[1] + f[1] * 40, E[i].origin[2]};
			f32 fwd[3] = {-f[0], -f[1], 0};
			s32 t = Ent_UseTarget(eye, fwd, 80);
			tried++;
			if (t == (s32)i) ok++;
			else printf("  %s: %s %u: use finds %s\n", argv[a], c, i, t > 0 ? E[t].classname : "nothing");
		}
	}
	printf("%d of %d chargers found by use: %s\n", ok, tried, ok == tried && tried ? "chargetest: ok" : "FAILED");
	return ok != tried;
}
