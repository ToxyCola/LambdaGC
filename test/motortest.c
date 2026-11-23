/* Host test: phys_motor (CPhysMotor) spins what it's attached to about its axis, up to its speed, and the
 * object stays on its hinge (phys_hinge or the motor's own) */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	int tried = 0, ok = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); pl.noclip = 1; EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		tick(1);
		Entity *E = Ent_List(&cnt);
		const char *mn = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
		for (u32 i = 1; i < cnt; i++) {
			if (strcmp(E[i].classname, "phys_motor")) continue;
			const char *at = Ent_KV(i, "attach1"); s32 o = at ? Ent_FindActor(at) : -1;
			if (o <= 0 || !Phys_IsBody(o)) { printf("  %s %s: attached %s isn't simulated\n", mn, E[i].name ? E[i].name : "-", at ? at : "-"); continue; }
			if (E[o].mdl < 0 ? (E[o].spawnflags & 0x8000) : (E[o].spawnflags & 8)) { printf("  %s %s: %s is motion-disabled until the map lets it go\n", mn, E[i].name ? E[i].name : "-", at); continue; }
			f32 ax[3]; sscanf(Ent_KV(i, "axis"), "%f %f %f", &ax[0], &ax[1], &ax[2]);
			f32 d[3] = {ax[0] - E[i].origin[0], ax[1] - E[i].origin[1], ax[2] - E[i].origin[2]}; f32 l = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); for (int k = 0; k < 3; k++) d[k] /= l;
			f32 target = (f32)atof(Ent_KV(i, "speed"));
			f32 c0[3]; memcpy(c0, Phys_Com(o), 12);
			Ent_Input(i, "TurnOn", "", 0);
			const char *su = Ent_KV(i, "spinup"); f32 spin = su ? (f32)atof(su) : 0;
			tick(spin + 2.0f);
			/* the turn rate about the axis over a short time from the orientation */
			f32 R0[3][3]; memcpy(R0, E[o].R, sizeof(R0));
			tick(0.05f);
			f32 R1[3][3]; memcpy(R1, E[o].R, sizeof(R1));
			/* a vector perpendicular to the axis, in body frame at t0, compared at t1 */
			f32 p[3] = {d[1], -d[0], 0}; if (fabsf(d[2]) > 0.9f) { p[0] = 1; p[1] = 0; p[2] = 0; }
			f32 lp[3]; for (int k = 0; k < 3; k++) lp[k] = R0[0][k]*p[0] + R0[1][k]*p[1] + R0[2][k]*p[2];
			f32 w1[3]; for (int k = 0; k < 3; k++) w1[k] = R1[k][0]*lp[0] + R1[k][1]*lp[1] + R1[k][2]*lp[2];
			f32 cr[3] = {p[1]*w1[2]-p[2]*w1[1], p[2]*w1[0]-p[0]*w1[2], p[0]*w1[1]-p[1]*w1[0]};
			f32 sn = cr[0]*d[0]+cr[1]*d[1]+cr[2]*d[2], cs = p[0]*w1[0]+p[1]*w1[1]+p[2]*w1[2];
			f32 rate = atan2f(sn, cs) * 180.0f / (f32)M_PI / 0.05f;
			const f32 *c1 = Phys_Com(o); f32 drift = sqrtf((c1[0]-c0[0])*(c1[0]-c0[0])+(c1[1]-c0[1])*(c1[1]-c0[1])+(c1[2]-c0[2])*(c1[2]-c0[2]));
			tried++;
			int good = fabsf(rate - target) < fabsf(target) * 0.25f + 5 || (fabsf(target) > 1000 && rate * target > 0 && fabsf(rate) > 300);
			ok += good;
			printf("  %s %-28s on %-26s target %7.1f deg/s, turning %7.1f, moved %5.1f%s\n", mn, E[i].name ? E[i].name : "-", at, target, rate, drift, good ? "" : "  WRONG");
		}
	}
	printf("%d of %d motors spin: %s\n", ok, tried, ok == tried && tried ? "motortest: ok" : "FAILED");
	return ok != tried;
}
