/* Host test: NPC heads at run time (scenes, look-at, gestures, act-busy) over N seconds. Every 0.25 s, each NPC's
 * head elevation relative to its pelvis from Npc_Pose; reports NPCs that spent time with the face > thr deg up. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static f32 pose[256][3][4];
static int upCount[512], samples[512];
static f32 worst[512];
static s16 worstSeq[512];
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	f32 secs = argc > 2 ? atof(argv[2]) : 60, thr = argc > 3 ? atof(argv[3]) : 25;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	for (int t = 0; t < (int)(secs * 66); t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		if (t % 16) continue;
		u32 nn; Npc *L = Npc_List(&nn);
		for (u32 i = 0; i < nn && i < 512; i++) {
			Npc *n = &L[i];
			if (n->prop || !n->skel || !P.skelLook) continue;
			s16 hb = P.skelLook[n->skel - P.skels].head;
			if (hb < 0) continue;
			Npc_Pose(n, pose);
			f32 e = (asinf(fmaxf(-1, fminf(1, -pose[hb][2][1]))) - asinf(fmaxf(-1, fminf(1, -pose[0][2][1])))) * 180 / (f32)M_PI;
			samples[i]++;
			if (e > thr) upCount[i]++;
			if (e > worst[i]) { worst[i] = e; worstSeq[i] = n->seq; }
		}
	}
	u32 nn, ne; Npc *L = Npc_List(&nn); Entity *E = Ent_List(&ne);
	int bad = 0, cnt = 0;
	for (u32 i = 0; i < nn && i < 512; i++) {
		if (!samples[i]) continue;
		cnt++;
		if (upCount[i] * 10 < samples[i]) continue; /* (a glance up now and then is fine) */
		bad++;
		Npc *n = &L[i];
		if (n->lookEnt > 0) printf("  [target %s %s at z %+.0f vs %+.0f] ", E[n->lookEnt].classname, E[n->lookEnt].name ? E[n->lookEnt].name : "-", E[n->lookEnt].origin[2], E[n->ent].origin[2]);
		printf("  [look ent %d yaw %.0f pitch %.0f w %.2f scene %d locked %d script %d] ", n->lookEnt, n->lookYaw, n->lookPitch, n->lookW, n->sceneLook, n->locked, n->script);
		printf("  %-16s %-26s face up >%.0f deg %d%% of the time (worst %+.0f in %s)\n", E[n->ent].classname,
		       E[n->ent].name ? E[n->ent].name : "-", thr, upCount[i] * 100 / samples[i], worst[i],
		       worstSeq[i] >= 0 ? P.entStrings + P.npcSeqs[n->skel->firstSeq + worstSeq[i]].name : "-");
	}
	printf("%s: %d npcs with heads, %d looking up most of the time\n", argv[1], cnt, bad);
	return 0;
}
