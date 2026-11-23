/* Host test: scene gestures and look-at.
 * - every gesture named by a scene event resolves on at least one skeleton in the map
 * - playing each resolvable gesture as a layer moves bones (and only by sane amounts)
 * - look-at turns the head toward a target and leaves the feet alone */
#include "ent.h"
#include "collide.h"
#include "npc.h"
#define main sim_main
#include "sim.c"
#undef main
static f32 a[256][3][4], b[256][3][4];

static f32 maxdiff(u32 nb, int *which)
{
	f32 m = 0;
	for (u32 i = 0; i < nb; i++) {
		/* rotation change, as units of travel at 20 units' reach (skinning translations swing with any turn) */
		f32 d = 0;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) {
				f32 e = fabsf(a[i][r][c] - b[i][r][c]) * 20.0f; /* rotation, as units at 20 units' reach */
				if (e > d)
					d = e;
			}
		if (d > m) {
			m = d;
			if (which)
				*which = (int)i;
		}
	}
	return m;
}

int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1]))
		return 1;
	u32 events = 0, named = 0, resolved = 0, moved = 0, tooFar = 0, nan = 0, withTags = 0;
	f32 worst = 0;
	for (u32 i = 0; i < P.numScenes; i++) {
		const PackScene *sc = &P.scenes[i];
		for (u32 k = 0; k < sc->numEvents; k++) {
			const PackSceneEvent *ev = &P.sceneEvents[sc->firstEvent + k];
			if (ev->type != SEV_GESTURE)
				continue;
			events++;
			const char *nm = P.entStrings + ev->p1;
			if (!*nm || !strcasecmp(nm, "null") || !strcasecmp(nm, "null1"))
				continue;
			named++;
			if (P.eventTags && P.eventTags[sc->firstEvent + k].numTags)
				withTags++;
			int found = 0;
			for (u32 s = 0; s < P.numSkels && !found; s++) {
				const PackSkel *sk = &P.skels[s];
				s16 q = Anim_FindSeq(&P, sk, nm);
				if (q < 0)
					continue;
				found = 1;
				s16 idle = sk->seqIdle;
				f32 best = 0;
				for (int c = 0; c <= 10; c++) {
					AnimLayer L = {q, c / 10.0f, 1.0f, c * 0.2f};
					AnimExtra ex = {&L, 1, 0, 0, 0};
					Anim_PoseEx(&P, sk, idle, 0.5f, 0, -1, 0, 1, NULL, a);
					Anim_PoseEx(&P, sk, idle, 0.5f, 0, -1, 0, 1, &ex, b);
					for (u32 bb = 0; bb < sk->numBones; bb++)
						for (int r = 0; r < 3; r++)
							for (int cc = 0; cc < 4; cc++)
								if (b[bb][r][cc] != b[bb][r][cc])
									nan++;
					f32 d = maxdiff(sk->numBones, NULL);
					if (d > best)
						best = d;
				}
				if (best > 0.5f)
					moved++;
				if (best > 120.0f) {
					tooFar++;
					const PackSeq *sq = &P.npcSeqs[sk->firstSeq + q];
					printf("  implausible: %s on skeleton %u (%.0f) = %s, %u bones, flags %x, %u layers, %u mask, %u frames\n", nm, s, best,
					       P.entStrings + sq->name, sk->numBones, sq->flags, sq->numLayers, sq->numBones, sq->numFrames);
				}
				if (best > worst)
					worst = best;
			}
			if (found)
				resolved++;
		}
	}
	printf("gestures: %u events, %u named, %u resolve on a skeleton, %u move bones (worst %.1f), %u implausible, %u NaN, "
	       "%u with timing tags\n", events, named, resolved, moved, worst, tooFar, nan, withTags);

	/* look-at: a skeleton with head bones, target 60 degrees left and a bit up */
	u32 looked = 0, lookSkels = 0, feetMoved = 0;
	for (u32 s = 0; s < P.numSkels; s++) {
		const PackSkel *sk = &P.skels[s];
		const PackSkelLook *L = &P.skelLook[s];
		if (L->head < 0 || sk->seqIdle < 0)
			continue;
		lookSkels++;
		AnimExtra ex = {NULL, 0, 60, 15, 1};
		Anim_PoseEx(&P, sk, sk->seqIdle, 0.5f, 0, -1, 0, 1, NULL, a);
		Anim_PoseEx(&P, sk, sk->seqIdle, 0.5f, 0, -1, 0, 1, &ex, b);
		int which = -1;
		f32 d = maxdiff(sk->numBones, &which);
		if (d > 1.0f)
			looked++;
		/* bone 0 (pelvis/root) must not move */
		f32 r0 = fabsf(a[0][0][3] - b[0][0][3]) + fabsf(a[0][1][3] - b[0][1][3]) + fabsf(a[0][2][3] - b[0][2][3]);
		if (r0 > 0.01f)
			feetMoved++;
	}
	printf("look-at: %u skeletons with a head, %u turn it, %u moved their root\n", lookSkels, looked, feetMoved);
	return (nan || tooFar || feetMoved) ? 1 : 0;
}
