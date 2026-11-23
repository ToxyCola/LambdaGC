/* Host test: func_rotating against CFuncRotating: start-on ones spin at maxspeed (accelerating ones get
 * there by spinning up), Stop brings them to rest, Reverse turns them the other way, SetSpeed scales */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	int tried = 0, bad = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		tick(3);
		Entity *E = Ent_List(&cnt);
		for (u32 i = 1; i < cnt; i++) {
			Entity *e = &E[i];
			if (strcmp(e->classname, "func_rotating") || (e->flags & EF_KILLED)) continue;
			const char *ms = Ent_KV(i, "maxspeed"); f32 mx = ms ? fabsf((f32)atof(ms)) : 100; if (mx == 0) mx = 100;
			tried++;
			int on = (e->spawnflags & 1) != 0, err = 0;
			f32 s0 = e->speed;
			if (on && fabsf(fabsf(s0) - mx) > 0.01f) err |= 1;
			Ent_Input(i, "Stop", "", 0); { const char *ff = Ent_KV(i, "fanfriction"); f32 fr = ff ? (f32)atof(ff) / 100 : 0; if (fr == 0) fr = 1; tick(3 + ((e->spawnflags & 16) ? 1.0f / fr + 1 : 0)); }
			if (e->speed != 0) err |= 2;
			Ent_Input(i, "Start", "", 0); tick(0.05f);
			int accel = (e->spawnflags & 16) != 0;
			if (accel && fabsf(e->speed) >= mx) err |= 4;
			const char *ff = Ent_KV(i, "fanfriction"); f32 fr = ff ? (f32)atof(ff) / 100 : 0; if (fr == 0) fr = 1;
			f32 spin = accel ? 0.1f / (0.1f * fr) + 1 : 1; /* (spin down takes twice spin up) */
			tick(spin + 5);
			if (fabsf(fabsf(e->speed) - mx) > 0.01f) err |= 8;
			f32 dir = e->speed;
			Ent_Input(i, "Reverse", "", 0); tick(2 * spin + 5);
			if (!(e->speed * dir < 0)) err |= 16;
			Ent_Input(i, "SetSpeed", "0.5", 0); tick(2 * spin + 5);
			if (fabsf(fabsf(e->speed) - mx * 0.5f) > 0.01f) err |= 32;
			f32 a0 = e->angles[0] + e->angles[1] + e->angles[2]; tick(0.5f);
			if (a0 == e->angles[0] + e->angles[1] + e->angles[2]) err |= 64;
			Ent_Input(i, "StopAtStartPos", "", 0); tick(120 + spin);
			if (e->speed != 0) err |= 128;
			bad += err != 0;
			printf("  %s %-24s sf %5d max %6.1f %s%s: start %.1f%s\n", argv[a] + (strrchr(argv[a], '/') ? strrchr(argv[a], '/') - argv[a] + 1 : 0), e->name ? e->name : "-", e->spawnflags, mx, on ? "on" : "off", accel ? " accel" : "", s0, err ? " FAILED" : "");
			if (err) printf("    err %d\n", err);
		}
	}
	printf("%d func_rotating, %d wrong: %s\n", tried, bad, !bad && tried ? "rottest: ok" : "FAILED");
	return bad != 0;
}
