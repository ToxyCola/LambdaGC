/* Host test: the level's sound effects. Soundscapes (which one is active as the player walks, its dsp room,
 * loops and random sounds), animation sound events (NPC footsteps, AE_CL_PLAYSOUND), moving things' sounds
 * and physics impacts, over N seconds with the player walking a loop. Counts only. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "soundscape.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
extern int stubSoundsPlayed, stubEverywhere, stubIsPlaying, stubDsp;
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	f32 secs = argc > 2 ? atof(argv[2]) : 60;
	stubIsPlaying = 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Scape_Init(&P);
	u32 ne; Entity *E = Ent_List(&ne);
	int scapeEnts = 0, esndEnts = 0;
	for (u32 i = 1; i < ne; i++) {
		if (Ent_KV((s32)i, "_scape")) scapeEnts++;
		if (E[i].esnd[0] >= 0 || E[i].esnd[1] >= 0 || E[i].esnd[2] >= 0) esndEnts++;
	}
	UserCmd c = {0};
	int changes = 0, before = stubSoundsPlayed;
	s32 last = -1;
	int dsps[32] = {0};
	for (int t = 0; t < (int)(secs * 66); t++) {
		/* wander: walk forward, turning slowly */
		c.forwardmove = 150; c.yaw += 0.4f;
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		f32 ear[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64};
		Scape_Tick(TICK_INTERVAL, ear, pl.origin);
		if (Scape_Current() != last) { changes++; last = Scape_Current(); if (stubDsp >= 0 && stubDsp < 32) dsps[stubDsp]++; }
	}
	printf("%s: %u soundscapes, %u elements, %u entities with one; %d soundscape changes (dsp:", argv[1], P.numScapes,
	       P.numScapeElems, scapeEnts, changes);
	for (int k = 0; k < 32; k++) if (dsps[k]) printf(" %d x%d", k, dsps[k]);
	printf("); %u animation sound events; %d moving things with sounds; %d sounds played in %.0f s (%d everywhere)\n",
	       P.numSeqSounds, esndEnts, stubSoundsPlayed - before, secs, stubEverywhere);
	return 0;
}
