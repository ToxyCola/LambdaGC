/* Host test: how long the simulation takes per tick (Ent_Tick: entities, NPCs, physics) with the player
 * walking around, on the host CPU. The GameCube's 486 MHz Gekko is roughly 20-30x slower per tick than
 * a current desktop core, so this is a proxy for where the time goes, not an absolute number. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include <string.h>
#include <time.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	f32 secs = argc > 2 ? atof(argv[2]) : 30;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	double ent = 0, ply = 0, worst = 0;
	int n = 0;
	for (int t = 0; t < (int)(secs * 66); t++) {
		c.forwardmove = (t / 300) % 2 ? 150 : 0; c.yaw += 0.2f;
		double a = now_ms();
		Player_Tick(&pl, &c);
		double b = now_ms();
		Ent_Tick(TICK_INTERVAL);
		double d = now_ms();
		ply += b - a; ent += d - b; n++;
		if (d - b > worst) worst = d - b;
	}
	u32 nn; Npc_List(&nn);
	printf("%s: %u npcs; per tick: entities %.3f ms (worst %.2f), player %.3f ms\n", argv[1], nn, ent / n, worst, ply / n);
	return 0;
}
