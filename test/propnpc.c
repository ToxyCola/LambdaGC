/* Props vs NPCs: throw each of a few physics props at the nearest armed NPC and see whether it stops
 * against the body (rather than passing through) and hurts it. usage: propnpc <map.hgm> */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	Ent_Spawn(&P, &pl, 1, &cb);
	for (int t = 0; t < 66; t++) Ent_Tick(TICK_INTERVAL);
	u32 nn, ne; Npc *N = Npc_List(&nn); Entity *E = Ent_List(&ne);
	int tries = 0, stopped = 0, hurt = 0, killed = 0;
	for (u32 i = 1; i < ne && tries < 6; i++) {
		if (!Phys_IsBody((s32)i) || Phys_Mass((s32)i) < 5 || Phys_Mass((s32)i) > 60) continue;
		/* nearest live NPC */
		Npc *best = NULL; f32 bd = 1e9f;
		for (u32 k = 0; k < nn; k++) {
			Entity *ne2 = &E[N[k].ent];
			if (ne2->cls != EC_NPC || (ne2->flags & (EF_KILLED | EF_DISABLED)) || !(ne2->flags & EF_SOLID) || N[k].health <= 0) continue;
			f32 dx = ne2->origin[0] - E[i].origin[0], dy = ne2->origin[1] - E[i].origin[1], d = dx * dx + dy * dy;
			if (d < bd) bd = d, best = &N[k];
		}
		if (!best) break;
		Entity *ne2 = &E[best->ent];
		/* put the prop 64 units from the NPC at chest height and throw it at 600 in/s */
		f32 h0 = best->health;
		f32 start[3], dirx = 0, diry = 0; int ok = 0;
		for (int a = 0; a < 8 && !ok; a++) {
			dirx = cosf(a * 0.785f); diry = sinf(a * 0.785f);
			f32 s0[3] = {ne2->origin[0] - dirx * 72, ne2->origin[1] - diry * 72, ne2->origin[2] + 40}, t0[3] = {ne2->origin[0] - dirx * 16, ne2->origin[1] - diry * 16, ne2->origin[2] + 40};
			f32 bm[3] = {-14, -14, -14}, bx[3] = {14, 14, 14}; Trace tr; CM_SetSkipBoxes(1); CM_SetIgnoreEnt((s32)i);
			CM_BoxTrace(&tr, s0, t0, bm, bx, MASK_PLAYERSOLID); CM_SetSkipBoxes(0); CM_SetIgnoreEnt(-1);
			if (!tr.startsolid && tr.fraction == 1) { ok = 1; memcpy(start, s0, 12); }
		}
		if (!ok) continue;
		memcpy(E[i].origin, start, 12); Phys_Wake((s32)i);

		Phys_Teleport((s32)i, start);
		f32 m = Phys_Mass((s32)i), imp[3] = {m * 600 * dirx, m * 600 * diry, 0};
		Phys_Impulse((s32)i, start, imp);
		f32 minx = 1e9f;
		for (int t = 0; t < 33; t++) { Ent_Tick(TICK_INTERVAL); }
		f32 endx = (E[i].origin[0] - ne2->origin[0]) * dirx + (E[i].origin[1] - ne2->origin[1]) * diry;
		if (fabsf(endx + 72) < 1) { printf("prop %.0f kg didn't move (motion disabled)\n", m); continue; }
		tries++;
		int st = endx < 0; stopped += st;
		hurt += best->health < h0; killed += (ne2->flags & EF_KILLED) || best->health <= 0;
		printf("prop %.0f kg at an NPC (health %.0f): ends %.0f units %s its centre; health now %.0f\n", m, h0, fabsf(endx), st ? "short of" : "past", best->health);
	}
	printf("%d throws: %d stopped by the body, %d hurt it, %d killed it\n", tries, stopped, hurt, killed);
	return 0;
}
