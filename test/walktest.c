/* Host test: NPCs that walk or run in place. Over a minute of the map running by itself (the player stood
 * at the start), any NPC that spends 3 s or more playing its walk / run sequence while moving less than
 * 20 units is reported: "stuck in a walk loop" */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
#define MAXN 512
static f32 since[MAXN], startPos[MAXN][3]; static u8 flagged[MAXN];
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	f32 secs = argc > 2 ? (f32)atof(argv[2]) : 60;
	UserCmd c = {0};
	int stuck = 0;
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL);
		u32 nn; Npc *L = Npc_List(&nn);
		Entity *ents = Ent_List(&cnt);
		for (u32 i = 0; i < nn && i < MAXN; i++) {
			Npc *n = &L[i]; Entity *e = &ents[n->ent];
			if (n->dead || n->prop || (e->flags & (EF_DISABLED | EF_KILLED))) { since[i] = 0; continue; }
			int walking = n->seq >= 0 && ((n->skel->seqWalk >= 0 && n->seq == n->skel->seqWalk) || (n->skel->seqRun >= 0 && n->seq == n->skel->seqRun) ||
			              (n->actsReady && ((n->actRunAim >= 0 && n->seq == n->actRunAim) || (n->actWalkAim >= 0 && n->seq == n->actWalkAim))));
			if (n->skel->seqIdle == n->seq) walking = 0;
			if (!walking) { since[i] = 0; continue; }
			if (since[i] == 0) memcpy(startPos[i], e->origin, 12);
			since[i] += TICK_INTERVAL;
			f32 d[3] = {e->origin[0]-startPos[i][0], e->origin[1]-startPos[i][1], e->origin[2]-startPos[i][2]};
			f32 moved = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
			if (moved > 20) { since[i] = TICK_INTERVAL; memcpy(startPos[i], e->origin, 12); }
			if (since[i] >= 3.0f && !flagged[i]) {
				flagged[i] = 1; stuck++;
				printf("  STUCK %s %s at %.0f %.0f %.0f: walk/run %d (moving %d script %d enemy %d locked %d chasing %d path %d/%d follow %d) t=%.0fs\n",
				       e->classname, e->name ? e->name : "-", e->origin[0], e->origin[1], e->origin[2], n->seq, n->moving, n->script, n->enemy, n->locked, n->chasing, n->pathPos, n->pathLen, n->followEnt, t * TICK_INTERVAL);
			}
		}
	}
	printf("%s: %d NPCs walking in place\n", argv[1], stuck);
	return stuck != 0;
}
