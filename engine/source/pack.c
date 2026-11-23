#include "pack.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <ogc/cache.h>

/* compile-time layout checks: these structs are read straight out of the file */
typedef char check_texinfo[(sizeof(PackTexInfo) == 16) ? 1 : -1];
typedef char check_mat[(sizeof(PackMat) == 8) ? 1 : -1];
typedef char check_vert[(sizeof(PackVert) == 32) ? 1 : -1];
typedef char check_face[(sizeof(PackFace) == 12) ? 1 : -1];
typedef char check_leaf[(sizeof(PackLeaf) == 28) ? 1 : -1];
typedef char check_brush[(sizeof(PackBrush) == 12) ? 1 : -1];
typedef char check_side[(sizeof(PackBrushSide) == 4) ? 1 : -1];
typedef char check_trigger[(sizeof(PackTrigger) == 88) ? 1 : -1];
typedef char check_landmark[(sizeof(PackLandmark) == 44) ? 1 : -1];
typedef char check_model[(sizeof(PackModel) == 48) ? 1 : -1];
typedef char check_bmodel[(sizeof(PackBModel) == 36) ? 1 : -1];
typedef char check_mesh[(sizeof(PackMesh) == 12) ? 1 : -1];
typedef char check_mvert[(sizeof(PackMVert) == 14) ? 1 : -1];
typedef char check_wlit[(sizeof(PackWorldLight) == 76) ? 1 : -1];
typedef char check_mfx[(sizeof(PackMatFx) == 16 && sizeof(PackFxOp) == 16) ? 1 : -1];
typedef char check_lsts[(sizeof(PackLightStyle) == 32 && sizeof(PackStylePage) == 8 && sizeof(PackStyleFace) == 4) ? 1 : -1];
typedef char check_hull[(sizeof(PackHull) == 32) ? 1 : -1];
typedef char check_sprop[(sizeof(PackStaticProp) == 36) ? 1 : -1];
typedef char check_ent[(sizeof(PackEntity) == 8) ? 1 : -1];
typedef char check_kv[(sizeof(PackKV) == 8) ? 1 : -1];
typedef char check_skel[(sizeof(PackSkel) == 20) ? 1 : -1];
typedef char check_bone[(sizeof(PackBone) == 52) ? 1 : -1];
typedef char check_seq[(sizeof(PackSeq) == 32) ? 1 : -1];
typedef char check_smask[(sizeof(PackSeqMask) == 2) ? 1 : -1];
typedef char check_slay[(sizeof(PackSeqLayer) == 20) ? 1 : -1];
typedef char check_slook[(sizeof(PackSkelLook) == 8) ? 1 : -1];
typedef char check_sqev[(sizeof(PackSeqEvent) == 12) ? 1 : -1];
typedef char check_navn[(sizeof(PackNavNode) == 20) ? 1 : -1];
typedef char check_navl[(sizeof(PackNavLink) == 4) ? 1 : -1];
typedef char check_etag[(sizeof(PackEventTags) == 20) ? 1 : -1];
typedef char check_bframe[(sizeof(PackBoneFrame) == 12) ? 1 : -1];
typedef char check_nmdl[(sizeof(PackNpcModel) == 24) ? 1 : -1];
typedef char check_nbat[(sizeof(PackNpcBatch) == 32) ? 1 : -1];
typedef char check_nvert[(sizeof(PackNpcVert) == 20) ? 1 : -1];
typedef char check_scene[(sizeof(PackScene) == 16) ? 1 : -1];
typedef char check_sev[(sizeof(PackSceneEvent) == 32) ? 1 : -1];
typedef char check_snd[(sizeof(PackSound) == 12) ? 1 : -1];
typedef char check_face[(sizeof(PackNpcFace) == 32) ? 1 : -1];
typedef char check_fct[(sizeof(PackFlexCtl) == 12) ? 1 : -1];
typedef char check_frl[(sizeof(PackFlexRule) == 8) ? 1 : -1];
typedef char check_flx[(sizeof(PackFlex) == 28) ? 1 : -1];
typedef char check_fva[(sizeof(PackFlexVA) == 12) ? 1 : -1];
typedef char check_att[(sizeof(PackAttachment) == 56) ? 1 : -1];
typedef char check_sph[(sizeof(PackPhoneme) == 12) ? 1 : -1];
typedef char check_ftr[(sizeof(PackFlexTrack) == 20) ? 1 : -1];

typedef struct {
	char tag[4];
	u32 offset, size;
} ChunkEntry;

static const ChunkEntry *find_chunk(const Pack *p, const char *tag)
{
	u32 n = *(u32 *)(p->base + 8);
	const ChunkEntry *c = (const ChunkEntry *)(p->base + 16);
	for (u32 i = 0; i < n; i++)
		if (memcmp(c[i].tag, tag, 4) == 0)
			return &c[i];
	return NULL;
}

static void *chunk_ptr(const Pack *p, const char *tag, u32 *size)
{
	const ChunkEntry *c = find_chunk(p, tag);
	if (!c) {
		if (size)
			*size = 0;
		return NULL;
	}
	if (size)
		*size = c->size;
	return p->base + c->offset;
}

static int parse(Pack *p);

void *Pack_Chunk(const Pack *p, const char *tag, u32 *size)
{
	return chunk_ptr(p, tag, size);
}

int Pack_LoadMemory(Pack *p, u8 *data, u32 size)
{
	memset(p, 0, sizeof(*p));
	p->base = data;
	p->size = size;
	return parse(p);
}

int Pack_Load(Pack *p, const char *path)
{
	memset(p, 0, sizeof(*p));
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("cannot open %s\n", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	p->base = memalign(32, (len + 31) & ~31);
	if (!p->base) {
		printf("out of memory loading %s (%ld KB)\n", path, len / 1024);
		fclose(f);
		return -1;
	}
	printf("loading %s (%ld KB)...\n", path, len / 1024);
	size_t got = 0;
	while (got < (size_t)len) {
		size_t r = fread(p->base + got, 1, 256 * 1024, f);
		if (r == 0)
			break;
		got += r;
	}
	fclose(f);
	if (got != (size_t)len) {
		printf("short read (%u of %ld)\n", (unsigned)got, len);
		return -1;
	}
	p->size = len;
	return parse(p);
}

static int parse(Pack *p)
{
	if (p->size < 16 || memcmp(p->base, "HGM1", 4) != 0)
		return -3; /* not a map pack (or a damaged one) */
	if (*(u32 *)(p->base + 4) != PACK_VERSION)
		return -2; /* from another converter version */

	u32 sz;
	u8 *c;

	c = chunk_ptr(p, "TEXI", &sz);
	p->numTex = c ? *(u32 *)c : 0;
	p->tex = c ? (PackTexInfo *)(c + 4) : NULL;
	p->texData = chunk_ptr(p, "TEXD", NULL);

	c = chunk_ptr(p, "MATS", &sz);
	if (c) {
		p->numMats = *(u32 *)c;
		p->mats = (PackMat *)(c + 4);
	}

	p->verts = chunk_ptr(p, "VERT", &sz);
	p->numVerts = sz / sizeof(PackVert);

	c = chunk_ptr(p, "FACE", &sz);
	if (c) { /* packs without a world (weapons.hgm) have no faces */
		p->numFaces = *(u32 *)c;
		p->faces = (PackFace *)(c + 4);
	}

	p->planes = chunk_ptr(p, "PLAN", &sz);
	p->numPlanes = sz / sizeof(PackPlane);
	p->nodes = chunk_ptr(p, "NODE", &sz);
	p->numNodes = sz / sizeof(PackNode);
	p->leafs = chunk_ptr(p, "LEAF", &sz);
	p->numLeafs = sz / sizeof(PackLeaf);
	p->leafFaces = chunk_ptr(p, "LFAC", &sz);
	p->numLeafFaces = sz / 2;

	p->brushes = chunk_ptr(p, "BRSH", &sz);
	p->numBrushes = sz / sizeof(PackBrush);
	p->brushSides = chunk_ptr(p, "BSID", &sz);
	p->numBrushSides = sz / sizeof(PackBrushSide);
	p->leafBrushes = chunk_ptr(p, "LBRS", &sz);
	p->numLeafBrushes = sz / 2;
	c = chunk_ptr(p, "BMDL", &sz);
	if (c) {
		p->numBModels = *(u32 *)c;
		p->bmodels = (PackBModel *)(c + 4);
	}
	p->bmodelFaces = chunk_ptr(p, "BMFC", NULL);

	c = chunk_ptr(p, "MDLS", &sz);
	if (c) {
		p->numModels = *(u32 *)c;
		p->models = (PackModel *)(c + 4);
	}
	p->meshes = chunk_ptr(p, "MMSH", NULL);
	p->mverts = chunk_ptr(p, "MVTX", NULL);
	p->mindices = chunk_ptr(p, "MIDX", NULL);
	p->hulls = chunk_ptr(p, "MHUL", NULL);
	p->hullPlanes = chunk_ptr(p, "MPLN", NULL);
	c = chunk_ptr(p, "SPRP", &sz);
	if (c) {
		p->numStaticProps = *(u32 *)c;
		p->staticProps = (PackStaticProp *)(c + 4);
	}

	c = chunk_ptr(p, "ENTS", &sz);
	if (c) {
		p->numEntities = *(u32 *)c;
		p->entities = (PackEntity *)(c + 4);
	}
	p->kvs = chunk_ptr(p, "EKVS", NULL);
	p->entStrings = chunk_ptr(p, "ESTR", NULL);
	c = chunk_ptr(p, "TRIG", &sz);
	if (c) {
		p->numTriggers = *(u32 *)c;
		p->triggers = (PackTrigger *)(c + 4);
	}
	c = chunk_ptr(p, "LMRK", &sz);
	if (c) {
		p->numLandmarks = *(u32 *)c;
		p->landmarks = (PackLandmark *)(c + 4);
	}

	c = chunk_ptr(p, "VISD", &sz);
	if (c) {
		p->numClusters = *(u32 *)c;
		p->visOffsets = (u32 *)(c + 4);
		p->visBase = c + 4 + 4 * p->numClusters;
	}


	c = chunk_ptr(p, "NSKL", &sz);
	if (c) {
		p->numSkels = *(u32 *)c;
		p->skels = (PackSkel *)(c + 4);
	}
	p->npcBones = chunk_ptr(p, "NBON", NULL);
	p->npcSeqs = chunk_ptr(p, "NSEQ", NULL);
	{
		u32 msz;
		const u8 *mc = chunk_ptr(p, "NSQM", &msz);
		p->npcSeqMove = mc ? (const f32(*)[4])mc : NULL;
		p->numNpcSeqMove = mc ? msz / 16 : 0;
	}
	p->npcFrames = chunk_ptr(p, "NFRM", NULL);
	p->npcMasks = chunk_ptr(p, "NMSK", NULL);
	p->npcLayers = chunk_ptr(p, "NLAY", NULL);
	p->npcRest = chunk_ptr(p, "NRST", NULL);
	p->skelLook = chunk_ptr(p, "NSKX", NULL);
	{
		u32 asz = 0;
		p->skelAim = chunk_ptr(p, "NPPI", &asz);
		p->numSkelAim = p->skelAim ? asz / 4 : 0;
		p->aimCtls = chunk_ptr(p, "NPPC", NULL);
		p->aimQuats = chunk_ptr(p, "NPPQ", NULL);
		u32 rsz = 0;
		p->skelRag = chunk_ptr(p, "NPRI", &rsz);
		p->numSkelRag = p->skelRag ? rsz / 4 : 0;
		p->ragSolids = chunk_ptr(p, "NPRR", NULL);
		p->ragLimits = chunk_ptr(p, "NPRL", NULL);
	}
	p->eventTags = chunk_ptr(p, "SEVT", NULL);
	c = chunk_ptr(p, "NSQE", &sz);
	if (c) {
		p->numSeqEvents = *(u32 *)c;
		p->seqEvents = (PackSeqEvent *)(c + 4);
	}
	c = chunk_ptr(p, "NSQS", &sz);
	if (c) {
		p->numSeqSounds = *(u32 *)c;
		p->seqSounds = (PackSeqEvent *)(c + 4);
	}
	c = chunk_ptr(p, "SSCP", &sz);
	if (c && sz >= 12) {
		p->numScapes = ((u32 *)c)[0];
		p->numScapeElems = ((u32 *)c)[1];
		p->numScapeWaves = ((u32 *)c)[2];
		p->scapes = (PackScape *)(c + 12);
		p->scapeElems = (PackScapeElem *)(c + 12 + p->numScapes * sizeof(PackScape));
		p->scapeWaves = (s16 *)((u8 *)p->scapeElems + p->numScapeElems * sizeof(PackScapeElem));
	}
	c = chunk_ptr(p, "AINN", &sz);
	if (c) {
		p->numNavNodes = *(u32 *)c;
		p->navNodes = (PackNavNode *)(c + 4);
	}
	p->navLinks = chunk_ptr(p, "AINL", NULL);
	c = chunk_ptr(p, "NMDL", &sz);
	if (c) {
		p->numNpcModels = *(u32 *)c;
		p->npcModels = (PackNpcModel *)(c + 4);
	}
	p->npcBatches = chunk_ptr(p, "NBAT", NULL);
	p->npcVerts = chunk_ptr(p, "NVTX", NULL);
	p->npcIndices = chunk_ptr(p, "NIDX", NULL);
	p->npcFaces = chunk_ptr(p, "NFMD", NULL);
	p->flexCtls = chunk_ptr(p, "NFCT", NULL);
	p->flexRules = chunk_ptr(p, "NFRL", NULL);
	p->flexOps = chunk_ptr(p, "NFOP", NULL);
	p->flexes = chunk_ptr(p, "NFLX", NULL);
	p->flexVAs = chunk_ptr(p, "NFVA", NULL);
	p->attachments = chunk_ptr(p, "NATT", NULL);
	p->eyeIndex = chunk_ptr(p, "NEYI", NULL);
	c = chunk_ptr(p, "GIBS", &sz);
	if (c) {
		p->numGibSets = *(u32 *)c;
		p->gibSets = (PackGibSet *)(c + 4);
	}
	p->gibModels = chunk_ptr(p, "GIBL", NULL);
	c = chunk_ptr(p, "MSTP", &sz);
	if (c) {
		p->matSurf = c;
		p->numMatSurf = sz;
	}
	c = chunk_ptr(p, "SPCB", &sz);
	p->staticCubes = (c && sz >= p->numStaticProps * 18) ? (const u8(*)[18])c : NULL;
	c = chunk_ptr(p, "LCUB", &sz);
	if (c && sz >= 4) {
		p->numLeafCubes = *(u32 *)c;
		p->leafCubes = (const u8(*)[18])(c + 4);
	}
	c = chunk_ptr(p, "SPLO", &sz);
	p->staticLightOrg = (c && sz >= p->numStaticProps * 12) ? (const f32(*)[3])c : NULL;
	c = chunk_ptr(p, "SPCL", &sz);
	p->spClusterRange = NULL;
	p->spClusters = NULL;
	p->numSpClusters = 0;
	if (c && sz >= 8 && *(u32 *)c == p->numStaticProps) {
		u32 nc = *(u32 *)(c + 4);
		if (8 + p->numStaticProps * 4 + nc * 2 <= sz) {
			p->spClusterRange = (const u16(*)[2])(c + 8);
			p->spClusters = (const u16 *)(c + 8 + p->numStaticProps * 4);
			p->numSpClusters = nc;
		}
	}
	c = chunk_ptr(p, "WLIT", &sz);
	if (c && sz >= 4 && sz >= 4 + *(u32 *)c * sizeof(PackWorldLight)) {
		p->numWorldLights = *(u32 *)c;
		p->worldLights = (const PackWorldLight *)(c + 4);
	}
	c = chunk_ptr(p, "MFXH", &sz);
	if (c && sz >= 4 && sz >= 4 + *(u32 *)c * sizeof(PackMatFx)) {
		p->numMatFx = *(u32 *)c;
		p->matFx = (const PackMatFx *)(c + 4);
		p->fxOps = (const PackFxOp *)chunk_ptr(p, "MFXO", &sz);
		p->numFxOps = p->fxOps ? sz / sizeof(PackFxOp) : 0;
		p->fxVars = (const f32 *)chunk_ptr(p, "MFXV", &sz);
		p->numFxVars = p->fxVars ? sz / 4 : 0;
		if (!p->fxOps || !p->fxVars)
			p->numMatFx = 0;
	}
	c = chunk_ptr(p, "LSTS", &sz);
	p->lightStyles = (c && sz >= 64 * sizeof(PackLightStyle)) ? (const PackLightStyle *)c : NULL;
	c = chunk_ptr(p, "LSTP", &sz);
	if (c && sz >= 4 && sz >= 4 + *(u32 *)c * sizeof(PackStylePage)) {
		p->numStylePages = *(u32 *)c;
		p->stylePages = (const PackStylePage *)(c + 4);
	}
	c = chunk_ptr(p, "LSTF", &sz);
	if (c && sz >= 4 && sz >= 4 + *(u32 *)c * sizeof(PackStyleFace)) {
		p->numStyleFaces = *(u32 *)c;
		p->styleFaces = (const PackStyleFace *)(c + 4);
	}
	c = chunk_ptr(p, "MFAR", &sz);
	if (c) {
		p->modelFar = (u16 *)c;
		p->numModelFar = sz / 2;
	}
	c = chunk_ptr(p, "MSRF", &sz);
	p->modelSurf = (u16 *)c;
	p->numModelSurf = c ? sz / 2 : 0;
	c = chunk_ptr(p, "WATR", &sz);
	if (c) {
		p->numWater = *(u32 *)c;
		p->water = (PackWater *)(c + 4);
	}
	p->eyes = chunk_ptr(p, "NEYE", NULL);
	c = chunk_ptr(p, "PHON", &sz);
	if (c && sz >= 4) {
		p->numPhonKeys = *(u16 *)c;
		p->numPhonemes = *(u16 *)(c + 2);
		p->phonKeys = (const u32 *)(c + 4);
		p->phonemes = c + 4 + p->numPhonKeys * 4;
	}
	p->soundPhon = chunk_ptr(p, "SPHI", NULL);
	p->phonemeRecs = chunk_ptr(p, "SPHR", NULL);
	p->eventFlex = chunk_ptr(p, "SFEV", NULL);
	p->flexTracks = chunk_ptr(p, "SFTR", NULL);
	p->flexSamples = chunk_ptr(p, "SFSM", NULL);
	c = chunk_ptr(p, "SCEN", &sz);
	if (c) {
		p->numScenes = *(u32 *)c;
		p->scenes = (PackScene *)(c + 4);
	}
	p->sceneEvents = chunk_ptr(p, "SCEV", NULL);
	c = chunk_ptr(p, "SNDS", &sz);
	if (c) {
		p->numSounds = *(u32 *)c;
		p->sounds = (PackSound *)(c + 4);
	}
	p->soundCaptions = chunk_ptr(p, "SCAP", NULL);

	c = chunk_ptr(p, "SPWN", &sz);
	if (c) {
		memcpy(p->spawnOrigin, c, 12);
		memcpy(p->spawnAngles, c + 12, 12);
		memcpy(p->sky, c + 24, 12);
	} else {
		for (int i = 0; i < 6; i++)
			p->sky[i] = -1;
	}

	/* the GPU reads textures and vertices from main memory, so push them out of the CPU cache */
	DCFlushRange(p->base, (p->size + 31) & ~31);

	printf("%u faces, %u textures, %u models, %u props, %u entities\n", (unsigned)p->numFaces,
	       (unsigned)p->numTex, (unsigned)p->numModels, (unsigned)p->numStaticProps, (unsigned)p->numEntities);
	return 0;
}
