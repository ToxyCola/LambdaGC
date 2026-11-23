/* Host test: every func_tracktrain on a map over the first N seconds (no player input): how far it went,
 * where it ended up, its speed. For comparing engine versions. usage: trainsurvey secs map.hgm... */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
int main(int argc, char **argv)
{
	f32 secs = (f32)atof(argv[1]);
	for (int a = 2; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); pl.noclip = 1; EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		Entity *E = Ent_List(&cnt);
		static f32 dist[4096], last[4096][3];
		for (u32 i = 1; i < cnt && i < 4096; i++) { dist[i] = 0; memcpy(last[i], E[i].origin, 12); }
		UserCmd c = {0};
		for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
			Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); E = Ent_List(&cnt);
			for (u32 i = 1; i < cnt && i < 4096; i++) if (E[i].cls == EC_TRACKTRAIN) {
				f32 d = 0; for (int k = 0; k < 3; k++) d += (E[i].origin[k] - last[i][k]) * (E[i].origin[k] - last[i][k]);
				dist[i] += sqrtf(d); memcpy(last[i], E[i].origin, 12);
			}
		}
		for (u32 i = 1; i < cnt && i < 4096; i++) if (E[i].cls == EC_TRACKTRAIN)
			printf("%s %-32s went %8.0f  at (%6.0f %6.0f %6.0f) speed %6.1f node %s\n", strrchr(argv[a], '/') + 1, E[i].name ? E[i].name : "-", dist[i], E[i].origin[0], E[i].origin[1], E[i].origin[2], E[i].trainSpeed, E[i].pathNode > 0 && E[E[i].pathNode].name ? E[E[i].pathNode].name : "-");
	}
	return 0;
}
