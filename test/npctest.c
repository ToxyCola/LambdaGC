/* Host test for milestone 4: spawn a map's NPCs, run its intro, and report what the characters did -
 * scenes played, lines spoken (counts only), scripted sequences, movement, poses and collision. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "scene.h"

extern int stubSoundsPlayed, stubCaptions, stubEverywhere, stubFar;
extern const f32 *stubListener;
extern f32 stubSoundSeconds;
extern int stubHudMessages, stubHints;

static void cl(const char *m, const char *l) { printf("*** CHANGELEVEL %s (%s)\n", m, l); }

static int pose_ok(const Npc *n, f32 *height)
{
	static f32 pose[NPC_MAX_BONES][3][4];
	Npc_Pose(n, pose);
	f32 zmax = -1e9f, zmin = 1e9f;
	for (u32 b = 0; b < n->skel->numBones; b++)
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++)
				if (!isfinite(pose[b][r][c]) || fabsf(pose[b][r][c]) > 1e4f)
					return 0;
	/* skin a few vertices of the model and measure its height */
	const PackNpcModel *m = &P.npcModels[n->model];
	for (u32 bi = 0; bi < m->numBatches; bi++) {
		const PackNpcBatch *bt = &P.npcBatches[m->firstBatch + bi];
		for (u32 v = 0; v < bt->numVerts; v += 7) {
			const PackNpcVert *pv = &P.npcVerts[bt->firstVert + v];
			u32 bone = bt->bones[pv->mtx / 3];
			f32 z = pose[bone][2][0] * pv->pos[0] + pose[bone][2][1] * pv->pos[1] + pose[bone][2][2] * pv->pos[2] +
			        pose[bone][2][3];
			if (z > zmax) zmax = z;
			if (z < zmin) zmin = z;
		}
	}
	*height = zmax - zmin;
	return 1;
}

int main(int argc, char **argv)
{
	if (load(&P, argv[1]))
		return 1;
	CM_Init(&P);
	static Player pl;
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) {
		printf("spawn failed\n");
		return 1;
	}
	u32 nn, ne;
	Npc *npcs = Npc_List(&nn);
	Entity *ents = Ent_List(&ne);
	printf("%u NPCs, %u skeletons, %u sequences, %u scenes, %u sounds\n", nn, P.numSkels,
	       P.numSkels ? P.skels[P.numSkels - 1].firstSeq + P.skels[P.numSkels - 1].numSeqs : 0, P.numScenes, P.numSounds);
	int badPose = 0;
	for (u32 i = 0; i < nn; i++) {
		f32 h;
		if (!pose_ok(&npcs[i], &h))
			badPose++;
		else if (i < 12)
			printf("  %-22s %-18s at (%6.0f %6.0f %5.0f) seq %d  height %.0f%s\n", ents[npcs[i].ent].classname,
			       ents[npcs[i].ent].name ? ents[npcs[i].ent].name : "-", ents[npcs[i].ent].origin[0],
			       ents[npcs[i].ent].origin[1], ents[npcs[i].ent].origin[2], npcs[i].seq, h,
			       (ents[npcs[i].ent].flags & EF_DISABLED) ? " (template)" : "");
	}
	stubListener = pl.origin;
	f32 secs = argc > 2 ? (f32)atof(argv[2]) : 60;
	UserCmd idle = {0, 0, 0, 0, P.spawnAngles[1]};
	int scenesSeen = 0, wasPlaying[4096] = {0};
	f32 maxMove = 0;
	f32 start[128][3];
	for (u32 i = 0; i < nn && i < 128; i++)
		memcpy(start[i], ents[npcs[i].ent].origin, 12);
	int inSolid = 0;
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
		Player_Tick(&pl, &idle);
		Ent_Tick(TICK_INTERVAL);
		for (u32 i = 1; i < ne && i < 4096; i++)
			if (ents[i].cls == EC_SCENE && Scene_IsPlaying((s32)i) && !wasPlaying[i]) {
				wasPlaying[i] = 1;
				scenesSeen++;
			}
		if (t % 200 == 0)
			for (u32 i = 0; i < nn; i++) {
				const Entity *e = &ents[npcs[i].ent];
				if (e->flags & (EF_DISABLED | EF_KILLED))
					continue;
				f32 c[3] = {e->origin[0], e->origin[1], e->origin[2] + 36};
				if (CM_PointContents(c) & CONTENTS_SOLID)
					inSolid++;
			}
	}
	/* then walk the player into every trigger, as in enttest, so the map's scripts start their scenes */
	if (argc > 3) {
		for (u32 i = 1; i < ne; i++) {
			if (ents[i].cls != EC_TRIGGER_ONCE && ents[i].cls != EC_TRIGGER_MULTIPLE)
				continue;
			if (!ents[i].enabled || (ents[i].flags & EF_KILLED) || ents[i].bmodel <= 0)
				continue;
			const PackBModel *bm = &P.bmodels[ents[i].bmodel];
			for (int k = 0; k < 3; k++)
				pl.origin[k] = (bm->mins[k] + bm->maxs[k]) * 0.5f + ents[i].origin[k];
			pl.origin[2] = bm->mins[2] + ents[i].origin[2] + 1;
			memset(pl.velocity, 0, 12);
			for (int t = 0; t < 600; t++) {
				Ent_Tick(TICK_INTERVAL);
				for (u32 k = 1; k < ne && k < 4096; k++)
					if (ents[k].cls == EC_SCENE && Scene_IsPlaying((s32)k) && !wasPlaying[k]) {
						wasPlaying[k] = 1;
						scenesSeen++;
					}
			}
		}
	}
	for (u32 i = 0; i < nn && i < 128; i++) {
		const f32 *o = ents[npcs[i].ent].origin;
		f32 d = sqrtf((o[0] - start[i][0]) * (o[0] - start[i][0]) + (o[1] - start[i][1]) * (o[1] - start[i][1]));
		if (d > maxMove)
			maxMove = d;
	}
	int scripts = 0, scriptsDone = 0, moved = 0, following = 0;
	for (u32 i = 1; i < ne; i++)
		if (ents[i].cls == EC_SCRIPTED_SEQUENCE) {
			scripts++;
			if (ents[i].phase == 0 || ents[i].phase == 5)
				scriptsDone++;
		}
	for (u32 i = 0; i < nn && i < 128; i++) {
		const f32 *o = ents[npcs[i].ent].origin;
		if (fabsf(o[0] - start[i][0]) + fabsf(o[1] - start[i][1]) > 32)
			moved++;
		if (npcs[i].followEnt >= 0)
			following++;
	}
	{
		extern u32 sceneGestures, sceneLooks;
		u32 gl = 0, gmax = 0;
		u32 cnt; Npc *L = Npc_List(&cnt);
		for (u32 i = 0; i < cnt; i++) if (L[i].lookW > 0.5f) gl++;
		(void)gmax;
		printf("gestures played %u, look-at events %u, characters looking at something now %u\n", sceneGestures, sceneLooks, gl);
	}
	printf("after %.0f s: scenes started %d, lines played %d (%.0f s of audio), captions %d\n", secs, scenesSeen,
	       stubSoundsPlayed, stubSoundSeconds, stubCaptions);
	printf("  NPCs moved %d (furthest %.0f units), following %d, scripted sequences %d (%d idle/finished)\n", moved,
	       maxMove, following, scripts, scriptsDone);
	printf("  sounds everywhere %d, out of earshot when started %d\n", stubEverywhere, stubFar);
	printf("  on-screen messages %d, hints %d\n", stubHudMessages, stubHints);
	printf("  bad poses %d, NPC-in-solid samples %d, player at (%.0f %.0f %.0f)\n", badPose, inSolid, pl.origin[0],
	       pl.origin[1], pl.origin[2]);
	return 0;
}
