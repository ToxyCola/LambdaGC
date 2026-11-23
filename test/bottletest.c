/* Host test: glass bottles break when thrown at a wall or dropped off a table height (Source's glass damage
 * table); a wooden crate dropped the same way doesn't */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	int glassTried = 0, glassBroke = 0, otherTried = 0, otherBroke = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, 0); EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		tick(0.5f);
		Entity *E = Ent_List(&cnt);
		for (u32 i = 1; i < cnt; i++) {
			if (!Phys_IsBody((s32)i) || (E[i].flags & (EF_KILLED | EF_DISABLED))) continue;
			const char *mt = Ent_KV((s32)i, "_mat"), *hp = Ent_KV((s32)i, "_hp");
			if (!hp) continue;
			int glass = mt && atoi(mt) == 2;
			int wood = mt && atoi(mt) == 1 && atof(hp) >= 20;
			if (!glass && !wood) continue;
			if ((glass ? glassTried : otherTried) >= 6) continue;
			/* dropped from 48 units over open floor below it */
			f32 p[3] = {Phys_Com((s32)i)[0], Phys_Com((s32)i)[1], Phys_Com((s32)i)[2] + 48};
			Trace tr; f32 z[3] = {0, 0, 0}, mn[3] = {-8, -8, -8}, mx[3] = {8, 8, 8};
			f32 dn[3] = {p[0], p[1], p[2] - 200};
			CM_BoxTrace(&tr, Phys_Com((s32)i), p, mn, mx, MASK_PLAYERSOLID);
			if (tr.fraction < 1) continue;
			CM_BoxTrace(&tr, p, dn, z, z, MASK_PLAYERSOLID);
			if (tr.fraction >= 1 || p[2] - tr.endpos[2] < 48) continue;
			Phys_SetMotion((s32)i, 1);
			Phys_Teleport((s32)i, p);
			tick(1.5f);
			int broke = (E[i].flags & EF_KILLED) != 0;
			if (glass) { glassTried++; glassBroke += broke; } else { otherTried++; otherBroke += broke; }
			if (getenv("V")) printf("  %s %u (%s hp %s): dropped %.0f, broke %d\n", argv[a], i, glass ? "glass" : "wood", hp, p[2] - tr.endpos[2], broke);
		}
	}
	printf("glass: %d of %d broke; wood: %d of %d broke\n", glassBroke, glassTried, otherBroke, otherTried);
	int ok = glassTried && glassBroke == glassTried && otherBroke == 0;
	printf("%s\n", ok ? "bottletest: ok" : "FAILED");
	return !ok;
}
