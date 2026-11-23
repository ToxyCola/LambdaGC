/* Host test: a killed soldier / cop ragdolls from its .phy solids: falls, comes to rest on the floor (not
 * sunk into it), and keeps its shape (each solid stays its length from its parent) */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt; static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static f32 pose[NPC_MAX_BONES][3][4];
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Entity *ents = Ent_List(&cnt);
	printf("ragdoll tables: %u skeletons\n", P.numSkelRag);
	int tested = 0;
	for (u32 i = 1; i < cnt && tested < 3; i++) {
		if (strcmp(ents[i].classname, "npc_combine_s") && strcmp(ents[i].classname, "npc_metropolice") && strcmp(ents[i].classname, "npc_citizen") && strcmp(ents[i].classname, "npc_zombie") && strcmp(ents[i].classname, "npc_fastzombie") && strcmp(ents[i].classname, "npc_poisonzombie")) continue;
		Npc *n = Npc_OfEntity((s32)i); if (!n || n->dead) continue;
		u32 si = (u32)(n->skel - P.skels);
		if (si >= P.numSkelRag || P.skelRag[si][1] < 2) { printf("  %s: no ragdoll data\n", ents[i].classname); continue; }
		ents[i].flags &= ~EF_DISABLED;
		n->script = -1; n->locked = 0;
		/* on the floor where it stands */
		f32 feet[3]; memcpy(feet, ents[i].origin, 12);
		Npc_BonePose(n, pose);
		const PackRagSolid *rs = &P.ragSolids[P.skelRag[si][0]]; u32 nrs = P.skelRag[si][1];
		f32 len0[32];
		for (u32 k = 0; k < nrs && k < 32; k++) { s32 a = rs[k].parent; if (a < 0) { len0[k] = 0; continue; }
			f32 d[3] = {pose[rs[k].bone][0][3]-pose[a][0][3], pose[rs[k].bone][1][3]-pose[a][1][3], pose[rs[k].bone][2][3]-pose[a][2][3]}; len0[k] = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); }
		static f32 W0[NPC_MAX_BONES][3][4]; memcpy(W0, pose, sizeof(W0));
		f32 dir[3] = {1, 0, 0};
		Ent_Damage((s32)i, 1000, dir, ents[i].origin);
		UserCmd c = {0};
		for (int t = 0; t < (int)(5.0f / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); }
		Npc_BonePose(n, pose);
		f32 maxStretch = 0, lowest = 1e9f, highest = -1e9f;
		for (u32 k = 0; k < nrs && k < 32; k++) {
			u32 b = rs[k].bone; s32 a = rs[k].parent;
			/* world height of the solid's bone */
			f32 z = ents[i].origin[2] + ents[i].R[2][0] * pose[b][0][3] + ents[i].R[2][1] * pose[b][1][3] + ents[i].R[2][2] * pose[b][2][3];
			if (z < lowest) lowest = z;
			if (z > highest) highest = z;
			if (a < 0) continue;
			f32 d[3] = {pose[b][0][3]-pose[a][0][3], pose[b][1][3]-pose[a][1][3], pose[b][2][3]-pose[a][2][3]};
			f32 l = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
			f32 st = fabsf(l - len0[k]) / (len0[k] + 1e-3f);
			if (st > maxStretch) maxStretch = st;
		}
		/* the joints: each child's bone origin where its parent holds it, and turned no further than its
		 * ragdollconstraint lets it (both against the pose it died in / its bind) */
		f32 sep = 0, over = 0;
		const PackBone *bones = &P.npcBones[n->skel->firstBone];
		const PackRagLimit *rl = P.ragLimits ? &P.ragLimits[P.skelRag[si][0]] : NULL;
		for (u32 k = 0; k < nrs && k < 32; k++) {
			u32 b = rs[k].bone; s32 a = rs[k].parent; if (a < 0) continue;
			f32 d[3], loc[3], w[3];
			for (int c = 0; c < 3; c++) d[c] = W0[b][c][3] - W0[a][c][3];
			for (int c = 0; c < 3; c++) loc[c] = W0[a][0][c]*d[0] + W0[a][1][c]*d[1] + W0[a][2][c]*d[2];
			for (int c = 0; c < 3; c++) w[c] = pose[a][c][0]*loc[0] + pose[a][c][1]*loc[1] + pose[a][c][2]*loc[2] + pose[a][c][3] - pose[b][c][3];
			f32 e = sqrtf(w[0]*w[0]+w[1]*w[1]+w[2]*w[2]); if (e > sep) sep = e;
			if (!rl) continue;
			f32 rest[3][3], ref[3][3], rel[3][3];
			for (int x=0;x<3;x++) for (int y=0;y<3;y++) rest[x][y] = bones[a].poseToBone[x][0]*bones[b].poseToBone[y][0]+bones[a].poseToBone[x][1]*bones[b].poseToBone[y][1]+bones[a].poseToBone[x][2]*bones[b].poseToBone[y][2];
			for (int x=0;x<3;x++) for (int y=0;y<3;y++) ref[x][y] = pose[a][x][0]*rest[0][y]+pose[a][x][1]*rest[1][y]+pose[a][x][2]*rest[2][y];
			for (int x=0;x<3;x++) for (int y=0;y<3;y++) rel[x][y] = ref[0][x]*pose[b][0][y]+ref[1][x]*pose[b][1][y]+ref[2][x]*pose[b][2][y];
			f32 cc = (rel[0][0]+rel[1][1]+rel[2][2]-1)*0.5f; if (cc>1) cc=1; if (cc<-1) cc=-1; f32 an = acosf(cc), sn = sinf(an);
			f32 rv[3] = {rel[2][1]-rel[1][2], rel[0][2]-rel[2][0], rel[1][0]-rel[0][1]};
			for (int c = 0; c < 3; c++) { rv[c] = sn > 1e-5f ? rv[c]*an/(2*sn)*57.2958f : 0;
				f32 ex = rv[c] < rl[k].lo[c] ? rl[k].lo[c] - rv[c] : rv[c] > rl[k].hi[c] ? rv[c] - rl[k].hi[c] : 0;
				if (ex > over) over = ex; }
		}
		printf("    joints: worst separation %.2f units, worst limit overrun %.0f deg\n", sep, over);
		if (n->dead) { CHECK(sep < 1.0f, "ragdoll %u joints pulled apart (%.1f)", i, sep); CHECK(over < 15.0f, "ragdoll %u bent past its joint limits (%.0f deg)", i, over); }
		printf("  %s %u: dead %d, solids %u, joint length error max %.0f%%, lowest solid %.1f above where it stood, highest %.1f\n",
		       ents[i].classname, i, n->dead, nrs, maxStretch * 100, lowest - feet[2], highest - feet[2]);
		if (!n->dead) { printf("    (protected: not killable here)\n"); continue; }
		CHECK(maxStretch < 0.15f, "ragdoll %u stretched / collapsed (%.0f%%)", i, maxStretch * 100);
		/* (it may have fallen off a ledge or onto a table: against the world under and around it) */
		int inside = 0; f32 maxUp = 0;
		for (u32 k = 0; k < nrs && k < 32; k++) {
			u32 b = rs[k].bone; f32 w[3];
			for (int c = 0; c < 3; c++) w[c] = ents[i].origin[c] + ents[i].R[c][0]*pose[b][0][3] + ents[i].R[c][1]*pose[b][1][3] + ents[i].R[c][2]*pose[b][2][3];
			Trace tr; f32 z0[3] = {0,0,0}; f32 m1[3] = {-0.5f,-0.5f,-0.5f}, x1[3] = {0.5f,0.5f,0.5f};
			CM_BoxTrace(&tr, w, w, m1, x1, MASK_NPCSOLID & ~CONTENTS_MONSTERCLIP);
			if (tr.startsolid) inside++;
			f32 dn[3] = {w[0], w[1], w[2] - 400};
			CM_BoxTrace(&tr, w, dn, z0, z0, MASK_NPCSOLID & ~CONTENTS_MONSTERCLIP);
			f32 up = tr.fraction * 400; if (k == 0 && up > maxUp) maxUp = up;
		}
		printf("    %d of its solids' bones inside the world, its root %.0f above what's under it\n", inside, maxUp);
		CHECK(inside <= 2, "ragdoll %u sank into the world (%d bones)", i, inside);
		CHECK(maxUp < 24.0f, "ragdoll %u didn't fall (%.0f up)", i, maxUp);
		tested++;
	}
	printf("%s\n", fails || !tested ? "FAILED" : "ragtest: ok");
	return fails != 0 || !tested;
}
