/* Host test: every drawn NPC / prop actor batch has sane bone matrices and vertex matrix slots. */
#include "ent.h"
#include "collide.h"
#include "npc.h"
#define main sim_main
#include "sim.c"
#undef main
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static f32 pose[256][3][4];
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	CM_Init(&P);
	static Player pl;
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	int badSlot = 0, badMtx = 0, checked = 0, badActors = 0;
	for (int t = 0; t < 66 * 20; t++) {
		Ent_Tick(1.0f / 66);
		if (t % 66) continue;
		u32 nn;
		Npc *list = Npc_List(&nn);
		for (u32 i = 0; i < nn; i++) {
			const Npc *n = &list[i];
			const PackNpcModel *m = &P.npcModels[n->model];
			Npc_Pose(n, pose);
			int bad = 0;
			for (u32 b = 0; b < m->numBatches; b++) {
				const PackNpcBatch *bt = &P.npcBatches[m->firstBatch + b];
				for (u32 k = 0; k < bt->numBones && k < 10; k++) {
					u32 bone = bt->bones[k] < n->skel->numBones ? bt->bones[k] : 0;
					f32 (*M)[4] = pose[bone];
					f32 det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
					          M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
					int fin = 1;
					for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) fin &= isfinite(M[r][c]) && fabsf(M[r][c]) < 1e5f;
					f32 tl = sqrtf(M[0][3] * M[0][3] + M[1][3] * M[1][3] + M[2][3] * M[2][3]);
					f32 lim = m->radius * 2 + m->height * 2 + 512;
					if (!fin || fabsf(det) < 0.01f || fabsf(det) > 4 || tl > lim) {
						badMtx++; bad = 1;
						if (badMtx < 6) printf("  model %u (%s) bone %u: det %.3f |t| %.0f limit %.0f\n", n->model,
						                       Ent_List(NULL) ? "" : "", bone, det, tl, lim);
					}
					checked++;
				}
				for (u32 v = 0; v < bt->numVerts; v++)
					if (P.npcVerts[bt->firstVert + v].mtx >= bt->numBones * 3 || bt->numBones > 10) { badSlot++; bad = 1; }
			}
			badActors += bad;
		}
	}
	u32 nn;
	Npc_List(&nn);
	printf("%s: %u npcs/actors, %d bone matrices checked, %d degenerate, %d vertices in unloaded slots, %d bad samples\n",
	       argv[1], nn, checked, badMtx, badSlot, badActors);
	return 0;
}
