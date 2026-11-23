/* Train passengers' hands: for each npc_citizen parented to the train, after a few seconds of the ride,
 * its height against where the map put it, and how far its right hand is from anything solid (the
 * grab rail) in 14 directions. Numbers only. usage: railhand <d1_trainstation_01.hgm> [secs] */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)l; (void)m; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	Ent_Spawn(&P, &pl, 1, &cb);
	int secs = argc > 2 ? atoi(argv[2]) : 5;
	for (int t = 0; t < 66 * secs; t++) { UserCmd idle = {0}; Player_Tick(&pl, &idle); Ent_Tick(TICK_INTERVAL); }
	u32 ne; Entity *E = Ent_List(&ne);
	for (u32 i = 1; i < ne; i++) {
		if (E[i].cls != EC_NPC || E[i].parent <= 0) continue;
		const char *tn = Ent_KV((s32)i, "targetname"), *o = Ent_KV((s32)i, "origin");
		Npc *n = Npc_OfEntity((s32)i);
		if (!n) continue;
		f32 ox = 0, oy = 0, oz = 0; if (o) sscanf(o, "%f %f %f", &ox, &oy, &oz);
		Entity *pa = &E[E[i].parent];
		f32 hp[3], hR[3][3];
		int have = Npc_Attachment(n, "_rhand", hp, hR);
		if (n->seq == Npc_FindSeq(n, "d1_t01_TrainRide_Stand")) { /* the left hand, where the PC animation holds it */
			f32 l[3] = {13.5f, 16.9f, 62.3f};
			for (int k = 0; k < 3; k++) hp[k] = E[i].origin[k] + E[i].R[k][0] * l[0] + E[i].R[k][1] * l[1] + E[i].R[k][2] * l[2];
		}
		f32 best = 99; int bestDir = -1;
		static const f32 dirs[14][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{.577f,.577f,.577f},{-.577f,.577f,.577f},{.577f,-.577f,.577f},{-.577f,-.577f,.577f},{.577f,.577f,-.577f},{-.577f,.577f,-.577f},{.577f,-.577f,-.577f},{-.577f,-.577f,-.577f}};
		if (have) {
			CM_SetSkipBoxes(1);
			for (int d = 0; d < 14; d++) {
				f32 b[3] = {hp[0] + dirs[d][0] * 24, hp[1] + dirs[d][1] * 24, hp[2] + dirs[d][2] * 24}; Trace tr;
				CM_BoxTrace(&tr, hp, b, (f32[3]){0,0,0}, (f32[3]){0,0,0}, MASK_PLAYERSOLID);
				f32 dd = tr.startsolid ? 0 : tr.fraction * 24; if (dd < best) best = dd, bestDir = d;
			}
			CM_SetSkipBoxes(0);
		}
		printf("[stand %d sit %d] ", Npc_FindSeq(n, "d1_t01_TrainRide_Stand"), Npc_FindSeq(n, "d1_t01_TrainRide_Sit_Idle"));
		printf("%s seq %d: feet %.1f above the train origin (map placed it %.1f above at spawn); right hand %s %.1f above feet, nearest solid %.1f units away (dir %d)\n",
		       tn ? tn : "-", n->seq, E[i].origin[2] - pa->origin[2], oz - atof(Ent_KV(E[i].parent, "origin") ? strrchr(Ent_KV(E[i].parent, "origin"), ' ') + 1 : "0"),
		       have ? "at" : "MISSING", hp[2] - E[i].origin[2], best, bestDir);
	}
	return 0;
}
