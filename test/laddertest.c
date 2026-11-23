/* Host test: useable ladders. From each side of each ladder, walk at it, climb, and see where the player ends up. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	u32 ne; Entity *E = Ent_List(&ne);
	int ladders = 0, climbed = 0;
	for (u32 i = 1; i < ne; i++) {
		f32 a[3], b[3];
		if (!strcasecmp(E[i].classname, "func_useableladder")) {
			const char *p0 = Ent_KV(i, "point0"), *p1 = Ent_KV(i, "point1");
			sscanf(p0, "%f %f %f", &a[0], &a[1], &a[2]); sscanf(p1, "%f %f %f", &b[0], &b[1], &b[2]);
		} else continue;
		if (a[2] > b[2]) { f32 t[3]; memcpy(t, a, 12); memcpy(a, b, 12); memcpy(b, t, 12); }
		ladders++;
		int ok = 0;
		for (int d = 0; d < 4 && !ok; d++) {
			f32 th = d * 90.0f * (f32)M_PI / 180;
			f32 at[3] = {a[0] - cosf(th) * 30, a[1] - sinf(th) * 30, a[2]};
			if (!Player_Spawn(&pl, at, 0, d * 90.0f)) continue;
			UserCmd c = {0}; c.forwardmove = 1; c.yaw = d * 90.0f; c.pitch = -10;
			f32 maxz = pl.origin[2]; int was = 0;
			for (int t = 0; t < (int)(4 / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); if (pl.ladder >= 0) was = 1; if (pl.origin[2] > maxz) maxz = pl.origin[2]; }
			printf("  ladder %d from side %d: mounted %d, rose %.0f of %.0f, ended at z %.0f (ladder %d)\n", ladders, d, was, maxz - a[2], b[2] - a[2], pl.origin[2], pl.ladder);
			if (was && maxz >= b[2] - 8) ok = 1;
		}
		climbed += ok;
	}
	printf("%s: %d useable ladders, %d climbed to the top\n", argv[1], ladders, climbed);
	return 0;
}
