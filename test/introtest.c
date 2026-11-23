/* Host test: the first map's opening as a new game - cutscene cameras, fades, trains carrying the
 * player, scenes, and how much G-Man's face moves. Prints a timeline, no dialogue text. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "face.h"
#include "scene.h"
extern int stubSoundsPlayed, stubCaptions;
static void cl(const char *m, const char *l) { printf("%6.1fs *** CHANGELEVEL %s (%s)\n", Ent_Now(), m, l); }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	static Player pl; Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) { printf("spawn failed\n"); return 1; }
	u32 ne, nn; Entity *e = Ent_List(&ne); Npc *npcs = Npc_List(&nn);
	Npc *gman = NULL;
	for (u32 i = 0; i < nn; i++) if (e[npcs[i].ent].name && !strcasecmp(e[npcs[i].ent].name, "gman")) gman = &npcs[i];
	if (gman) printf("gman ent %d name %s flags %x; FindActor(gman)=%d\n", gman->ent, e[gman->ent].name ? e[gman->ent].name : "-", e[gman->ent].flags, Ent_FindActor("gman"));
	for (u32 i = 0; i < nn; i++) if (!strcmp(e[npcs[i].ent].classname, "npc_gman")) printf("  npc_gman at entity %d\n", npcs[i].ent);
	f32 secs = argc > 2 ? (f32)atof(argv[2]) : 120;
	UserCmd idle = {0};
	int lastView = -2; u32 lastFade = 0; f32 faceMax = 0; int faceFrames = 0;
	f32 lastPos[3]; memcpy(lastPos, pl.origin, 12);
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
		Player_Tick(&pl, &idle);
		Ent_Tick(TICK_INTERVAL);
		f32 o[3], a[3];
		int v = Ent_ViewOverride(o, a) ? 1 : 0;
		static f32 lo[3];
		if (v && (fabsf(o[0]-lo[0])+fabsf(o[1]-lo[1])+fabsf(o[2]-lo[2]) > 50)) { printf("%6.1fs   camera moves to (%.0f %.0f %.0f) looking %.0f %.0f\n", Ent_Now(), o[0], o[1], o[2], a[0], a[1]); memcpy(lo, o, 12); }
		if (v != lastView) { printf("%6.1fs camera: %s", Ent_Now(), v ? "cutscene" : "player"); if (v) printf(" at (%.0f %.0f %.0f) looking %.0f %.0f", o[0], o[1], o[2], a[0], a[1]); printf("\n"); lastView = v; }
		u32 f = Ent_ScreenFade();
		if ((f & 0xFF) && !(lastFade & 0xFF)) printf("%6.1fs fade starts (alpha %u)\n", Ent_Now(), f & 0xFF);
		if (!(f & 0xFF) && (lastFade & 0xFF)) printf("%6.1fs fade clear\n", Ent_Now());
		lastFade = f;
		f32 dm = 0; for (int k = 0; k < 3; k++) dm += (pl.origin[k] - lastPos[k]) * (pl.origin[k] - lastPos[k]);
		if (t % 330 == 0) { printf("%6.1fs player (%.0f %.0f %.0f) moved %.0f in 5s\n", Ent_Now(), pl.origin[0], pl.origin[1], pl.origin[2], sqrtf(dm)); memcpy(lastPos, pl.origin, 12); }
		if (gman && t % 4 == 0) {
			Face_BeginFrame(Ent_Now());
			static int shown;
			if (!shown && gman->speakSound >= 0) { shown = 1; printf("%6.1fs G-Man speaking; face model %u: ctl %u flex %u\n", Ent_Now(), gman->model, P.npcFaces[gman->model].numCtl, P.npcFaces[gman->model].numFlex); }
			u32 base; const PackNpcVert *m = Face_Morph(gman, 10, &base);
			static int dbg; if (dbg++ < 1) printf("morph %p base %u numFlex %u firstFlex %u flex0 desc %d nva %u phonkeys %u phonemes %u sphi %p\n", (void*)m, base, P.npcFaces[gman->model].numFlex, P.npcFaces[gman->model].firstFlex, P.flexes[P.npcFaces[gman->model].firstFlex].desc, P.flexes[P.npcFaces[gman->model].firstFlex].numVA, P.numPhonKeys, P.numPhonemes, (void*)P.soundPhon);
			if (m) {
				f32 mx = 0;
				const PackNpcModel *nm = &P.npcModels[gman->model];
				const PackNpcBatch *lb = &P.npcBatches[nm->firstBatch + nm->numBatches - 1];
				u32 cnt = lb->firstVert + lb->numVerts - base;
				for (u32 k = 0; k < cnt; k++) {
					const PackNpcVert *o2 = &P.npcVerts[base + k];
					for (int c = 0; c < 3; c++) { f32 d = fabsf(m[k].pos[c] - o2->pos[c]); if (d > mx) mx = d; }
				}
				if (mx > 0.05f) faceFrames++;
				if (mx > faceMax) faceMax = mx;
			}
		}
	}
	printf("lines %d, captions %d; G-Man face moved in %d samples (max %.2f units)\n", stubSoundsPlayed, stubCaptions, faceFrames, faceMax);
	return 0;
}
