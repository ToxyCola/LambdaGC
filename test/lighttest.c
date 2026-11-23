/* model lighting: per static prop, its cube before/after the direct light, and the floor under it
 * (tools compare the cube's top face with the floor's lightmap). Prints CSV, then a timing line. */
#define main sim_main
#include "sim.c"
#undef main
#include "light.h"
#include <time.h>
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P);
	clock_t t0 = clock();
	Light_Init(&P);
	double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	printf("prop,x,y,z,fx,fy,fz,amb_up,lit_up,amb_avg,lit_avg\n");
	for (u32 i = 0; i < P.numStaticProps; i++) {
		const u8 *a = P.staticCubes ? P.staticCubes[i] : NULL;
		const u8 *l = Light_StaticCube(&P, i);
		if (!a || !l || !P.staticLightOrg) continue;
		const f32 *o = P.staticLightOrg[i];
		Trace tr; f32 e[3] = {o[0], o[1], o[2] - 512};
		CM_TraceWorld(&tr, o, e, CONTENTS_SOLID);
		if (tr.fraction >= 1 || tr.startsolid) continue;
		int sa = 0, sl = 0; for (int k = 0; k < 18; k++) sa += a[k], sl += l[k];
		printf("%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", i, o[0], o[1], o[2], tr.endpos[0], tr.endpos[1], tr.endpos[2],
		       (a[12] + a[13] + a[14]) / 3.0, (l[12] + l[13] + l[14]) / 3.0, sa / 18.0, sl / 18.0);
	}
	/* moving things: relight cost */
	t0 = clock();
	u8 c[18]; int n = 0;
	for (u32 i = 0; i < P.numStaticProps && n < 2000; i++, n++) {
		f32 pos[3] = {P.staticProps[i].origin[0], P.staticProps[i].origin[1], P.staticProps[i].origin[2] + 40};
		Light_EntityCube(&P, -1, pos, c);
	}
	fprintf(stderr, "%s: %u lights, %u props lit in %.0f ms, %d relights %.3f ms each\n", argv[1], P.numWorldLights, P.numStaticProps, ms, n,
	        n ? (clock() - t0) * 1000.0 / CLOCKS_PER_SEC / n : 0);
	return 0;
}
