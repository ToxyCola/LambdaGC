/* Host test: facial morphs stay near the base mesh. */
#include "ent.h"
#include "collide.h"
#include "npc.h"
#include "face.h"
#define main sim_main
#include "sim.c"
#undef main
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P);
	static Player pl; Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	f32 worst = 0; int morphs = 0, bad = 0;
	for (int t = 0; t < 66 * 60; t++) {
		Ent_Tick(1.0f / 66);
		if (t % 33) continue;
		Face_BeginFrame(Ent_Now());
		u32 nn; Npc *l = Npc_List(&nn);
		for (u32 i = 0; i < nn; i++) {
			u32 base = 0;
			const PackNpcVert *v = Face_Morph(&l[i], 100, &base);
			if (!v) continue;
			morphs++;
			const PackNpcModel *m = &P.npcModels[l[i].model];
			for (u32 b = 0; b < m->numBatches; b++) {
				const PackNpcBatch *bt = &P.npcBatches[m->firstBatch + b];
				if (!bt->flexed) continue;
				for (u32 k = 0; k < bt->numVerts; k++) {
					const PackNpcVert *a = &v[bt->firstVert - base + k], *o = &P.npcVerts[bt->firstVert + k];
					f32 d = fabsf(a->pos[0] - o->pos[0]) + fabsf(a->pos[1] - o->pos[1]) + fabsf(a->pos[2] - o->pos[2]);
					if (!(d < 8)) bad++;
					if (d > worst || d != d) worst = d;
					if (a->mtx != o->mtx) bad++;
				}
			}
		}
	}
	printf("%s: %d morphs, worst vertex move %.2f units, %d bad\n", argv[1], morphs, worst, bad);
	return 0;
}
