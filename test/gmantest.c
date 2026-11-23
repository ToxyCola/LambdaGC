#include "ent.h"
#include "collide.h"
#include "npc.h"

#define main sim_main
#include "sim.c"
#undef main
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int World_PointCluster(const Pack *p, const f32 pos[3]);
int main(int argc, char **argv)
{
	load(&P, argv[1]);
	u32 sz; u8 *c = chunk("VISD", &sz);
	if (c) { swap(c, 1, "4"); P.numClusters = *(u32 *)c; swap(c + 4, P.numClusters, "4"); P.visOffsets = (u32 *)(c + 4); P.visBase = c + 4 + 4 * P.numClusters; }
	CM_Init(&P);
	static Player pl;
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	Ent_Spawn(&P, &pl, 1, &cb);
	for (int t = 0; t < 66 * 40; t++) {
		Ent_Tick(1.0f / 66);
		if (t % (66 * 2)) continue;
		u32 n, nn; Entity *e = Ent_List(&n); Npc *l = Npc_List(&nn);
		for (u32 i = 0; i < nn; i++) {
			Entity *g = &e[l[i].ent];
			if (t == 0) printf("npc %s name %s\n", g->classname, g->name ? g->name : "-");
			if (!g->name || strcmp(g->name, "gman")) continue;
			f32 eye[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64};
			IntroView iv; int on = Ent_IntroView(&iv);
			f32 ap[3] = {0, 0, 0}, aR[3][3];
			int okA = Npc_Attachment(&l[i], "cameraeye", ap, aR);
			f32 vo[3] = {0}, va[3] = {0};
			int cs = Ent_ViewOverride(vo, va);
			printf("   zoom %.1f attach cameraeye %d at %.0f %.0f %.0f | view %d at %.0f %.0f %.0f ang %.1f %.1f\n", Ent_ZoomFov(), okA, ap[0], ap[1], ap[2], cs, vo[0], vo[1], vo[2], va[0], va[1]);
			f32 gc[3] = {g->origin[0], g->origin[1], g->origin[2] + 36};
			printf("t=%d envfade %08x gman %.0f %.0f %.0f flags %x cl %d | player eye %.0f %.0f %.0f cl %d yaw %.0f | intro %d cam %d fov %.0f mode %d fade %.0f\n",
			       t / 66, Ent_ScreenFade(), g->origin[0], g->origin[1], g->origin[2], g->flags, World_PointCluster(&P, gc), eye[0], eye[1], eye[2],
			       World_PointCluster(&P, eye), pl.yaw, on, iv.hasCamera, iv.fov, iv.mode[0], iv.fade[3]);
		}
	}
	return 0;
}
/* (attachment probe appended) */
