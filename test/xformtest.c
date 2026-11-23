/* Host test: entity transforms stay rigid (orthonormal R, finite origin) while a map runs. */
#include "ent.h"
#include "collide.h"
#include "npc.h"
#define main sim_main
#include "sim.c"
#undef main
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static f32 rigid_err(f32 R[3][3])
{
	f32 e = 0;
	for (int a = 0; a < 3; a++)
		for (int b = 0; b < 3; b++) {
			f32 d = R[0][a] * R[0][b] + R[1][a] * R[1][b] + R[2][a] * R[2][b] - (a == b);
			e = fmaxf(e, fabsf(d));
		}
	return e;
}
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	CM_Init(&P);
	static Player pl;
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	int bad = 0, badNan = 0, worstI = -1;

	f32 worst = 0;
	for (int t = 0; t < 66 * 60; t++) {
		UserCmd uc = {0};
		uc.yaw = pl.yaw;
		Player_Tick(&pl, &uc);
		Ent_Tick(1.0f / 66);
		if (getenv("TRACE") && t % 330 == 0)
			printf("t=%d player %.0f %.0f %.0f yaw %.0f\n", t / 66, pl.origin[0], pl.origin[1], pl.origin[2], pl.yaw);
		u32 n;
		Entity *e = Ent_List(&n);
		for (u32 i = 1; i < n; i++) {
			if (e[i].flags & EF_KILLED) continue;
			f32 er = rigid_err(e[i].R);
			if (!isfinite(e[i].origin[0]) || !isfinite(e[i].origin[1]) || !isfinite(e[i].origin[2]) ||
			    fabsf(e[i].origin[0]) > 40000 || fabsf(e[i].origin[2]) > 40000) badNan++;
			if (er > 0.01f) bad++;
			if (er > worst) { worst = er; worstI = (int)i; }
		}
	}
	u32 n;
	Entity *e = Ent_List(&n);
	printf("%s: worst rigidity error %.3f (%s, parent %d), %d bad samples, %d off-map/NaN samples\n", argv[1], worst,
	       worstI > 0 ? e[worstI].classname : "-", worstI > 0 ? (int)e[worstI].parent : 0, bad, badNan);
	return 0;
}
