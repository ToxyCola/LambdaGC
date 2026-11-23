/* Host test: look-at axes. For each skeleton with a head: pose its idle with look yaw/pitch and measure how the
 * head's skinning rotation changes relative to the pelvis: +pitch should raise the face by about the head's
 * share, +yaw should turn it left without raising it. */
#define main sim_main
#include "sim.c"
#undef main
#include "npc.h"
static f32 a[256][3][4], b[256][3][4];
/* rotation of the head's skin matrix relative to its rest, applied to a direction */
static void dirs(f32 m[3][4], const f32 v[3], f32 o[3]) { for (int r = 0; r < 3; r++) o[r] = m[r][0]*v[0]+m[r][1]*v[1]+m[r][2]*v[2]; }
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	for (u32 s = 0; s < P.numSkels; s++) {
		const PackSkel *sk = &P.skels[s];
		const PackSkelLook *L = &P.skelLook[s];
		if (L->head < 0 || sk->seqIdle < 0) continue;
		f32 tests[3][2] = {{0, 20}, {0, -20}, {45, 0}};
		printf("skel %u (%u bones):", s, sk->numBones);
		for (int t = 0; t < 3; t++) {
			AnimExtra ex = {NULL, 0, tests[t][0], tests[t][1], 1};
			Anim_PoseEx(&P, sk, sk->seqIdle, 0.5f, 0, -1, 0, 1, NULL, a);
			Anim_PoseEx(&P, sk, sk->seqIdle, 0.5f, 0, -1, 0, 1, &ex, b);
			/* the change: D = B * A^-1 (rotation part), applied to model forward (+X) */
			f32 D[3][3];
			for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
				D[r][c] = 0; for (int k = 0; k < 3; k++) D[r][c] += b[L->head][r][k] * a[L->head][c][k];
			}
			f32 f[3] = {D[0][0], D[1][0], D[2][0]};
			printf("  look(%g,%g): fwd -> (%+.2f %+.2f %+.2f) elev %+.0f yaw %+.0f", tests[t][0], tests[t][1], f[0], f[1], f[2],
			       asinf(fmaxf(-1, fminf(1, f[2]))) * 180 / (f32)M_PI, atan2f(f[1], f[0]) * 180 / (f32)M_PI);
		}
		printf("\n");
	}
	return 0;
}
