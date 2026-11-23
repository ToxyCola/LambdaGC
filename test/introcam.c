/* Host test: the G-Man intro's second view. Samples the script_intro camera (entity, origin, angles, blend
 * modes) through the intro so the backgrounds' movement can be checked against the map's paths. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	f32 step = argc > 2 ? atof(argv[2]) : 1;
	for (int t = 0, k = 0; t < 66 * 75; t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		if (t % (int)(66 * step)) continue;
		IntroView v;
		if (!Ent_IntroView(&v)) { if (k++ < 1) printf("%5.1fs no intro\n", Ent_Now()); continue; }
		printf("%5.1fs cam %d (%.0f %.0f %.0f) ang (%.1f %.1f %.1f) fov %.0f modes %d/%d a %.2f/%.2f fade %.2f\n", Ent_Now(), v.hasCamera,
		       v.camOrigin[0], v.camOrigin[1], v.camOrigin[2], v.camAngles[0], v.camAngles[1], v.camAngles[2], v.fov, v.mode[0], v.mode[1],
		       v.alpha[0], v.alpha[1], v.fade[3]);
	}
	return 0;
}
