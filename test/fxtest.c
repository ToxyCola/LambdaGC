/* material effects: every program runs without leaving its variables; scrolling moves, animations step,
 * flicker stays in range. Prints a summary per map. */
#define main sim_main
#include "sim.c"
#undef main
#include "fx.h"
#include <math.h>
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	Fx_Init(&P);
	int moving = 0, anim = 0, flicker = 0, bad = 0, unlit = 0, two = 0, selfl = 0, cams = 0, camTwo = 0;
	for (u32 i = 0; i < P.numMatFx; i++) {
		const PackMatFx *f = &P.matFx[i];
		if (f->firstOp + f->numOps > P.numFxOps || f->firstVar + f->numVars > P.numFxVars) bad++;
		for (u32 o = 0; o < f->numOps; o++) {
			const PackFxOp *op = &P.fxOps[f->firstOp + o];
			if (op->dst >= f->numVars) bad++;
		}
		unlit += !!(f->flags & FX_UNLIT); two += !!(f->flags & FX_TWOTEX); selfl += !!(f->flags & FX_SELFILLUM);
		if (f->mat < P.numMats && P.mats[f->mat].kind == MAT_CAMERA) { /* monitors: scanlines over the camera */
			cams++;
			camTwo += (f->flags & FX_TWOTEX) && f->tex2 >= 0;
		}
		FxState a, b;
		Fx_Tick(0.37f); a = *Fx_Of(&P, f->mat);
		Fx_Tick(1.91f); b = *Fx_Of(&P, f->mat);
		if (memcmp(a.xf1, b.xf1, sizeof(a.xf1)) || memcmp(a.xf2, b.xf2, sizeof(a.xf2))) moving++;
		if (a.tex != b.tex) anim++;
		if (a.color[0] != b.color[0] || a.alpha != b.alpha) flicker++;
		for (int k = 0; k < 3; k++) if (!isfinite(b.color[k]) || b.color[k] < 0 || b.color[k] > 4) bad++;
		if (!isfinite(b.xf2[0][3]) || !isfinite(b.xf1[1][3])) bad++;
		if (f->frames > 1 && (b.tex < f->frame0 || b.tex >= f->frame0 + f->frames)) bad++;
	}
	printf("%s: %u materials with effects (%d unlit, %d two-texture, %d self-lit, %d monitors / %d with an overlay); over 1.5 s %d scroll, %d animate, %d flicker; %d bad\n",
	       argv[1], P.numMatFx, unlit, two, selfl, cams, camTwo, moving, anim, flicker, bad);
	return bad != 0;
}
