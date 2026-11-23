/* Trainstation 1 customs: the player walks into trigger *54, the takeaway citizen is spawned and led by a
 * cop through Barney's door; when he passes the NPC trigger in the hallway, cage_playerclip (the gate
 * between the two cops) is disabled and the player can walk north into the pen. Prints what happens. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include <string.h>
#include "scene.h"
static u32 dummyN;
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static Entity *named(const char *n) { u32 ne; Entity *E = Ent_List(&ne); for (u32 i = 1; i < ne; i++) if (E[i].name && !strcmp(E[i].name, n) && !(E[i].flags & EF_KILLED)) return &E[i]; return NULL; }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	for (int t = 0; t < 66 * 2; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	f32 at[3] = {-4576, -669, -20};
	memcpy(pl.origin, at, 12);
	Entity *clip = named("cage_playerclip");
	int opened = -1;
	for (int t = 0; t < 66 * 40; t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		if (t % 132 == 0) {
			Entity *ci = named("customs_takeaway_citizen"), *co = named("customs_takeaway_cop");
			printf("%5.1fs cflags %x scene %d citizen %s (%.0f %.0f %.0f) cop %s (%.0f %.0f) clip flags %x\n", Ent_Now(), ci ? ci->flags : 0, Scene_IsPlaying((s32)(named("customs_takeaway_vcd") - Ent_List(&dummyN))), ci ? "yes" : "no", ci ? ci->origin[0] : 0, ci ? ci->origin[1] : 0, ci ? ci->origin[2] : 0,
			       co ? "yes" : "no", co ? co->origin[0] : 0, co ? co->origin[1] : 0, clip->flags);
		}
		if (opened < 0 && (clip->flags & EF_DISABLED)) opened = t;
	}
	printf("cage clip %s\n", opened >= 0 ? "disabled" : "STILL SOLID");
	/* walk north through the cop gate */
	f32 s[3] = {-4224, -700, -20};
	memcpy(pl.origin, s, 12);
	c.yaw = 90; c.forwardmove = 190;
	for (int t = 0; t < 66 * 2; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	printf("player walked from y -700 to y %.0f (gate at y -626)\n", pl.origin[1]);
	return opened < 0;
}
