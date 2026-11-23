/* Host test: func_button and func_movelinear against CBaseButton / CFuncMoveLinear: a pressed button goes in,
 * comes back out after "wait" unless it's a toggle (flag 32) or wait -1; a toggle comes out when pressed
 * again; a locked one doesn't move; SetPosition on a movelinear travels there at its speed */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	int nb = 0, bb = 0, nl = 0, bl = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]); pl.noclip = 1; EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		tick(1);
		Entity *E = Ent_List(&cnt);
		const char *mn = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
		for (u32 i = 1; i < cnt; i++) {
			Entity *e = &E[i];
			if (e->flags & EF_KILLED) continue;
			if (e->cls == EC_FUNC_BUTTON && e->moveState == 0) {
				nb++; int err = 0; int lk = e->locked;
				f32 travel = e->moveLen / (e->speed > 0 ? e->speed : 1) + 0.2f;
				Ent_Input(i, "Press", "", 0); tick(travel);
				if (lk) { if (e->moveState != 0) err |= 1; }
				else {
					if (e->moveState != 2 && e->wait != 0) err |= 2;
					int stays = (e->spawnflags & 32) || e->wait < 0;
					tick(e->wait > 0 ? e->wait + travel + 0.2f : travel);
					if (stays && e->moveState != 2) err |= 4;
					if (!stays && e->moveState != 0) err |= 8;
					if (e->spawnflags & 32) { Ent_Input(i, "Press", "", 0); tick(travel + 0.2f); if (e->moveState != 0) err |= e->locked ? 0 : 16; }
				}
				bb += err != 0;
				if (err) printf("  %s button %s sf %d wait %.1f%s: err %d\n", mn, e->name ? e->name : "-", e->spawnflags, e->wait, lk ? " locked" : "", err);
			} else if (e->cls == EC_FUNC_MOVELINEAR && !e->locked) {
				nl++; int err = 0;
				f32 f0 = e->frac, t = f0 < 0.5f ? 0.75f : 0.25f;
				char p[16]; snprintf(p, sizeof p, "%.2f", t);
				Ent_Input(i, "SetPosition", p, 0);
				tick(TICK_INTERVAL * 2);
				f32 sp = e->speed > 0 ? e->speed : 100;
				if (e->moveLen * fabsf(t - f0) > sp * 0.1f && fabsf(e->frac - t) < 0.001f) err |= 1; /* jumped */
				tick(e->moveLen * fabsf(t - f0) / sp + 0.5f);
				if (fabsf(e->frac - t) > 0.001f) err |= 2;
				Ent_Input(i, t > 0.5f ? "Open" : "Close", "", 0);
				tick(e->moveLen / sp + 0.5f);
				if (fabsf(e->frac - (t > 0.5f ? 1 : 0)) > 0.001f) err |= 4;
				bl += err != 0;
				if (err) printf("  %s movelinear %s: err %d (frac %.2f)\n", mn, e->name ? e->name : "-", err, e->frac);
			}
		}
	}
	printf("%d buttons (%d wrong), %d movelinears (%d wrong): %s\n", nb, bb, nl, bl, !bb && !bl ? "movertest: ok" : "FAILED");
	return bb || bl;
}
