/* Host test: load an .hgm (big-endian) into native structs and drive the real collide.c/player.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pack.h"
#include "collide.h"
#include "player.h"

static u8 *file;
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static u8 *chunk(const char *tag, u32 *size) {
	u32 n = be32(file + 8);
	for (u32 i = 0; i < n; i++) {
		u8 *e = file + 16 + i * 12;
		if (!memcmp(e, tag, 4)) { *size = be32(e + 8); return file + be32(e + 4); }
	}
	*size = 0; return NULL;
}
/* byteswap an array of records in place per a layout string: 2 = 16-bit, 4 = 32-bit, 1 = byte */
static void swap(u8 *p, u32 count, const char *layout) {
	for (u32 r = 0; r < count; r++)
		for (const char *c = layout; *c; c++) {
			if (*c == '2') { u8 t = p[0]; p[0] = p[1]; p[1] = t; p += 2; }
			else if (*c == '4') { u8 t = p[0]; p[0] = p[3]; p[3] = t; t = p[1]; p[1] = p[2]; p[2] = t; p += 4; }
			else p += 1;
		}
}
static int load(Pack *P, const char *path) {
	FILE *f = fopen(path, "rb"); if (!f) return -1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	file = malloc(n); fread(file, 1, n, f); fclose(f);
	memset(P, 0, sizeof(*P)); u32 sz; u8 *c;
	c = chunk("PLAN", &sz); swap(c, sz / 16, "4444"); P->planes = (void *)c; P->numPlanes = sz / 16;
	c = chunk("NODE", &sz); swap(c, sz / 12, "444"); P->nodes = (void *)c; P->numNodes = sz / 12;
	c = chunk("LEAF", &sz); swap(c, sz / 28, "224422222222"); P->leafs = (void *)c; P->numLeafs = sz / 28;
	c = chunk("BRSH", &sz); swap(c, sz / 12, "444"); P->brushes = (void *)c; P->numBrushes = sz / 12;
	c = chunk("BSID", &sz); swap(c, sz / 4, "22"); P->brushSides = (void *)c; P->numBrushSides = sz / 4;
	c = chunk("LBRS", &sz); swap(c, sz / 2, "2"); P->leafBrushes = (void *)c; P->numLeafBrushes = sz / 2;
	c = chunk("VERT", &sz); swap(c, sz / 32, "44444441111"); P->verts = (void *)c; P->numVerts = sz / 32;
	c = chunk("MATS", &sz); if (c) { swap(c, 1, "4"); P->numMats = *(u32 *)c; swap(c + 4, P->numMats, "221111"); P->mats = (void *)(c + 4); }
	c = chunk("FACE", &sz); if (c) { swap(c, 1, "4"); P->numFaces = *(u32 *)c; swap(c + 4, P->numFaces, "422211"); P->faces = (void *)(c + 4); }
	c = chunk("BMDL", &sz); if (c) { swap(c, 1, "4"); P->numBModels = *(u32 *)c; swap(c + 4, P->numBModels, "444444444"); P->bmodels = (void *)(c + 4); }
	c = chunk("BMFC", &sz); swap(c, sz / 2, "2"); P->bmodelFaces = (void *)c;
	c = chunk("MDLS", &sz); if (c) { swap(c, 1, "4"); P->numModels = *(u32 *)c; swap(c + 4, P->numModels, "444444444444"); P->models = (void *)(c + 4); }
	c = chunk("MMSH", &sz); if (c) { swap(c, sz / 12, "4422"); P->meshes = (void *)c; }
	c = chunk("MVTX", &sz); if (c) { swap(c, sz / 14, "2222211111"); P->mverts = (void *)c; }
	c = chunk("MIDX", &sz); if (c) { swap(c, sz / 2, "2"); P->mindices = (void *)c; }
	c = chunk("MHUL", &sz); if (c) { swap(c, sz / 32, "44444444"); P->hulls = (void *)c; }
	c = chunk("MPLN", &sz); if (c) { swap(c, sz / 16, "4444"); P->hullPlanes = (void *)c; }
	c = chunk("SPRP", &sz); if (c) { swap(c, 1, "4"); P->numStaticProps = *(u32 *)c; swap(c + 4, P->numStaticProps, "2114444441111" "22"); P->staticProps = (void *)(c + 4); }
	c = chunk("ENTS", &sz); if (c) { swap(c, 1, "4"); P->numEntities = *(u32 *)c; swap(c + 4, P->numEntities, "44"); P->entities = (void *)(c + 4); }
	c = chunk("EKVS", &sz); if (c) { swap(c, sz / 8, "44"); P->kvs = (void *)c; }
	c = chunk("ESTR", &sz); if (c) P->entStrings = (const char *)c;
	c = chunk("TRIG", &sz); if (c) { swap(c, 1, "4"); P->numTriggers = *(u32 *)c; swap(c + 4, P->numTriggers, "444444" "1111111111111111111111111111111111111111111111111111111111111111"); P->triggers = (void *)(c + 4); }
	c = chunk("LMRK", &sz); if (c) { swap(c, 1, "4"); P->numLandmarks = *(u32 *)c;
		for (u32 i = 0; i < P->numLandmarks; i++) swap(c + 4 + i * 44 + 32, 1, "444"); P->landmarks = (void *)(c + 4); }
	c = chunk("NSKL", &sz); if (c) { swap(c, 1, "4"); P->numSkels = *(u32 *)c; swap(c + 4, P->numSkels, "22442222"); P->skels = (void *)(c + 4); }
	c = chunk("NBON", &sz); if (c) { swap(c, sz / 52, "4444444444444"); P->npcBones = (void *)c; }
	c = chunk("NSEQ", &sz); if (c) { swap(c, sz / 32, "4224442244"); P->npcSeqs = (void *)c; }
	c = chunk("NSQM", &sz); if (c) { swap(c, sz / 4, "4"); P->npcSeqMove = (void *)c; P->numNpcSeqMove = sz / 16; }
	c = chunk("NMSK", &sz); if (c) P->npcMasks = (void *)c;
	c = chunk("NLAY", &sz); if (c) { swap(c, sz / 20, "224444"); P->npcLayers = (void *)c; }
	c = chunk("NRST", &sz); if (c) { swap(c, sz / 2, "2"); P->npcRest = (void *)c; }
	c = chunk("NSKX", &sz); if (c) { swap(c, sz / 2, "2"); P->skelLook = (void *)c; }
	c = chunk("NPPI", &sz); if (c) { swap(c, sz / 2, "2"); P->skelAim = (void *)c; P->numSkelAim = sz / 4; }
	c = chunk("NPPC", &sz); if (c) { swap(c, sz / 20, "211112444"); P->aimCtls = (void *)c; }
	c = chunk("NPPQ", &sz); if (c) { swap(c, sz / 4, "4"); P->aimQuats = (void *)c; }
	c = chunk("NPRI", &sz); if (c) { swap(c, sz / 2, "2"); P->skelRag = (void *)c; P->numSkelRag = sz / 4; }
	c = chunk("NPRR", &sz); if (c) { swap(c, sz / 12, "2244"); P->ragSolids = (void *)c; }
	c = chunk("NPRL", &sz); if (c) { swap(c, sz / 4, "4"); P->ragLimits = (void *)c; }
	c = chunk("NSQS", &sz); if (c) { swap(c, 1, "4"); P->numSeqSounds = *(u32 *)c; swap(c + 4, P->numSeqSounds, "4422"); P->seqSounds = (void *)(c + 4); }
	c = chunk("SSCP", &sz); if (c) { swap(c, 3, "4"); u32 *h = (u32 *)c; P->numScapes = h[0]; P->numScapeElems = h[1]; P->numScapeWaves = h[2];
		P->scapes = (void *)(c + 12); swap(c + 12, P->numScapes, "1124");
		P->scapeElems = (void *)(c + 12 + P->numScapes * 8); swap((u8 *)P->scapeElems, P->numScapeElems, "1111224444");
		P->scapeWaves = (void *)((u8 *)P->scapeElems + P->numScapeElems * 24); swap((u8 *)P->scapeWaves, P->numScapeWaves, "2"); }
	c = chunk("NSQE", &sz); if (c) { swap(c, 1, "4"); P->numSeqEvents = *(u32 *)c; swap(c + 4, P->numSeqEvents, "4422"); P->seqEvents = (void *)(c + 4); }
	c = chunk("AINN", &sz); if (c) { swap(c, 1, "4"); P->numNavNodes = *(u32 *)c; swap(c + 4, P->numNavNodes, "44411222"); P->navNodes = (void *)(c + 4); }
	c = chunk("AINL", &sz); if (c) { swap(c, sz / 4, "211"); P->navLinks = (void *)c; }
	c = chunk("SEVT", &sz); if (c) { swap(c, sz / 20, "11222222222"); P->eventTags = (void *)c; }
	c = chunk("NFRM", &sz); if (c) { swap(c, sz / 2, "2"); P->npcFrames = (void *)c; }
	c = chunk("NMDL", &sz); if (c) { swap(c, 1, "4"); P->numNpcModels = *(u32 *)c; swap(c + 4, P->numNpcModels, "2244444"); P->npcModels = (void *)(c + 4); }
	c = chunk("NBAT", &sz); if (c) { swap(c, sz / 32, "42244" "1111111111111111"); P->npcBatches = (void *)c; }
	c = chunk("NEYI", &sz); if (c) { swap(c, sz / 2, "2"); P->eyeIndex = (void *)c; }
	c = chunk("NEYE", &sz); if (c) { swap(c, sz / 48, "444444444444"); P->eyes = (void *)c; }
	c = chunk("SCAP", &sz); if (c) { swap(c, sz / 4, "4"); P->soundCaptions = (void *)c; }
	c = chunk("LFAC", &sz); if (c) { swap(c, sz / 2, "2"); P->leafFaces = (void *)c; }
	c = chunk("MSTP", &sz); if (c) { P->matSurf = c; P->numMatSurf = sz; }
	c = chunk("SPCB", &sz); if (c) P->staticCubes = (void *)c;
	c = chunk("LCUB", &sz); if (c) { swap(c, 1, "4"); P->numLeafCubes = *(u32 *)c; P->leafCubes = (void *)(c + 4); }
	c = chunk("SPLO", &sz); if (c) { swap(c, sz / 4, "4"); P->staticLightOrg = (void *)c; }
	P->spClusterRange = NULL; P->spClusters = NULL; P->numSpClusters = 0;
	c = chunk("SPCL", &sz);
	if (c && sz >= 8) { swap(c, 2, "4"); u32 np_ = *(u32 *)c, nc = *(u32 *)(c + 4); swap(c + 8, np_ * 2 + nc, "2");
		if (np_ == P->numStaticProps) { P->spClusterRange = (void *)(c + 8); P->spClusters = (void *)(c + 8 + np_ * 4); P->numSpClusters = nc; } }
	c = chunk("WLIT", &sz); if (c) { swap(c, 1, "4"); P->numWorldLights = *(u32 *)c; swap(c + 4, P->numWorldLights * 19, "4"); P->worldLights = (void *)(c + 4); }
	c = chunk("MFXH", &sz); if (c) { swap(c, 1, "4"); P->numMatFx = *(u32 *)c; swap(c + 4, P->numMatFx, "211222222"); P->matFx = (void *)(c + 4); }
	c = chunk("MFXO", &sz); if (c) { swap(c, sz / 16, "111111244"); P->fxOps = (void *)c; P->numFxOps = sz / 16; }
	c = chunk("MFXV", &sz); if (c) { swap(c, sz / 4, "4"); P->fxVars = (void *)c; P->numFxVars = sz / 4; }
	c = chunk("LSTS", &sz); if (c) P->lightStyles = (void *)c;
	c = chunk("LSTP", &sz); if (c) { swap(c, 1, "4"); P->numStylePages = *(u32 *)c; swap(c + 4, P->numStylePages, "2114"); P->stylePages = (void *)(c + 4); }
	c = chunk("LSTF", &sz); if (c) { swap(c, 1, "4"); P->numStyleFaces = *(u32 *)c; swap(c + 4, P->numStyleFaces, "211"); P->styleFaces = (void *)(c + 4); }
	c = chunk("MFAR", &sz); if (c) { swap(c, sz / 2, "2"); P->modelFar = (void *)c; P->numModelFar = sz / 2; }
	c = chunk("MSRF", &sz); if (c) { swap(c, sz / 2, "2"); P->modelSurf = (void *)c; P->numModelSurf = sz / 2; }
	c = chunk("WATR", &sz); if (c) { swap(c, 1, "4"); P->numWater = *(u32 *)c; swap(c + 4, P->numWater, "22" "11111111" "44"); P->water = (void *)(c + 4); }
	c = chunk("GIBS", &sz); if (c) { swap(c, 1, "4"); P->numGibSets = *(u32 *)c; swap(c + 4, P->numGibSets, "22224"); P->gibSets = (void *)(c + 4); }
	c = chunk("GIBL", &sz); if (c) { swap(c, sz / 2, "2"); P->gibModels = (void *)c; }
	c = chunk("NVTX", &sz); if (c) { swap(c, sz / 20, "444221111"); P->npcVerts = (void *)c; }
	c = chunk("NIDX", &sz); if (c) { swap(c, sz / 2, "2"); P->npcIndices = (void *)c; }
	c = chunk("SCEN", &sz); if (c) { swap(c, 1, "4"); P->numScenes = *(u32 *)c; swap(c + 4, P->numScenes, "42244"); P->scenes = (void *)(c + 4); }
	c = chunk("SCEV", &sz); if (c) { swap(c, sz / 32, "1124444444"); P->sceneEvents = (void *)c; }
	c = chunk("SNDS", &sz); if (c) { swap(c, 1, "4"); P->numSounds = *(u32 *)c; swap(c + 4, P->numSounds, "444"); P->sounds = (void *)(c + 4); }
	c = chunk("NFMD", &sz); if (c) { swap(c, sz / 32, "422444444"); P->npcFaces = (void *)c; }
	c = chunk("NFCT", &sz); if (c) { swap(c, sz / 12, "444"); P->flexCtls = (void *)c; }
	c = chunk("NFRL", &sz); if (c) { swap(c, sz / 8, "224"); P->flexRules = (void *)c; }
	c = chunk("NFOP", &sz); if (c) { swap(c, sz / 8, "44"); P->flexOps = (void *)c; }
	c = chunk("NFLX", &sz); if (c) { swap(c, sz / 28, "22444444"); P->flexes = (void *)c; }
	c = chunk("NFVA", &sz); if (c) { swap(c, sz / 12, "422211"); P->flexVAs = (void *)c; }
	c = chunk("NATT", &sz); if (c) { swap(c, sz / 56, "44444444444444"); P->attachments = (void *)c; }
	c = chunk("PHON", &sz); if (c) { swap(c, 1, "22"); P->numPhonKeys = *(u16 *)c; P->numPhonemes = *(u16 *)(c + 2); swap(c + 4, (sz - 4) / 4, "4"); P->phonKeys = (void *)(c + 4); P->phonemes = c + 4 + P->numPhonKeys * 4; }
	c = chunk("SPHI", &sz); if (c) { swap(c, sz / 8, "44"); P->soundPhon = (void *)c; }
	c = chunk("SPHR", &sz); if (c) { swap(c, sz / 12, "2244"); P->phonemeRecs = (void *)c; }
	c = chunk("SFEV", &sz); if (c) { swap(c, sz / 8, "422"); P->eventFlex = (void *)c; }
	c = chunk("SFTR", &sz); if (c) { swap(c, sz / 20, "44444"); P->flexTracks = (void *)c; }
	c = chunk("SFSM", &sz); if (c) { swap(c, sz / 8, "44"); P->flexSamples = (void *)c; }
	c = chunk("SPWN", &sz); if (c) { swap(c, 1, "444444"); memcpy(P->spawnOrigin, c, 12); memcpy(P->spawnAngles, c + 12, 12); }
	return 0;
}

static Pack P;
static int bad_position(const Player *pl) {
	/* centre of the hull must not be inside solid */
	f32 mn[3], mx[3]; Player_Bounds(pl, mn, mx);
	f32 c[3] = {pl->origin[0], pl->origin[1], pl->origin[2] + (mx[2]) * 0.5f};
	return (CM_PointContents(c) & MASK_PLAYERSOLID) != 0;
}
static void run(Player *pl, UserCmd cmd, int ticks, const char *what) {
	f32 s[3]; memcpy(s, pl->origin, 12); f32 maxz = pl->origin[2]; f32 vmax = 0;
	for (int i = 0; i < ticks; i++) { Player_Tick(pl, &cmd); if (pl->origin[2] > maxz) maxz = pl->origin[2];
		f32 v = sqrtf(pl->velocity[0]*pl->velocity[0]+pl->velocity[1]*pl->velocity[1]); if (v > vmax) vmax = v; }
	f32 d = sqrtf((pl->origin[0]-s[0])*(pl->origin[0]-s[0]) + (pl->origin[1]-s[1])*(pl->origin[1]-s[1]));
	printf("  %-28s -> (%8.1f %8.1f %7.1f) moved %6.1f  dz %+6.1f  peak %+5.1f  ground %d  top speed %5.1f %s\n", what,
	       pl->origin[0], pl->origin[1], pl->origin[2], d, pl->origin[2]-s[2], maxz-s[2], pl->onGround,
	       vmax, bad_position(pl) ? "IN SOLID!" : "");
}

static int cluster_at(const Pack *p, const Player *pl) {
	f32 c[3] = {pl->origin[0], pl->origin[1], pl->origin[2] + 36}; s32 node = 0;
	while (node >= 0) { const PackNode *n = &p->nodes[node]; const PackPlane *q = &p->planes[n->plane];
		node = n->children[(c[0]*q->n[0]+c[1]*q->n[1]+c[2]*q->n[2]-q->d) >= 0 ? 0 : 1]; }
	return p->leafs[-1 - node].cluster;
}
static const PackLandmark *landmark(const Pack *p, const char *n) {
	for (u32 i = 0; i < p->numLandmarks; i++) if (!strcmp(p->landmarks[i].name, n)) return &p->landmarks[i];
	return NULL;
}
/* For every touchable level exit in every map: walk into it, carry the landmark offset over,
 * load the destination, and check the arrival point is valid. */
static int transit(const char *dir) {
	static const char *maps[] = {MAPLIST};
	int total = 0, ok = 0;
	for (unsigned m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
		char path[256]; snprintf(path, sizeof path, "%s/%s.hgm", dir, maps[m]);
		Pack A; if (load(&A, path)) { printf("%s: missing\n", maps[m]); continue; }
		for (u32 t = 0; t < A.numTriggers; t++) {
			PackTrigger tr = A.triggers[t]; total++;
			const PackLandmark *la = landmark(&A, tr.landmark);
			/* stand at the trigger's centre, on its floor */
			f32 pos[3] = {(tr.mins[0] + tr.maxs[0]) / 2, (tr.mins[1] + tr.maxs[1]) / 2, tr.mins[2] + 1};
			snprintf(path, sizeof path, "%s/%s.hgm", dir, tr.map);
			Pack B;
			if (!la) { printf("  %-20s -> %-20s landmark '%s' missing in source\n", maps[m], tr.map, tr.landmark); continue; }
			if (load(&B, path)) { printf("  %-20s -> %-20s destination pack missing\n", maps[m], tr.map); continue; }
			const PackLandmark *lb = landmark(&B, tr.landmark);
			if (!lb) { printf("  %-20s -> %-20s landmark '%s' missing in destination\n", maps[m], tr.map, tr.landmark); continue; }
			CM_Init(&B);
			f32 dst[3]; for (int i = 0; i < 3; i++) dst[i] = lb->origin[i] + (pos[i] - la->origin[i]);
			Player pl; int placed = Player_Spawn(&pl, dst, 0, 0) && cluster_at(&B, &pl) >= 0; const char *how = "relative";
			if (!placed) { placed = Player_Spawn(&pl, lb->origin, 0, 0) && cluster_at(&B, &pl) >= 0; how = "landmark"; }
			if (!placed) { Player_Spawn(&pl, B.spawnOrigin, 0, 0); how = "map spawn"; }
			f32 nudged = pl.origin[2] - dst[2]; (void)how;
			UserCmd idle = {0, 0, 0, 0, 0};
			for (int i = 0; i < 133; i++) Player_Tick(&pl, &idle);
			int solid = bad_position(&pl);
			int good = !solid && pl.onGround;
			ok += good;
			if (strcmp(how, "relative")) printf("  (fallback: %s)", how);
			printf("  %-20s -> %-20s via %-28s arrive (%7.0f %7.0f %6.0f) nudged %+4.0f settled dz %+6.1f ground %d %s\n",
			       maps[m], tr.map, tr.landmark, dst[0], dst[1], dst[2], nudged, pl.origin[2] - dst[2], pl.onGround,
			       good ? "ok" : (solid ? "IN SOLID" : "CHECK"));
		}
	}
	printf("%d/%d transitions arrive standing on solid ground\n", ok, total);
	return 0;
}

int main(int argc, char **argv) {
	if (!strcmp(argv[1], "transit")) return transit(argv[2]);
	if (load(&P, argv[1])) { printf("load failed\n"); return 1; }
	CM_Init(&P);
	printf("%s: %u brushes, %u brush models\n", argv[1], P.numBrushes, P.numBModels);
	Player pl;
	f32 sp[3] = {P.spawnOrigin[0], P.spawnOrigin[1], P.spawnOrigin[2]};
	if (argc >= 5) { sp[0] = atof(argv[2]); sp[1] = atof(argv[3]); sp[2] = atof(argv[4]); }
	f32 yaw = argc >= 6 ? atof(argv[5]) : P.spawnAngles[1];
	Player_Spawn(&pl, sp, 0, yaw);
	printf("spawn (%.1f %.1f %.1f) -> (%.1f %.1f %.1f) ground %d\n", sp[0], sp[1], sp[2], pl.origin[0], pl.origin[1], pl.origin[2], pl.onGround);
	UserCmd idle = {0, 0, 0, 0, yaw};
	run(&pl, idle, 133, "stand 2s");
	UserCmd fw = {1, 0, 0, 0, yaw};
	run(&pl, fw, 200, "run forward 3s");
	UserCmd sprint = fw; sprint.buttons = IN_SPEED;
	run(&pl, sprint, 133, "sprint 2s");
	UserCmd jump = idle; jump.buttons = IN_JUMP; jump.yaw = pl.yaw;
	run(&pl, idle, 66, "stop");
	run(&pl, jump, 60, "jump (hold)");
	UserCmd crouch = idle; crouch.buttons = IN_DUCK;
	run(&pl, crouch, 60, "crouch");
	UserCmd cw = fw; cw.buttons = IN_DUCK;
	run(&pl, cw, 133, "crouch-walk 2s");
	run(&pl, idle, 60, "stand up");
	for (int a = 0; a < 360; a += 90) {
		UserCmd w = {1, 0, 0, 0, (f32)a};
		char name[64]; snprintf(name, sizeof name, "run yaw %d for 5s", a);
		run(&pl, w, 333, name);
	}
	/* long random walk: count ticks spent inside solid or below the world */
	srand(1234);
	int bad = 0, falls = 0; UserCmd r = fw; clock_t t0 = clock(); long ticks = 0;
	f32 minz = 1e9;
	for (int s = 0; s < 400; s++) {
		r.yaw = rand() % 360; r.forwardmove = (rand() % 3 - 1); r.sidemove = (rand() % 3 - 1);
		r.buttons = (rand() % 4 == 0 ? IN_JUMP : 0) | (rand() % 6 == 0 ? IN_DUCK : 0) | (rand() % 3 == 0 ? IN_SPEED : 0);
		for (int i = 0; i < 40; i++) { Player_Tick(&pl, &r); ticks++; if (bad_position(&pl)) bad++; if (pl.origin[2] < minz) minz = pl.origin[2]; }
		if (pl.origin[2] < -15000) { falls++; Player_Spawn(&pl, sp, 0, yaw); }
	}
	double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	printf("random walk: %ld ticks, %d ticks inside solid, %d falls out of world, lowest z %.1f, %.3f ms/tick on host\n",
	       ticks, bad, falls, minz, ms / ticks);
	return 0;
}
