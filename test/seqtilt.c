/* Host test: for every NPC sequence, the head's elevation relative to the rest pose at mid-cycle (degrees,
 * + = face up). Lists the ones beyond a threshold. */
#define main sim_main
#include "sim.c"
#undef main
#include "npc.h"
static f32 a[256][3][4];
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	f32 thr = argc > 2 ? atof(argv[2]) : 30;
	int total = 0, over = 0;
	for (u32 s = 0; s < P.numSkels; s++) {
		const PackSkel *sk = &P.skels[s];
		const PackSkelLook *L = &P.skelLook[s];
		if (L->head < 0) continue;
		for (u16 q = 0; q < sk->numSeqs; q++) {
			const PackSeq *sp = &P.npcSeqs[sk->firstSeq + q];
			if (sp->flags & SEQF_DELTA) continue;
			Anim_PoseEx(&P, sk, (s16)q, 0.5f * (sp->numFrames > 1 ? (sp->numFrames - 1) / sp->fps : 0), 1, -1, 0, 1, NULL, a);
			/* skin matrices are relative to the bind pose: the head's change applied to model forward, minus the pelvis's */
			/* the bind pose faces -Y (the skinning turns it to face +X): the face direction is -(column 1) */
			f32 hz = -a[L->head][2][1], pz = -a[0][2][1];
			f32 e = (asinf(fmaxf(-1, fminf(1, hz))) - asinf(fmaxf(-1, fminf(1, pz)))) * 180 / (f32)M_PI;
			total++;
			if (fabsf(e) > thr) { over++; printf("  skel %u %-36s head %+.0f\n", s, P.entStrings + sp->name, e); }
		}
	}
	printf("%s: %d sequences, %d with the head more than %.0f deg off level\n", argv[1], total, over, thr);
	return 0;
}
