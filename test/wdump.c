/* HUD: with the real weapons.hgm and fonts, pick things up and draw the HUD through the GX checker:
 * the pickup history's entries, the weapon selection's icon, squad and poison panels; no GX errors. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "gxv.h"
#include "../engine/source/weapons.c"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static Pack W;
int Pack_LoadMemory(Pack *p, u8 *d, u32 n) { (void)p; (void)d; (void)n; return -1; }
void *Pack_Chunk(const Pack *p, const char *t, u32 *n) { (void)p; (void)t; *n = 0; return NULL; }
static u8 *slurp(const char *p, u32 *n) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, 2); *n = ftell(f); rewind(f);
	u8 *d = aligned_alloc(32, (*n + 31) & ~31u); fread(d, 1, *n, f); fclose(f); return d; }
int main(int argc, char **argv)
{
	/* argv: map, hl2gc dir */
	char p[512]; u32 n;
	snprintf(p, sizeof p, "%s/font_title.bin", argv[2]); u8 *d = slurp(p, &n); printf("title font %d\n", d ? Text_LoadFont(FONT_TITLE, d, n) : -1);
	snprintf(p, sizeof p, "%s/font_icons.bin", argv[2]); d = slurp(p, &n); printf("icon font %d\n", d ? Text_LoadFont(FONT_ICONS, d, n) : -1);
	snprintf(p, sizeof p, "%s/font.bin", argv[2]); d = slurp(p, &n); if (d) Text_LoadFont(FONT_CAPTION, d, n);
	snprintf(p, sizeof p, "%s/weapons.hgm", argv[2]);
	if (load(&W, p)) { printf("weapons.hgm failed\n"); return 1; }
	u32 sz, isz, wsz, asz, csz;
	u8 *wc = chunk("WEAP", &sz), *ic = chunk("ITEM", &isz), *sc = chunk("WSND", &wsz), *ac = chunk("AMMX", &asz), *icn = chunk("WICN", &csz);
	swap(wc, 1, "4"); u32 nw = *(u32 *)wc; swap(wc + 4, nw, "4221121111444" "222222222222");
	swap(ic, 1, "4"); u32 ni = *(u32 *)ic; swap(ic + 4, ni, "421122");
	swap(sc, wsz / 2, "2"); swap(ac, asz / 2, "2");
	Weap_Setup(&W, wc + 4, nw, ic + 4, ni, (s16 *)sc, wsz / 2, (s16 *)ac, asz / 2);
	{ u32 tsz; u8 *tc = chunk("TEXI", &tsz); if (tc) { swap(tc, 1, "4"); W.numTex = *(u32 *)tc; swap(tc + 4, W.numTex, "221111" "44"); W.tex = (void *)(tc + 4); } W.texData = chunk("TEXD", &tsz); }
	wp = W; loaded = 1;
	{ u32 fsz2 = 0; u8 *fx = chunk("FXTX", &fsz2); for (int i = 0; i < FX_COUNT; i++) fxTex[i] = -1;
	  if (fx) { u32 n = fx[0] << 8 | fx[1]; int have = 0; for (u32 i = 0; i < n && i < FX_COUNT; i++) { fxTex[i] = (s16)(fx[2 + i * 2] << 8 | fx[3 + i * 2]); have += fxTex[i] >= 0; } printf("effect textures: %d of %d\n", have, FX_COUNT); } }

	if (icn) memcpy(wIcon, icn, csz < sizeof(wIcon) ? csz : sizeof(wIcon));
	if (load(&P, argv[1])) return 1;
	for (u32 w = 0; w < numWeaps; w++) {
		for (int which = 0; which < 2; which++) {
			s16 m = which ? weaps[w].wm : weaps[w].vm;
			if (m < 0 || (u32)m >= wp.numNpcModels) { printf("%s %s: no model (%d)\n", wstr(weaps[w].name), which ? "world" : "view", m); continue; }
			const PackNpcModel *nm = &wp.npcModels[m];
			const PackSkel *sk = &wp.skels[nm->skel];
			u32 nb; model_pose(m, which ? -1 : weaps[w].seq[SQ_IDLE], 0, 1, &nb);
			f32 mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9}; u32 nv = 0, ni = 0, strips = 0, skipped = 0, bad = 0;
			for (u32 b = 0; b < nm->numBatches; b++) {
				const PackNpcBatch *bt = &wp.npcBatches[nm->firstBatch + b];
				const PackMat *mat = &wp.mats[bt->mat];
				if (mat->kind == MAT_NODRAW || mat->tex >= (s32)wp.numTex) skipped++;
				strips += bt->strip; ni += bt->numIndices;
				for (u32 j = 0; j < bt->numIndices; j++) if (wp.npcIndices[bt->firstIndex + j] >= bt->numVerts) bad++;
				for (u32 v = 0; v < bt->numVerts; v++) {
					const PackNpcVert *pv = &wp.npcVerts[bt->firstVert + v];
					u32 k = pv->mtx / 3; if (k >= bt->numBones) { bad++; continue; }
					u32 bone = bt->bones[k]; if (bone >= nb) { bad++; continue; }
					for (int r = 0; r < 3; r++) { f32 a = pose[bone][r][0]*pv->pos[0] + pose[bone][r][1]*pv->pos[1] + pose[bone][r][2]*pv->pos[2] + pose[bone][r][3];
						if (!(a == a)) bad++; if (a < mn[r]) mn[r] = a; if (a > mx[r]) mx[r] = a; }
					nv++;
				}
			}
			printf("%s %s: model %d skel %u bones %u seqs %u idle %d; %u batches (%u strips, %u skipped) %u verts %u indices, bad %u; box %.0f..%.0f %.0f..%.0f %.0f..%.0f\n",
			       wstr(weaps[w].name), which ? "world" : "view", m, nm->skel, sk->numBones, sk->numSeqs, which ? sk->seqIdle : weaps[w].seq[SQ_IDLE], nm->numBatches, strips, skipped, nv, ni, bad,
			       mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
		}
	}
	if (getenv("DUMPONLY")) return 0;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Weap_LevelStart(&P, &pl);
	extern int rt_init(const Pack *p);
	printf("render init %d, skin pack %d (%u textures)\n", rt_init(&P), Render_InitSkinPack(&wp), wp.numTex);
	inv.suit = 1;
	Camera cam; memset(&cam, 0, sizeof cam);
	cam.origin = (guVector){pl.origin[0], pl.origin[1], pl.origin[2] + 64}; cam.fovy = 59; cam.fovx = 75; Camera_Update(&cam);
	Mtx view; guVector tg = {cam.origin.x + cam.forward.x, cam.origin.y + cam.forward.y, cam.origin.z + cam.forward.z}; guLookAt(view, &cam.origin, &cam.up, &tg);
	f32 proj[4][4]; guPerspective(proj, 59, 4.0f / 3.0f, 4, 16000);
	gxv.maxDist = 40000;
	for (u32 w = 0; w < numWeaps; w++) {
		give_weapon((s32)w); inv.cur = (s32)w; vm_play(SQ_IDLE);
		unsigned long v0 = gxv.vertices, d0 = gxv.draws, e0 = gxv.errors;
		Weap_DrawViewmodel(view, &cam, proj, 640, 480);
		printf("%s: idle seq %d (draw %d fire %d reload %d): viewmodel %lu draws %lu vertices, %lu errors\n", wstr(weaps[w].name), weaps[w].seq[SQ_IDLE], weaps[w].seq[SQ_DRAW], weaps[w].seq[SQ_PRIMARY], weaps[w].seq[SQ_RELOAD], gxv.draws - d0, gxv.vertices - v0, gxv.errors - e0);
	}
	/* effects: shots at the walls, an explosion, things in flight */
	memcpy(eye, pl.origin, 12); eye[2] += 64; map = &P; player = &pl;
	for (int i = 0; i < 24; i++) { f32 yaw = i * 0.26f; f32 d[3] = {cosf(yaw), sinf(yaw), -0.2f}; shoot_bullet(d, 1); }
	f32 at[3] = {pl.origin[0] + 100, pl.origin[1], pl.origin[2] + 40};
	Weap_Effect(WFX_EXPLOSION, at); Weap_Effect(WFX_SPARK, at);
	static const u8 types[] = {P_ROCKET, P_BOLT, P_SMGNADE, P_BALL, P_FRAG};
	for (int k = 0; k < 5; k++) { projs[k].alive = 1; projs[k].type = types[k]; memcpy(projs[k].pos, at, 12); projs[k].pos[1] += k * 20; projs[k].vel[0] = 600; projs[k].vel[2] = 100; projs[k].t = 0.3f; projs[k].fuse = 99; }
	int nd = 0, np = Weap_ImpactCounts(&nd);
	unsigned long v0 = gxv.vertices, d0 = gxv.draws, e0 = gxv.errors;
	Weap_DrawWorld(view, &cam);
	printf("world effects: %d decals %d particles, an explosion, 5 projectiles: %lu draws %lu vertices, %lu errors\n", nd, np, gxv.draws - d0, gxv.vertices - v0, gxv.errors - e0);
	for (int k = 0; k < 3; k++) { int it = item_named((const char *[]){"proj_rocket", "proj_bolt", "proj_smgnade"}[k]); printf("  projectile model %d: item %d model %d\n", k, it, it >= 0 ? items[it].model : -1); }
	gxv_end_frame();
	printf("total errors %lu overflow %lu\n", gxv.errors, gxv.overflow);
	return 0;
}
