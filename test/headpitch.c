/* Host test: NPCs "staring at the sky". After 20 s of idling, for each NPC with a head bone, the angle
 * between the head bone's long (x) axis and the model's up axis, and whether it leans back. Prints a
 * histogram and the worst offenders (counts and angles only). */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static f32 pose[256][3][4];
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	for (int t = 0; t < 66 * 20; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	u32 nn, ne; Npc *L = Npc_List(&nn); Entity *E = Ent_List(&ne);
	/* "near": stand the player 90 units in front of one NPC (argv[3] = its index among the NPCs) so it looks at him */
	if (argc > 3) {
		Npc *t = &L[atoi(argv[3])];
		f32 *o = E[t->ent].origin, yr = t->yaw * (f32)M_PI / 180;
		f32 at[3] = {o[0] + cosf(yr) * 90, o[1] + sinf(yr) * 90, o[2] + (argc > 4 ? atof(argv[4]) : 0) + 1};
		memcpy(pl.origin, at, sizeof(at));
		for (int t2 = 0; t2 < 66 * 3; t2++) { Ent_Tick(TICK_INTERVAL); memcpy(pl.origin, at, sizeof(at)); }
	}
	int hist[7] = {0}, n_ = 0, back = 0;
	for (u32 i = 0; i < nn; i++) {
		Npc *n = &L[i];
		if (n->prop || !n->skel || !P.skelLook) continue;
		s16 hb = P.skelLook[n->skel - P.skels].head;
		if (hb < 0) continue;
		Npc_Pose(n, pose);
		f32 *x = (f32[3]){pose[hb][0][0], pose[hb][1][0], pose[hb][2][0]};
		f32 l = sqrtf(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]);
		f32 ang = acosf(fminf(1, fmaxf(-1, x[2] / l))) * 180 / (f32)M_PI;
		int lean = x[0] / l < -0.35f; /* the head's axis tipped backwards: face turned up */
		hist[ang < 15 ? 0 : ang < 30 ? 1 : ang < 45 ? 2 : ang < 60 ? 3 : ang < 90 ? 4 : ang < 135 ? 5 : 6]++;
		n_++; back += lean && ang > 35;
		if (argc > 2 && !strcmp(argv[2], "all")) {
			/* skinning matrices: the head's tilt relative to the pelvis, for a forward axis of +X or +Y in
			 * the bind pose (degrees of elevation; + = looking up) */
			s16 pb = 0;
			f32 e[2];
			for (int ax = 0; ax < 2; ax++) {
				f32 hz = pose[hb][2][ax], pz = pose[pb][2][ax];
				e[ax] = (asinf(fmaxf(-1, fminf(1, hz))) - asinf(fmaxf(-1, fminf(1, pz)))) * 180 / (f32)M_PI;
			}
			printf("  #%u %-14s %-26s %-26s head tilt vs pelvis: fwd+X %+.0f fwd+Y %+.0f look %.0f/%.0f w %.2f\n", i, E[n->ent].classname,
			       E[n->ent].name ? E[n->ent].name : "-", n->seq >= 0 ? P.entStrings + P.npcSeqs[n->skel->firstSeq + n->seq].name : "-",
			       e[0], e[1], n->lookYaw, n->lookPitch, n->lookW);
		} else if (argc > 2 && ang > 35)
			printf("  %-18s %-24s %-28s head %.0f deg from up%s\n", E[n->ent].classname, E[n->ent].name ? E[n->ent].name : "-",
			       n->seq >= 0 ? P.entStrings + P.npcSeqs[n->skel->firstSeq + n->seq].name : "-", ang, lean ? " (tipped back)" : "");
	}
	printf("%s: %d npcs; head from up <15:%d <30:%d <45:%d <60:%d <90:%d <135:%d more:%d; tipped back >35deg: %d\n",
	       argv[1], n_, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], back);
	return 0;
}
