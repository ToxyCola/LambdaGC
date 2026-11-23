/* Host test: no prop hangs in the air. After the map has run a few seconds every physics prop (not held
 * by a constraint, not motion-disabled) rests on something: the world, a static prop, a brush entity, or
 * another prop under it.  usage: hovertest map.hgm... */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	int total = 0, floating = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free(); if (load(&P, argv[a])) continue; CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, 0); EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
		tick(6);
		Entity *E = Ent_List(&cnt);
		for (u32 i = 1; i < cnt; i++) {
			if (!Phys_IsBody((s32)i) || (E[i].flags & (EF_KILLED | EF_DISABLED))) continue;
			if (Ent_KV((s32)i, "parentname")) continue;
			if (!strcmp(E[i].classname, "func_physbox") ? (E[i].spawnflags & 0x8000) : (E[i].spawnflags & 8)) continue; /* (motion disabled: stays where it was put) */
			total++;
			/* its centre over what's under it, against its own size (any way up) */
			const f32 *cm = Phys_Com((s32)i);
			f32 lo[3], hi[3]; int m = E[i].mdl;
			if (m >= 0 && (u32)m < P.numModels) { memcpy(lo, P.models[m].mins, 12); memcpy(hi, P.models[m].maxs, 12); }
			else if (E[i].bmodel > 0) { memcpy(lo, P.bmodels[E[i].bmodel].mins, 12); memcpy(hi, P.bmodels[E[i].bmodel].maxs, 12); }
			else continue;
			f32 ext = 0; for (int k = 0; k < 3; k++) { f32 h = (hi[k] - lo[k]) * 0.5f; if (h > ext) ext = h; }
			Trace tr; f32 z[3] = {0, 0, 0}, d[3] = {cm[0], cm[1], cm[2] - 4000};
			CM_SetIgnoreEnt((s32)i);
			CM_BoxTrace(&tr, cm, d, z, z, MASK_PLAYERSOLID | CONTENTS_WATER | CONTENTS_SLIME);
			CM_SetIgnoreEnt(-1);
			f32 gap = cm[2] - tr.endpos[2];
			int supported = tr.startsolid || gap <= ext + 6;
			f32 c[3] = {cm[0], cm[1], cm[2]};
			if (!supported) {
				floating++;
				printf("  %s: %u %s %s centre %.0f above what's under it (size %.0f) at %.0f %.0f %.0f (spawnflags %u)\n", argv[a], i, E[i].classname, E[i].name ? E[i].name : "-", gap, ext, c[0], c[1], c[2], E[i].spawnflags);
			}
		}
	}
	printf("%d of %d props float: %s\n", floating, total, floating ? "FAILED" : "hovertest: ok");
	return floating != 0;
}
