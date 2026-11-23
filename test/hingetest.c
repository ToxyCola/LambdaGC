/* Host test: ball joints and hinges (phys_ballsocket, phys_hinge as two joints on its axis) hold: after the
 * map has run a while, each joint's two ends are still together */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	int n = 0, bad = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); pl.noclip = 1; EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		UserCmd c = {0};
		for (int t = 0; t < (int)(15 / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
		const char *mn = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
		int nb = 0, bb = 0; f32 worst = 0;
		for (int i = 0; i < Phys_ConstraintCount(); i++) {
			f32 x[3], y[3], len; int on;
			if (Phys_ConstraintProbe(i, x, y, &len, &on) != 3 || !on) continue;
			f32 d = sqrtf((x[0]-y[0])*(x[0]-y[0]) + (x[1]-y[1])*(x[1]-y[1]) + (x[2]-y[2])*(x[2]-y[2]));
			nb++; if (d > 8) bb++; if (d > worst) worst = d;
		}
		printf("  %s: %d joints, %d apart by more than 8 units (worst %.1f)\n", mn, nb, bb, worst);
		n += nb; bad += bb;
	}
	printf("%d joints, %d apart: %s\n", n, bad, bad == 0 ? "hingetest: ok" : "FAILED");
	return bad != 0;
}
