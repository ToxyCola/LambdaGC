/* Host test: hostile NPCs fight. On a map with Combine/zombies: for each hostile NPC with a clear view, put the
 * player 300 units in front of it and count hits over 10 s; also check the Combine hold fire while the
 * "gordon_precriminal" global is on. Counts only. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static f32 taken; static int hits;
static void hurt(f32 d, u32 t, const f32 *f) { (void)t; (void)f; taken += d; hits++; }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl, NULL, NULL, hurt, NULL};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	u32 nn, ne; Npc *L = Npc_List(&nn); Entity *E = Ent_List(&ne);
	int tried = 0, attacked = 0, armed = 0;
	UserCmd idle = {0};
	for (u32 i = 0; i < nn && tried < 6; i++) {
		Npc *n = &L[i];
		if (!n->weapon || n->dead || (E[n->ent].flags & (EF_KILLED | EF_DISABLED))) continue;
		armed++;
		f32 yaw = n->yaw * (f32)M_PI / 180;
		f32 at[3] = {E[n->ent].origin[0] + cosf(yaw) * 300, E[n->ent].origin[1] + sinf(yaw) * 300, E[n->ent].origin[2] + 8};
		if (!Player_Spawn(&pl, at, 0, 0)) continue;
		f32 eye[3] = {E[n->ent].origin[0], E[n->ent].origin[1], E[n->ent].origin[2] + 60};
		if (!Ent_PlayerVisible(eye, NULL)) continue;
		tried++;
		taken = 0; hits = 0;
		for (int t = 0; t < (int)(10 / TICK_INTERVAL); t++) { Player_Tick(&pl, &idle); Ent_Tick(TICK_INTERVAL); }
		printf("  %-20s weapon %u: %d hits, %.0f damage in 10 s, now %.0f units away\n", E[n->ent].classname, n->weapon, hits, taken,
		       sqrtf((pl.origin[0]-E[n->ent].origin[0])*(pl.origin[0]-E[n->ent].origin[0])+(pl.origin[1]-E[n->ent].origin[1])*(pl.origin[1]-E[n->ent].origin[1])));
		if (hits) attacked++;
	}
	printf("%s: %d armed/hostile-capable NPCs, %d tested in view, %d attacked; precriminal %d\n", argv[1], armed, tried, attacked, Ent_GlobalState("gordon_precriminal"));
	return 0;
}
