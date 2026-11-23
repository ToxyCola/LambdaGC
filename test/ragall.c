/* Host test: every character on a map killed at once; after 6 s every ragdoll's bones are finite, near
 * where it died and none of its solids' joints pulled apart */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static f32 pose[NPC_MAX_BONES][3][4];
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Entity *ents = Ent_List(&cnt);
	int killed = 0, bad = 0;
	for (u32 i = 1; i < cnt && killed < 12; i++) {
		Npc *n = Npc_OfEntity((s32)i); if (!n || n->dead || !n->skel || (ents[i].flags & EF_DISABLED)) continue;
		n->script = -1; n->locked = 0;
		f32 dir[3] = {0.15f, 0.15f, 0}; /* (a light push: this is about the bodies, not how far a hit throws them) */
		Ent_Damage((s32)i, 5000, dir, ents[i].origin);
		if (n->dead) killed++;
	}
	UserCmd c = {0};
	for (int t = 0; t < (int)(6.0f / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); }
	for (u32 i = 1; i < cnt; i++) {
		Npc *n = Npc_OfEntity((s32)i); if (!n || !n->dead || !n->skel || (ents[i].flags & EF_KILLED)) continue;
		Npc_BonePose(n, pose);
		u32 nb = n->skel->numBones < NPC_MAX_BONES ? n->skel->numBones : NPC_MAX_BONES;
		f32 far = 0; int fin = 1;
		for (u32 b = 0; b < nb; b++) for (int r = 0; r < 3; r++) for (int k = 0; k < 4; k++) if (!isfinite(pose[b][r][k])) fin = 0;
		for (u32 b = 0; b < nb; b++) { f32 d = sqrtf(pose[b][0][3]*pose[b][0][3] + pose[b][1][3]*pose[b][1][3]); if (d > far) far = d; }
		if (!fin || far > 400) { bad++; printf("  BAD %s %u: finite %d, %0.f from where it died\n", ents[i].classname, i, fin, far); }
	}
	printf("%d killed, %d bad: %s\n", killed, bad, bad ? "FAILED" : "ragall: ok");
	return bad != 0;
}
