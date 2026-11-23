/* Host test: the big flyers (npc_helicopter, npc_combinegunship, npc_combinedropship) fly their path_tracks
 * like CAI_TrackPather: over the first N seconds of a map (no player input), how far each flew, which
 * track points it passed. usage: tracktest secs map.hgm... */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
int main(int argc, char **argv)
{
	f32 secs = (f32)atof(argv[1]);
	int total = 0, moved = 0;
	for (int a = 2; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); pl.noclip = 1; EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		Entity *E = Ent_List(&cnt);
		static s32 goal[8192];
		if (getenv("TRACK_SEND")) {
			/* send each one to the far end of the path nearest it */
			for (u32 i = 1; i < cnt && i < 8192; i++) {
				goal[i] = -1;
				const char *cn = E[i].classname;
				if (strcmp(cn, "npc_helicopter") && strcmp(cn, "npc_combinegunship") && strcmp(cn, "npc_combinedropship")) continue;
				s32 best = -1; f32 bd = 1e30f;
				for (u32 j = 1; j < cnt; j++) if (!strcmp(E[j].classname, "path_track") && E[j].name) {
					f32 d = 0; for (int k = 0; k < 3; k++) d += (E[j].origin[k] - E[i].origin[k]) * (E[j].origin[k] - E[i].origin[k]);
					if (d < bd) { bd = d; best = (s32)j; }
				}
				if (best < 0) continue;
				s32 t = best; for (int g = 0; g < 40; g++) { s32 nx = Ent_PathNext(t, 0); if (nx <= 0 || nx == best) break; t = nx; }
				goal[i] = t;
				E[i].flags &= ~EF_DISABLED;
				Ent_Input((s32)i, "FlyToSpecificTrackViaPath", E[t].name, 0);
				printf("  %s -> %s (from %s, %.0f away)\n", E[i].name ? E[i].name : "-", E[t].name, E[best].name, sqrtf(bd));
			}
		}
		static f32 dist[8192], last[8192][3], start[8192][3];
		for (u32 i = 1; i < cnt && i < 8192; i++) { dist[i] = 0; memcpy(last[i], E[i].origin, 12); memcpy(start[i], E[i].origin, 12); }
		UserCmd c = {0};
		for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
			Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); E = Ent_List(&cnt);
			for (u32 i = 1; i < cnt && i < 8192; i++) {
				f32 d = 0; for (int k = 0; k < 3; k++) d += (E[i].origin[k] - last[i][k]) * (E[i].origin[k] - last[i][k]);
				dist[i] += sqrtf(d); memcpy(last[i], E[i].origin, 12);
			}
		}
		const char *mn = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
		for (u32 i = 1; i < cnt && i < 8192; i++) {
			const char *cn = E[i].classname;
			if (strcmp(cn, "npc_helicopter") && strcmp(cn, "npc_combinegunship") && strcmp(cn, "npc_combinedropship")) continue;
			if (E[i].flags & EF_TEMPLATE) continue;
			total++; if (dist[i] > 100) moved++;
			if (getenv("TRACK_SEND") && goal[i] > 0) {
				f32 d = 0; for (int k = 0; k < 3; k++) d += (E[i].origin[k] - E[goal[i]].origin[k]) * (E[i].origin[k] - E[goal[i]].origin[k]);
				printf("    ended %.0f from its goal %s\n", sqrtf(d), E[goal[i]].name);
			}
			printf("%s %-22s %-28s %s flew %7.0f from (%6.0f %6.0f %6.0f) to (%6.0f %6.0f %6.0f)\n", mn, cn, E[i].name ? E[i].name : "-", (E[i].flags & EF_KILLED) ? "killed" : (E[i].flags & EF_DISABLED) ? "off   " : "      ", dist[i], start[i][0], start[i][1], start[i][2], E[i].origin[0], E[i].origin[1], E[i].origin[2]);
		}
	}
	printf("%d big flyers, %d flew\n", total, moved);
	return 0;
}
