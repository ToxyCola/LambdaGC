/* Host test: valve wheels (momentary_rot_button with "use activates") turn while use is held, reach their
 * end and drive what their Position output feeds (the Ravenholm graveyard gate, the canal gates) */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static void tick(f32 secs, s32 hold) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { if (hold > 0) Ent_UseHeld(hold, 0); Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	int tried = 0, ok = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		tick(1, 0);
		Entity *E = Ent_List(&cnt);
		for (u32 i = 1; i < cnt; i++) {
			if (strcmp(E[i].classname, "momentary_rot_button") || !(E[i].spawnflags & 1024)) continue;
			int wasLocked = E[i].locked;
			E[i].locked = 0;
			f32 f0 = E[i].frac, others0[64]; int no = 0;
			for (u32 j = 1; j < cnt && no < 64; j++) if (!strcmp(E[j].classname, "momentary_rot_button") && j != i) others0[no++] = E[j].frac;
			f32 secs = E[i].moveLen / (E[i].interval > 0 ? E[i].interval : 100) + 1.5f;
			tick(secs, (s32)i);
			f32 f1 = E[i].frac;
			tick(1.0f, 0);
			int moved = 0; no = 0;
			for (u32 j = 1; j < cnt && no < 64; j++) if (!strcmp(E[j].classname, "momentary_rot_button") && j != i) { if (fabsf(E[j].frac - others0[no]) > 0.05f) moved++; no++; }
			tried++;
			int good = fabsf(f1 - f0) > 0.9f;
			ok += good;
			printf("  %s: %s %s%s: turned %.2f -> %.2f in %.1f s held (now %.2f), %d driven wheels/doors moved\n", argv[a], E[i].name ? E[i].name : "-", wasLocked ? "(locked) " : "", good ? "" : " NOT TURNED", f0, f1, secs, E[i].frac, moved);
		}
	}
	printf("%d of %d wheels turned end to end: %s\n", ok, tried, ok == tried && tried ? "wheeltest: ok" : "FAILED");
	return ok != tried;
}
