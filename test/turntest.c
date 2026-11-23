/* NPCs through turnstiles: for each upright-hinged func_physbox, put a citizen on one side of it and
 * send it to the other, through the arms (several lanes), and see whether it gets there.
 * usage: turntest <map.hgm> */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	int tried = 0, crossed = 0;
	for (int lane = 0; lane < 16; lane++) {
		Ent_Free();
		if (load(&P, argv[1])) return 1;
		CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, 0);
		EntCallbacks cb = {cl};
		Ent_Spawn(&P, &pl, 1, &cb);
		u32 n, nn; Entity *E = Ent_List(&n); Npc *L = Npc_List(&nn);
		Npc *cit = NULL;
		for (u32 k = 0; k < nn && !cit; k++) if (!L[k].dead && L[k].weapon == 0 && E[L[k].ent].cls == EC_NPC && E[L[k].ent].parent <= 0 && !strcmp(E[L[k].ent].classname, "npc_citizen") && !(E[L[k].ent].flags & EF_DISABLED)) cit = &L[k];
		if (!cit) { printf("no npc\n"); return 0; }
		if (lane == 0 && getenv("DBG2")) { const char *keys[] = {"targetname","model","excludednpc","invert_exclusion","Solidity","StartDisabled","spawnflags"}; for (int q = 0; q < 7; q++) printf("  921 %s=%s\n", keys[q], Ent_KV(921, keys[q]) ? Ent_KV(921, keys[q]) : "-"); }
		for (u32 i = 1; i < n; i++) {
			Entity *t = &E[i];
			if (!t->hinged) continue;
			f32 mn[3], mx[3]; Ent_WorldBounds((s32)i, mn, mx);
			f32 *pv = t->hingePivot;
			/* lanes: opposite points 60 out from the post, 22 to the side of it, in 16 directions; only
			 * ones the world leaves clear (the turnstile itself ignored) */
			f32 ang = lane * 0.3927f * 2 + (lane & 1) * 0.3927f, cx = cosf(ang), cy = sinf(ang);
			f32 a[3] = {pv[0] + cx * 60 - cy * 18, pv[1] + cy * 60 + cx * 18, mn[2] + 2}, b[3] = {pv[0] - cx * 60 - cy * 18, pv[1] - cy * 60 + cx * 18, mn[2] + 2};
			Trace tr; f32 hm[3] = {-13, -13, 2}, hx[3] = {13, 13, 64};
			s32 ex[8]; int nx = 0;
			for (u32 q = 1; q < n && nx < 8; q++) { const char *x = Ent_KV((s32)q, "excludednpc"); if (x && !strcasecmp(x, "npc_citizen")) ex[nx++] = (s32)q; }
			CM_SetSkipBoxes(1); CM_SetIgnoreEnt((s32)i); CM_SetIgnoreExtra(ex, nx);
			CM_BoxTrace(&tr, a, b, hm, hx, MASK_NPCSOLID);
			CM_SetIgnoreExtra(NULL, 0); CM_SetIgnoreEnt(-1); CM_SetSkipBoxes(0);
			if (getenv("DBG2")) printf("  lane %d world %s %.2f ent %d %s\n", lane, tr.startsolid ? "solid" : "frac", tr.fraction, tr.ent, tr.ent > 0 ? E[tr.ent].classname : "world");
			if (tr.startsolid || tr.fraction < 1) continue;
			int ax = fabsf(cx) > fabsf(cy) ? 0 : 1; f32 dir = (ax ? cy : cx) > 0 ? 1.f : -1.f;
			Npc_Teleport(cit, a, 0);
			Npc_MoveTo(cit, b, 0, 8);
			for (int k = 0; k < 66 * 8; k++) { UserCmd idle = {0}; Player_Tick(&pl, &idle); Ent_Tick(TICK_INTERVAL); }
			f32 *o = E[cit->ent].origin;
			f32 dd = sqrtf((o[0] - b[0]) * (o[0] - b[0]) + (o[1] - b[1]) * (o[1] - b[1]));
			int through = (o[ax] - pv[ax]) * dir < -20;
			tried++; crossed += through;
			if (getenv("DBG")) printf("  turnstile %u lane %d: %s (%.0f from the goal)\n", i, lane, through ? "through" : "STUCK", dd);
		}
	}
	printf("%d walks at turnstiles, %d got through\n", tried, crossed);
	return 0;
}
