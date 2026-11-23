/* light styles: initial states from the pack, named lights switching their style, pattern animation,
 * and the style layers referring to real faces and pages */
#define main sim_main
#include "sim.c"
#undef main
#include "light.h"
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Light_Init(&P);
	static Player pl; Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; if (Ent_Spawn(&P, &pl, 1, &cb)) return 1;
	int bad = 0;
	for (u32 i = 0; i < P.numStyleFaces; i++) {
		const PackStyleFace *f = &P.styleFaces[i];
		if (f->face >= P.numFaces || f->page >= P.numStylePages || (P.numTex && P.stylePages[f->page].tex >= P.numTex) || f->style >= 64) bad++;
	}
	for (u32 i = 0; i < P.numStylePages; i++)
		if (P.stylePages[i].vscale < 1.0f || P.stylePages[i].vscale > 64) bad++;
	u32 n; Entity *e = Ent_List(&n);
	int lights = 0, switched = 0;
	for (u32 i = 1; i < n; i++) {
		if (e[i].cls != EC_LIGHT) continue;
		{ const char *sk0 = Ent_KV((s32)i, "style"); if (!sk0 || atoi(sk0) <= 0) continue; }
		lights++;
		const char *sk = Ent_KV((s32)i, "style"); int s = sk ? atoi(sk) : 0;
		f32 before = Light_StyleValue(s);
		Ent_Input((s32)i, "Toggle", "", 0);
		f32 mid = Light_StyleValue(s);
		Ent_Input((s32)i, "Toggle", "", 0);
		if (mid != before && Light_StyleValue(s) == before) switched++;
	}
	int on = 0;
	for (int s = 32; s < 64; s++) on += Light_StyleValue(s) > 0.5f;
	/* a pattern: style 10 (fluorescent flicker) over a second */
	int changes = 0; f32 last = Light_StyleValue(10);
	for (int t = 0; t < 60; t++) { Light_StyleTick(1.0f / 60); if (Light_StyleValue(10) != last) changes++, last = Light_StyleValue(10); }
	printf("%s: %u styled face layers on %u pages (%d bad), %d named lights (%d switch both ways), %d named styles on at start, style 10 changed %d times in 1 s\n",
	       argv[1], P.numStyleFaces, P.numStylePages, bad, lights, switched, on, changes);
	return bad || switched != lights;
}
