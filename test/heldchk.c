/* NPCs' held items: after a few seconds, which NPCs hold something, whether the weapons pack has its
 * model, and whether both hands line up (character _rhand and the item's own). usage: heldchk map weapons */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "../engine/source/weapons.c"
static void m34_mul(f32 a[3][4], const f32 b[3][4], const f32 c[3][4]) { f32 t[3][4]; for (int r = 0; r < 3; r++) for (int k = 0; k < 4; k++) t[r][k] = b[r][0]*c[0][k] + b[r][1]*c[1][k] + b[r][2]*c[2][k] + (k == 3 ? b[r][3] : 0); memcpy(a, t, sizeof(t)); }
static void m34_inv(f32 a[3][4], const f32 b[3][4]) { f32 t[3][4]; for (int r = 0; r < 3; r++) { for (int k = 0; k < 3; k++) t[r][k] = b[k][r]; t[r][3] = -(b[0][r]*b[0][3] + b[1][r]*b[1][3] + b[2][r]*b[2][3]); } memcpy(a, t, sizeof(t)); }
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static Pack W;
int main(int argc, char **argv)
{
	if (argc < 3 || load(&W, argv[2])) return 1;
	u32 sz, isz, wsz, asz;
	u8 *wc = chunk("WEAP", &sz), *ic = chunk("ITEM", &isz), *sc = chunk("WSND", &wsz), *ac = chunk("AMMX", &asz);
	swap(wc, 1, "4"); u32 nw = *(u32 *)wc; swap(wc + 4, nw, "4221121111444" "222222222222");
	swap(ic, 1, "4"); u32 ni = *(u32 *)ic; swap(ic + 4, ni, "421122");
	swap(sc, wsz / 2, "2"); swap(ac, asz / 2, "2");
	Weap_Setup(&W, wc + 4, nw, ic + 4, ni, (s16 *)sc, wsz / 2, (s16 *)ac, asz / 2);
	wp = W;
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	Ent_Spawn(&P, &pl, 1, &cb);
	for (int t = 0; t < 66 * 3; t++) Ent_Tick(TICK_INTERVAL);
	{ u32 ne; Ent_List(&ne); for (u32 i = 1; i < ne; i++) { const char *tn = Ent_KV((s32)i, "targetname"); if (tn && !strcmp(tn, "logic_start_train")) Ent_Input((s32)i, "Trigger", "", 0); } }
	for (int t = 0; t < 66 * 2; t++) Ent_Tick(TICK_INTERVAL);
	u32 nn; Npc *L = Npc_List(&nn);
	int holding = 0, resolved = 0, attached = 0;
	for (u32 i = 0; i < nn; i++) {
		Npc *n = &L[i];
		if (!n->held[0] || n->dead) continue;
		holding++;
		int it = item_named(n->held);
		s16 m = it >= 0 ? items[it].model : -1;
		resolved += m >= 0 && (u32)m < wp.numNpcModels;
		f32 hp[3], hR[3][3]; s32 wb; f32 loc[3][4];
		int ok = m >= 0 && Npc_Attachment(n, "_rhand", hp, hR); (void)wb; (void)loc;
		attached += ok;
		const char *tn = Ent_KV(n->ent, "targetname");
		printf("  %s holds %s: item %d model %d %s\n", tn ? tn : "-", n->held, it, m, ok ? "placed in the hand" : "NOT PLACED");
		if (ok && (strstr(n->held, "suitcase") || getenv("ALLHELD"))) {
			/* the engine's bone-merge, as draw_npc_weapons does it, then the item's vertices in the hand's frame */
			const PackNpcFace *f = &wp.npcFaces[m]; s32 wbn = -1;
			for (u32 k = 0; k < f->numAtt; k++) { const PackAttachment *a = &wp.attachments[f->firstAtt + k]; if (!strcasecmp(wstr(a->name), "_rhand")) wbn = a->bone; }
			const PackSkel *sk = &wp.skels[wp.npcModels[m].skel];
			static f32 wpose[64][3][4];
			s16 sq = sk->seqIdle >= 0 ? sk->seqIdle : (sk->numSeqs ? 0 : -1);
			Anim_Pose(&wp, sk, sq, 0, 1, -1, 0, 1, wpose);
			const PackBone *bn = &wp.npcBones[sk->firstBone + (u32)wbn];
			f32 inv[3][4], Wh[3][4], T[3][4], H[3][4], Hi[3][4];
			m34_inv(inv, bn->poseToBone); m34_mul(Wh, wpose[wbn], inv);
			for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) H[r][c] = hR[r][c]; H[r][3] = hp[r]; }
			m34_inv(inv, Wh); m34_mul(T, H, inv); m34_inv(Hi, H);
			{ s32 ab = -1; for (u32 k = 0; k < f->numAtt; k++) { const PackAttachment *a = &wp.attachments[f->firstAtt + k]; if (!strcasecmp(wstr(a->name), "_rattach")) ab = a->bone; }
			  f32 ap[3], aR[3][3];
			  if (ab >= 0 && Npc_Attachment(n, "_rattach", ap, aR)) { f32 A[3][4], Ti[3][4], L2[3][4];
				for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) A[r][c] = aR[r][c]; A[r][3] = ap[r]; }
				m34_inv(Ti, T); m34_mul(L2, Ti, A); m34_mul(wpose[ab], L2, wp.npcBones[sk->firstBone + (u32)ab].poseToBone); printf("    (attach bone merged)\n"); } }
			f32 mn[3] = {1e9,1e9,1e9}, mx[3] = {-1e9,-1e9,-1e9}, cs[3] = {0}; int nv = 0;
			const PackNpcModel *nm = &wp.npcModels[m];
			for (u32 bi = 0; bi < nm->numBatches; bi++) {
				const PackNpcBatch *bt = &wp.npcBatches[nm->firstBatch + bi];
				for (u32 v = 0; v < bt->numVerts; v++) {
					const PackNpcVert *pv = &wp.npcVerts[bt->firstVert + v];
					u32 bone = bt->bones[pv->mtx / 3];
					f32 a[3], b2[3], c2[3];
					for (int r = 0; r < 3; r++) a[r] = wpose[bone][r][0]*pv->pos[0] + wpose[bone][r][1]*pv->pos[1] + wpose[bone][r][2]*pv->pos[2] + wpose[bone][r][3];
					for (int r = 0; r < 3; r++) b2[r] = T[r][0]*a[0] + T[r][1]*a[1] + T[r][2]*a[2] + T[r][3];
					for (int r = 0; r < 3; r++) c2[r] = Hi[r][0]*b2[0] + Hi[r][1]*b2[1] + Hi[r][2]*b2[2] + Hi[r][3];
					for (int r = 0; r < 3; r++) { cs[r] += c2[r]; if (c2[r] < mn[r]) mn[r] = c2[r]; if (c2[r] > mx[r]) mx[r] = c2[r]; }
					nv++;
				}
			}
			for (u32 bb = 0; bb < sk->numBones; bb++) { const PackBone *b3 = &wp.npcBones[sk->firstBone + bb]; f32 w3[3][4]; m34_inv(inv, b3->poseToBone); m34_mul(w3, wpose[bb], inv);
				printf("    bone %u parent %d frame t %.2f %.2f %.2f  x %.2f %.2f %.2f  ptb t %.2f %.2f %.2f\n", bb, b3->parent, w3[0][3], w3[1][3], w3[2][3], w3[0][0], w3[1][0], w3[2][0], b3->poseToBone[0][3], b3->poseToBone[1][3], b3->poseToBone[2][3]); }
			printf("    engine: %d verts, in the hand frame centroid %.1f %.1f %.1f min %.1f %.1f %.1f max %.1f %.1f %.1f (idle seq %d of %u)\n", nv, cs[0]/nv, cs[1]/nv, cs[2]/nv, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], sq, sk->numSeqs);
		}
	}
	printf("%d holding, %d with a model, %d placed\n", holding, resolved, attached);
	return 0;
}
