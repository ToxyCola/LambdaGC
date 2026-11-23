/* static prop grid: traces must come out the same as testing every hull (prints a digest to compare builds) */
#define main sim_main
#include "sim.c"
#undef main
#include <time.h>
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P);
	/* random sweeps around static props and the spawn */
	u32 seed = 12345, hits = 0; double sum = 0;
	clock_t t0 = clock();
	for (int i = 0; i < 200000; i++) {
		f32 a[3], b[3];
		const f32 *c = P.numStaticProps ? P.staticProps[(seed = seed * 1103515245u + 12345u) % P.numStaticProps].origin : P.spawnOrigin;
		for (int k = 0; k < 3; k++) {
			seed = seed * 1103515245u + 12345u; a[k] = c[k] + (f32)((seed >> 8) % 512) - 256;
			seed = seed * 1103515245u + 12345u; b[k] = c[k] + (f32)((seed >> 8) % 1024) - 512;
		}
		f32 mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72}, z[3] = {0, 0, 0};
		Trace tr;
		CM_BoxTrace(&tr, a, b, (i & 1) ? mn : z, (i & 1) ? mx : z, MASK_PLAYERSOLID);
		if (tr.fraction < 1) hits++;
		if (getenv("DUMP")) printf("%d %.5f %d %d %.3f %.3f %.3f\n", i, tr.fraction, tr.startsolid, tr.allsolid, tr.normal[0], tr.normal[1], tr.normal[2]);
		sum += tr.fraction * (i % 97 + 1) + tr.startsolid * 3 + tr.allsolid * 7 + tr.normal[0] + tr.normal[1] * 2 + tr.normal[2] * 3;
	}
	printf("%s: %u hits, digest %.6f, %.0f ms\n", argv[1], hits, sum, (clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
	return 0;
}
