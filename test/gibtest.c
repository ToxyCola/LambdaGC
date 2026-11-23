/* Break pieces: break every breakable prop/brush on a map (a few at a time) and see where the
 * pieces go: how many fell to something solid under them, how many stuck where they spawned.
 * usage: gibtest <map.hgm>... */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "gibs.h"
#include "weapons.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	for (int a = 1; a < argc; a++) {
		Ent_Free();
		if (load(&P, argv[a])) continue;
		CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
		EntCallbacks cb = {cl};
		Ent_Spawn(&P, &pl, 1, &cb);

		u32 n; Entity *E = Ent_List(&n);
		int buried = 0, wet = 0, falling = 0, broke = 0, pieces = 0, fell = 0, stuck = 0, floating = 0, moved = 0;
		for (u32 i = 1; i < n; i++) {
			const char *hp = Ent_KV((s32)i, "_hp");
			int brush = E[i].cls == EC_FUNC_BREAKABLE;
			if ((!brush && (E[i].cls != EC_PROP || !hp)) || (E[i].flags & (EF_KILLED | EF_DISABLED)) || Ent_KV((s32)i, "_boom")) continue;
			Gibs_Clear();
			f32 d[3] = {1, 0, 0};
			Ent_Damage((s32)i, 5000, d, E[i].origin);
			if (!(E[i].flags & EF_KILLED)) continue;
			broke++;
			f32 p0[64][3]; int alive0[64];
			for (int g = 0; g < 64; g++) { int r; f32 l; alive0[g] = Gibs_Probe(g, p0[g], &r, &l); pieces += alive0[g]; }
			for (int t = 0; t < 66 * 3; t++) Gibs_Tick(1.0f / 66);
			for (int g = 0; g < 64; g++) {
				f32 p[3]; int r; f32 l;
				if (!alive0[g] || !Gibs_Probe(g, p, &r, &l)) continue;
				f32 dz = p0[g][2] - p[2], dx = p[0] - p0[g][0], dy = p[1] - p0[g][1];
				if (dz < 2 && dx * dx + dy * dy < 4) stuck++;
				else moved++;
				/* something solid within 10 units under it? */
				f32 b[3] = {p[0], p[1], p[2] - 10}; Trace tr; CM_TraceWorld(&tr, p, b, MASK_PLAYERSOLID);
				if (tr.fraction < 1) fell++; else { floating++; int c = CM_PointContents(p); if (c & CONTENTS_SOLID) buried++; else if (c & MASK_WATER) wet++; else if (!r) falling++; }
			}
		}
		printf("%s: %d broken, %d pieces; after 3 s %d moved, %d never moved, %d resting on something, %d with nothing under them (%d inside solid, %d in water, %d still moving)\n",
		       argv[a], broke, pieces, moved, stuck, fell, floating, buried, wet, falling);
	}
	return 0;
}
