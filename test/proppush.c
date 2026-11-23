/* Host test: NPCs walking into physics props. For light props resting on the ground: take an NPC, put it
 * 80 units to one side of the prop and send it to 120 units the other side, straight through. It should get
 * there (shoving the prop) without climbing on top of it. Heavy props should block (no walking through). */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	for (int t = 0; t < 66 * 3; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	pl.origin[2] -= 20000; /* the player out of the way */
	u32 nn, ne; Npc *L = Npc_List(&nn); Entity *E = Ent_List(&ne);
	Npc *walker = NULL;
	for (u32 i = 0; i < nn && !walker; i++)
		if (!L[i].prop && !L[i].dead && !(E[L[i].ent].flags & (EF_DISABLED | EF_KILLED)) && E[L[i].ent].parent <= 0 && !strcmp(E[L[i].ent].classname, "npc_citizen")) walker = &L[i];
	if (!walker) for (u32 i = 0; i < nn && !walker; i++) if (!L[i].prop && !L[i].dead && !strncmp(E[L[i].ent].classname, "npc_metropolice", 15)) walker = &L[i];
	if (!walker) { printf("%s: no walker\n", argv[1]); return 0; }
	walker->script = -1; walker->locked = 0; walker->followEnt = -1;
	int tried = 0, arrived = 0, climbed = 0, heavyTried = 0, heavyBlocked = 0;
	for (u32 i = 1; i < ne && tried + heavyTried < 16; i++) {
		if (!Phys_IsBody((s32)i) || (E[i].flags & (EF_KILLED | EF_DISABLED))) continue;
		f32 m = Phys_Mass((s32)i);
		f32 *o = E[i].origin;
		/* a clear lane through it: both ends on the ground, open floor around */
		f32 a[3] = {o[0] - 80, o[1], o[2] + 8}, b[3] = {o[0] + 120, o[1], o[2] + 8};
		Trace tr; f32 mn[3] = {-13, -13, 0}, mx[3] = {13, 13, 72};
		CM_SetIgnoreEnt((s32)i); CM_SetSkipBoxes(1);
		CM_BoxTrace(&tr, a, b, mn, mx, MASK_NPCSOLID);
		f32 dn[3] = {a[0], a[1], a[2] - 64}; Trace t2; CM_BoxTrace(&t2, a, dn, mn, mx, MASK_NPCSOLID);
		CM_SetIgnoreEnt(-1); CM_SetSkipBoxes(0);
		if (tr.startsolid || tr.fraction < 1 || t2.fraction >= 1 || t2.startsolid) continue;
		f32 start[3] = {a[0], a[1], t2.endpos[2]};
		Npc_Teleport(walker, start, 0);
		f32 z0 = start[2], zmax = z0;
		Npc_MoveTo(walker, b, 0, 4);
		for (int t = 0; t < 66 * 5 && walker->moving; t++) {
			Ent_Tick(TICK_INTERVAL);
			if (E[walker->ent].origin[2] > zmax) zmax = E[walker->ent].origin[2];
		}
		f32 dx = E[walker->ent].origin[0] - b[0];
		int ok = fabsf(dx) < 24;
		if (m <= 250) { tried++; arrived += ok; climbed += zmax > z0 + 18; }
		else { heavyTried++; heavyBlocked += !ok || walker->stuckTime > 0; }
		if (argc > 2) printf("  prop %u mass %.0f: %s, rose %.0f; npc %s ends (%.0f %.0f) want (%.0f %.0f) moving %d path %d/%d locked %d script %d\n", i, m, ok ? "got through" : "blocked", zmax - z0, E[walker->ent].name ? E[walker->ent].name : "-", E[walker->ent].origin[0], E[walker->ent].origin[1], b[0], b[1], walker->moving, walker->pathPos, walker->pathLen, walker->locked, walker->script);
	}
	printf("%s: light props %d, got past %d (climbed on top %d); heavy props %d\n", argv[1], tried, arrived, climbed, heavyTried);
	return 0;
}
