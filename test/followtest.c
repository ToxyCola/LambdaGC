/* Host test: an NPC following the player keeps up as he walks a route, stops near him when he stops (not
 * walking on the spot, not overshooting), and never stands more than a few seconds walking without moving */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt; static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Entity *ents = Ent_List(&cnt);
	s32 who = -1;
	for (u32 i = 1; i < cnt && who < 0; i++) {
		if (strcmp(ents[i].classname, "npc_citizen") && strcmp(ents[i].classname, "npc_alyx") && strcmp(ents[i].classname, "npc_barney")) continue;
		Npc *n = Npc_OfEntity((s32)i);
		if (!n || n->dead || (ents[i].flags & (EF_DISABLED | EF_TEMPLATE | EF_KILLED)) || n->script >= 0 || n->locked) continue;
		who = (s32)i;
	}
	if (who < 0) { printf("no free citizen\n"); return 0; }
	Npc *n = Npc_OfEntity(who);
	/* the player walks a route: from beside the NPC out along open floor (whatever the node graph reaches) */
	memcpy(pl.origin, ents[who].origin, 12);
	pl.origin[0] += 40;
	Npc_Follow(n, 0, 100.0f);
	UserCmd c = {0};
	f32 walkInPlace = 0, worstWIP = 0, maxGap = 0;
	f32 yaw = 0;
	for (int t = 0; t < (int)(30.0f / TICK_INTERVAL); t++) {
		/* he walks for 20 s (turning now and then), then stands */
		memset(&c, 0, sizeof(c));
		if (t * TICK_INTERVAL < 20) {
			if (t % (int)(4.0f / TICK_INTERVAL) == 0) yaw += 70;
			c.yaw = yaw; c.forwardmove = 1;
		}
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL);
		int walking = n->seq >= 0 && (n->seq == n->skel->seqWalk || n->seq == n->skel->seqRun);
		static f32 last[3]; static int init;
		if (!init) { memcpy(last, ents[who].origin, 12); init = 1; }
		f32 dm = sqrtf((ents[who].origin[0]-last[0])*(ents[who].origin[0]-last[0]) + (ents[who].origin[1]-last[1])*(ents[who].origin[1]-last[1]));
		memcpy(last, ents[who].origin, 12);
		if (walking && dm < 0.05f) walkInPlace += TICK_INTERVAL; else walkInPlace = 0;
		if (walkInPlace > worstWIP) worstWIP = walkInPlace;
		f32 gap = sqrtf((pl.origin[0]-ents[who].origin[0])*(pl.origin[0]-ents[who].origin[0]) + (pl.origin[1]-ents[who].origin[1])*(pl.origin[1]-ents[who].origin[1]));
		if (gap > maxGap) maxGap = gap;
	}
	f32 gap = sqrtf((pl.origin[0]-ents[who].origin[0])*(pl.origin[0]-ents[who].origin[0]) + (pl.origin[1]-ents[who].origin[1])*(pl.origin[1]-ents[who].origin[1]));
	printf("  %s %s: final gap %.0f (max %.0f), moving at the end %d, longest walk-in-place %.1f s\n", ents[who].classname, ents[who].name ? ents[who].name : "-", gap, maxGap, n->moving, worstWIP);
	CHECK(worstWIP < 1.0f, "walked in place %.1f s", worstWIP);
	CHECK(!n->moving || gap > 164, "still walking after the player stopped");
	printf("%s\n", fails ? "FAILED" : "followtest: ok");
	return fails != 0;
}
