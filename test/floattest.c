/* Host test: props dropped into water float: they settle at the surface (not sunk, not flying out), level
 * off and stop bobbing; heavy dense ones (mass over their box) sink */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt; static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static f32 surface_above(const f32 p[3])
{
	f32 q[3] = {p[0], p[1], p[2]};
	for (int i = 0; i < 400 && (CM_PointContents(q) & MASK_WATER); i++) q[2] += 1;
	return q[2];
}
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Entity *ents = Ent_List(&cnt);
	/* a water spot: under a wet point well away from the player */
	int tested = 0, floated = 0;
	for (u32 i = 1; i < cnt && tested < 6; i++) {
		if (!Phys_IsBody((s32)i) || Phys_Mass((s32)i) > 60) continue;
		/* find water near it: straight down from it */
		f32 p[3]; memcpy(p, ents[i].origin, 12);
		int wet = 0;
		for (int d = 0; d < 600; d += 8) { f32 q[3] = {p[0], p[1], p[2] - d}; if (CM_PointContents(q) & MASK_WATER) { wet = 1; p[2] = q[2]; break; } }
		if (!wet) continue;
		f32 surf = surface_above(p);
		f32 drop[3] = {p[0] + tested * 70, p[1], surf + 48};
		f32 pw[3] = {drop[0], drop[1], surf - 4};
		if (!(CM_PointContents(pw) & MASK_WATER)) continue;
		Phys_Teleport((s32)i, drop);
		Phys_Wake((s32)i);
		UserCmd c = {0};
		f32 zs[4]; int zi = 0;
		for (int t = 0; t < (int)(12.0f / TICK_INTERVAL); t++) {
			Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL);
			if (t >= (int)(10.0f / TICK_INTERVAL) && t % 40 == 0 && zi < 4) zs[zi++] = ents[i].origin[2];
		}
		if (fabsf(ents[i].origin[2] - drop[2]) < 0.5f) { printf("  %u: motion disabled, skipped\n", i); continue; }
		f32 bob = 0; for (int k = 1; k < zi; k++) bob = fmaxf(bob, fabsf(zs[k] - zs[0]));
		f32 rel = ents[i].origin[2] - surf;
		const char *mdl = Ent_KV((s32)i, "model");
		printf("  %u %s mass %.1f: %.1f from the surface after 12 s, still bobbing %.1f, up axis z %.2f\n", i, mdl ? mdl : "?", Phys_Mass((s32)i), rel, bob, ents[i].R[2][2]);
		CHECK(rel < 40, "prop %u flew out of the water (%.0f above)", i, rel);
		CHECK(bob < 3, "prop %u still bobbing %.1f after 10 s", i, bob);
		floated += rel > -40;
		tested++;
	}
	printf("%d props dropped in water, %d at the surface\n%s\n", tested, floated, fails || !tested ? "FAILED" : "floattest: ok");
	return fails != 0 || !tested;
}
