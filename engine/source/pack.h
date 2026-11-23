/*
 * hl2gc - map pack (.hgm) format, produced by tools/convert.py from the user's own HL2 install.
 * All data is big-endian, chunk payloads are 32-byte aligned so textures and vertex arrays
 * can be handed straight to GX.
 */
#ifndef HL2GC_PACK_H
#define HL2GC_PACK_H

#include <gctypes.h>

#define PACK_VERSION 6

enum {
	MAT_OPAQUE = 0,
	MAT_ALPHATEST = 1,
	MAT_TRANSLUCENT = 2,
	MAT_ADDITIVE = 3,
	MAT_WATER = 4,
	MAT_SKY = 5,
	MAT_NODRAW = 6,
	MAT_BLEND = 7, /* WorldVertexTransition: two textures, vertex alpha blends */
	MAT_CAMERA = 8, /* monitor screen: shows the active point_camera's view */
	MAT_EYE = 9,    /* eyeball: sclera (tex) with the iris (tex2) projected over it */
};

#define PRIM_FAN 0
#define PRIM_TRIS 1
#define FACEF_SKY 1
#define FACEF_WATER 2
#define NO_LIGHTMAP 0xFFFF
#define NO_FACE 0xFFFF

typedef struct {
	u16 w, h;
	u8 fmt, levels, clamp, pad;
	u32 offset, size; /* offset is relative to the TEXD chunk */
} PackTexInfo;

typedef struct {
	s16 tex, tex2;
	u8 kind, r, g, b;
} PackMat;

typedef struct {
	f32 pos[3];
	f32 uv[2];
	f32 lm[2];
	u8 rgba[4];
} PackVert;

typedef struct {
	u32 firstvert;
	u16 numverts, mat, lmtex;
	u8 prim, flags;
} PackFace;

typedef struct {
	f32 n[3];
	f32 d;
} PackPlane;

typedef struct {
	s32 plane;
	s32 children[2]; /* negative = -(leaf + 1) */
} PackNode;

typedef struct {
	s16 cluster;
	u16 numleaffaces;
	u32 firstleafface;
	u32 firstleafbrush;
	u16 numleafbrushes, pad;
	s16 mins[3], maxs[3];
} PackLeaf;

typedef struct {
	u32 firstside, numsides;
	s32 contents;
} PackBrush;

typedef struct {
	u16 plane, bevel;
} PackBrushSide;

/* every brush model in the BSP (index = "*N"; 0 is the world): collision subtree + faces */
typedef struct {
	s32 headnode;
	f32 mins[3], maxs[3];
	u32 firstFace, numFaces; /* into bmodelFaces */
} PackBModel;

/* studio models (props) */
typedef struct {
	u32 firstMesh, numMeshes;
	u32 firstVert, numVerts;
	u32 firstHull, numHulls;
	f32 mins[3], maxs[3];
} PackModel;

typedef struct {
	u32 firstIndex, numIndices; /* into modelIndices, relative to the model's firstVert */
	u16 mat, frac;              /* frac: the model's position fraction bits (PackMVert.pos); top bit: a strip */
} PackMesh;

typedef struct {
	s16 pos[3]; /* 1 / 2^frac units (the mesh record's frac) */
	s16 uv[2];  /* 1/1024 */
	s8 nrm[3];  /* unit normal x 64 (GX_S8, 6 fraction bits) */
	u8 pad;
} PackMVert;

typedef struct {
	u32 firstPlane, numPlanes; /* convex collision piece, planes in model space */
	f32 mins[3], maxs[3];
} PackHull;

typedef struct {
	u16 model;
	u8 solid, flags;
	f32 origin[3], angles[3];
	u8 tint[4];
	s16 cluster, pad; /* pad: fademaxdist / 4, 0 = none */
} PackStaticProp;

typedef struct {
	u32 firstKV, numKV;
} PackEntity;

/* aim_yaw (param 0) / aim_pitch (1) turning a bone: its rotation at count entries evenly from start to end,
 * a delta on top of the sequence's (delta) or in place of it (NPPC) */
/* a ragdoll solid (the model's .phy): its bone, the bone of the solid it hangs from (-1 the root), its
 * thickness (units) and how far its joint swings (degrees) */
typedef struct PackRagSolid {
	u16 bone;
	s16 parent;
	f32 radius, swing;
} PackRagSolid;

/* its joint's ranges (the ragdollconstraint's xmin/xmax, ymin/ymax, zmin/zmax, degrees), about the solid's
 * own bone axes, from where it rests in its parent at bind (NPRL, one per solid; missing in old packs) */
typedef struct PackRagLimit {
	f32 lo[3], hi[3];
} PackRagLimit;

typedef struct PackAimCtl {
	u16 bone;
	u8 param, count, delta, pad;
	u16 pad2;
	f32 start, end;
	u32 firstQuat;
} PackAimCtl; /* 20 bytes */

typedef struct {
	u32 key, val; /* offsets into entStrings */
} PackKV;

typedef struct {
	f32 mins[3], maxs[3];
	char map[32], landmark[32];
} PackTrigger;

/* ---- NPCs: skeletons, baked animation, skinned meshes (rigid, <= 10 bones per batch) */
typedef struct {
	u16 numBones, numSeqs;
	u32 firstBone, firstSeq;   /* into npcBones / npcSeqs */
	s16 seqIdle, seqWalk, seqRun, seqDie; /* local sequence index, -1 if missing */
} PackSkel;

typedef struct {
	s32 parent;
	f32 poseToBone[3][4];
} PackBone;

#define SEQF_LOOP 1
#define SEQF_DELTA 2              /* STUDIO_DELTA: added onto the pose instead of blended toward */
#define SEQF_POST 4               /* STUDIO_POST: delta applied in the bone's own frame */
#define SEQF_REALTIME 8           /* STUDIO_REALTIME: cycles with time, not with the parent's cycle */
typedef struct {
	u32 name;                 /* entStrings offset */
	u16 numFrames, flags;
	f32 fps, speed;           /* speed: ground speed of the motion (units/s) */
	u32 firstFrame;           /* in PackBoneFrame records, frame-major: [frame][mask bone] */
	u16 numBones;             /* bones it animates (its weight list): numMask records from firstMask */
	u16 numLayers;            /* autolayers, from firstLayer */
	u32 firstMask, firstLayer;
} PackSeq;

typedef struct {
	u8 bone, weight;          /* skeleton bone, weight * 255 */
} PackSeqMask;

/* mstudioautolayer_t: a sub-sequence played under this one, faded in/out over the parent's cycle */
#define ALF_SPLINE 0x40
#define ALF_XFADE 0x80
#define ALF_NOBLEND 0x200
typedef struct {
	s16 seq;                  /* skeleton-local sequence */
	u16 flags;
	f32 start, peak, tail, end;
} PackSeqLayer;

typedef struct {
	s16 head, neck, spine, pad; /* bones turned by look-at (-1 = none) */
} PackSkelLook;

typedef struct {
	s16 q[3];                 /* rotation x,y,z * 32767, w >= 0 */
	s16 p[3];                 /* position * 64 */
} PackBoneFrame;

typedef struct {
	u16 skel, numBatches;
	u32 firstBatch;
	f32 eyeHeight, radius, height, pad;
} PackNpcModel;

typedef struct {
	u32 firstVert;
	u16 numVerts, mat;
	u32 firstIndex, numIndices;
	u8 numBones;
	u8 bones[10];
	u8 flexed;                /* has vertices moved by facial animation */
	u8 eye;                   /* eyeball index in the model (NEYE), 255 = not an eye */
	u8 strip;                 /* the indices are one triangle strip (else a triangle list) */
	u8 pad[2];
} PackNpcBatch;

/* break pieces: a breakable model's own (model >= 0), or a generic set (-1 wood, -2 glass, -3 metal,
 * -4 concrete) for func_breakables */
typedef struct {
	s16 model;
	u16 first, count;
	u16 pad;
	f32 fade;
} PackGibSet;

/* water surfaces (Source's Water shader): fog colour, refraction tint, distortion texture */
typedef struct {
	u16 mat;
	s16 normalTex;            /* IA8 distortion map (from the normal map), -1 none */
	u8 fog[3], tint[3];
	u8 reflect, pad;
	f32 fogStart, fogEnd;
} PackWater;

/* eyeballs: Source's eye shader projects the iris along where the eye looks (bind-pose model space) */
typedef struct {
	s32 bone;
	f32 org[3], up[3], fwd[3]; /* fwd points into the head */
	f32 zoffset, irisScale;
} PackEye;

typedef struct {
	u16 first, count;
} PackEyeIndex;

/* ---- faces (flex animation): per NPC model, Source's flex controllers -> rules -> morph targets */
typedef struct {
	u32 firstCtl;
	u16 numCtl, numDesc;
	u32 firstRule, numRules, firstFlex, numFlex, firstAtt, numAtt;
} PackNpcFace;

typedef struct {
	u32 name;
	f32 min, max;
} PackFlexCtl;

enum { FLOP_CONST = 1, FLOP_FETCH1, FLOP_FETCH2, FLOP_ADD, FLOP_SUB, FLOP_MUL, FLOP_DIV, FLOP_NEG, FLOP_EXP,
       FLOP_OPEN, FLOP_CLOSE, FLOP_COMMA, FLOP_MAX, FLOP_MIN, FLOP_2WAY_0, FLOP_2WAY_1, FLOP_NWAY, FLOP_COMBO,
       FLOP_DOMINATE };
typedef struct {
	u16 flexDesc, numOps;
	u32 firstOp;
} PackFlexRule;

typedef struct {
	u32 op;
	f32 value;
} PackFlexOp;

typedef struct {
	s16 desc, pair;           /* pair -1: not split left/right */
	f32 target[4];
	u32 firstVA, numVA;
} PackFlex;

typedef struct {
	u32 vert;                 /* NVTX index */
	s16 d[3];                 /* * 1/4096 */
	u8 side, pad;
} PackFlexVA;

typedef struct {
	u32 name;
	s32 bone;
	f32 local[3][4];
} PackAttachment;

typedef struct {
	u32 first, count;
} PackSoundPhon;

typedef struct {
	u16 phon, pad;
	f32 start, end;
} PackPhoneme;

typedef struct {
	u32 firstTrack;
	u16 numTracks, pad;
} PackEventFlex;

typedef struct {
	u32 name;
	f32 min, max;
	u32 firstSample, numSamples;
} PackFlexTrack;

typedef struct {
	f32 t, v;
} PackFlexSample;

typedef struct {
	f32 pos[3];
	s16 uv[2];                /* 1/1024 */
	u8 mtx;                   /* GX_PNMTX slot * 3 */
	s8 nrm[3];                /* bind-pose unit normal x 64 */
} PackNpcVert;

/* ---- choreographed scenes and sounds */
enum {
	SEV_SECTION = 1, SEV_EXPRESSION, SEV_LOOKAT, SEV_MOVETO, SEV_SPEAK, SEV_GESTURE, SEV_SEQUENCE, SEV_FACE,
	SEV_FIRETRIGGER, SEV_FLEXANIMATION, SEV_SUBSCENE, SEV_LOOP, SEV_INTERRUPT, SEV_STOPPOINT,
	SEV_PERMITRESPONSES, SEV_GENERIC
};
#define SEVF_RESUMECONDITION 1
typedef struct {
	u32 name;
	u16 numEvents, pad;
	u32 firstEvent;
	f32 duration;
} PackScene;

typedef struct {
	u8 type, flags;
	s16 sound;                /* into sounds, -1 = none */
	f32 start, end;
	u32 actor, p1, p2, p3;    /* entStrings offsets */
	u32 caption;              /* entStrings offset, 0 = none */
} PackSceneEvent;

typedef struct PackScape {
	u8 dsp, pad;            /* dsp room preset (1 = automatic) */
	u16 numElems;
	u32 firstElem;
} PackScape;

typedef struct PackScapeElem {
	u8 type;                /* 0 playlooping, 1 playrandom */
	u8 pos;                 /* position0-7 of the env_soundscape, 254 random, 255 none (everywhere) */
	u8 level, pad;          /* soundlevel dB, 0 = everywhere */
	u16 firstWave, numWaves; /* into scapeWaves (sound indices) */
	f32 volMin, volMax, timeMin, timeMax;
} PackScapeElem;

/* animation events that fire a scripted_sequence's OnScriptEventNN (SCRIPT_EVENT_FIREEVENT) */
typedef struct {
	u32 seq;                  /* global index into npcSeqs */
	f32 cycle;
	u16 option, pad;
} PackSeqEvent;

/* AI node graph (maps/graphs/<map>.ain): ground nodes and the links a human hull can use */
#define NAVN_GROUND 2
#define NAVN_AIR 3
#define NAVM_WALK 1
#define NAVM_JUMP 2
typedef struct {
	f32 pos[3];
	u8 type, pad;
	u16 firstLink, numLinks, pad2;
} PackNavNode;

typedef struct {
	u16 node;
	u8 moves, pad;
} PackNavLink;

/* gesture events: playback -> original timing tags (fractions * 4096) */
typedef struct {
	u8 numTags, pad;
	u16 play[4], orig[4];
	u16 pad2;
} PackEventTags;

typedef struct {
	u32 offset;               /* in hl2gc/vo.bin: IMA ADPCM, 22050 Hz mono, 12-byte header */
	u32 sizeVol;              /* volume (0..255) << 24 | bytes */
	u32 samplesLevel;         /* soundlevel dB (0 = heard everywhere) << 24 | samples */
} PackSound;
/* offsets in the bank are multiples of 32; bit 0 marks a sound with loop points (tools/soundflags.py) */
static inline u32 Sound_Offset(const PackSound *s) { return s->offset & ~31u; }
static inline int Sound_Loops(const PackSound *s) { return (int)(s->offset & 1); }
static inline u32 Sound_Size(const PackSound *s) { return s->sizeVol & 0xFFFFFF; }
static inline u32 Sound_Samples(const PackSound *s) { return s->samplesLevel & 0xFFFFFF; }
static inline u32 Sound_Level(const PackSound *s) { return s->samplesLevel >> 24; }
static inline f32 Sound_Volume(const PackSound *s) { return (f32)(s->sizeVol >> 24) / 255.0f; }

typedef struct {
	char name[32];
	f32 origin[3];
} PackLandmark;

#define CONTENTS_SOLID 0x1
#define CONTENTS_WINDOW 0x2
#define CONTENTS_GRATE 0x8
#define CONTENTS_SLIME 0x10
#define CONTENTS_WATER 0x20
#define MASK_WATER (CONTENTS_WATER | CONTENTS_SLIME)
#define CONTENTS_MOVEABLE 0x4000
#define CONTENTS_PLAYERCLIP 0x10000
#define MASK_PLAYERSOLID (CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_GRATE)
#define CONTENTS_MONSTERCLIP 0x20000
/* what bullets hit */
#define MASK_SHOT (CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW)
/* Source's MASK_NPCSOLID (less CONTENTS_MONSTER: NPCs' own boxes are skipped separately) */
#define MASK_NPCSOLID (CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_MONSTERCLIP | CONTENTS_WINDOW | CONTENTS_GRATE)


/* WLIT: a worldlight (tools/props.py worldlight_chunk). type: 0 surface, 1 point, 2 spot, 3 sun;
 * intensity is linear (lightmap scale); reach is where it fades below notice (0 = sun, no falloff) */
typedef struct {
	f32 origin[3], intensity[3], normal[3];
	s32 type, cluster, style;
	f32 stopdot, stopdot2, exponent, reach, c, l, q;
} PackWorldLight;

/* MFXH/MFXO/MFXV: material effects (tools/matfx.py) */
#define FX_UNLIT 1
#define FX_TWOTEX 2
#define FX_SELFILLUM 4
#define FX_XFORM 8
#define FX_XFORM2 16
#define FX_COLOR 32
typedef struct {
	u16 mat;
	u8 flags, frames;
	s16 frame0, tex2;
	u16 firstOp, numOps, firstVar, numVars;
} PackMatFx;
typedef struct {
	u8 op, dst, a, b, c, d;
	u16 pad;
	f32 k0, k1;
} PackFxOp;

/* LSTS: per light style (0-63), its starting brightness (x64) and pattern ('a' dark, 'm' normal) */
typedef struct {
	u8 on64, patLen;
	char pattern[30];
} PackLightStyle;
/* LSTP: a page of light style layers (laid out like lightmap page "layer" of the faces it serves) */
typedef struct {
	u16 tex;
	u8 layer, pad;
	f32 vscale; /* the base page's height / this page's */
} PackStylePage;
/* LSTF: one styled face's layer */
typedef struct {
	u16 face;
	u8 style, page;
} PackStyleFace;

typedef struct {
	u8 *base;
	u32 size;

	u32 numTex;
	PackTexInfo *tex;
	u8 *texData;

	u32 numMats;
	PackMat *mats;

	u32 numVerts;
	PackVert *verts;

	u32 numFaces;
	PackFace *faces;

	u32 numPlanes;
	PackPlane *planes;
	u32 numNodes;
	PackNode *nodes;
	u32 numLeafs;
	PackLeaf *leafs;
	u32 numLeafFaces;
	u16 *leafFaces;

	u32 numBrushes;
	PackBrush *brushes;
	u32 numBrushSides;
	PackBrushSide *brushSides;
	u32 numLeafBrushes;
	u16 *leafBrushes;
	u32 numBModels;
	PackBModel *bmodels;
	u16 *bmodelFaces;

	u32 numModels;
	PackModel *models;
	PackMesh *meshes;
	PackMVert *mverts;
	u16 *mindices;
	PackHull *hulls;
	PackPlane *hullPlanes;
	u32 numStaticProps;
	PackStaticProp *staticProps;

	u32 numEntities;
	PackEntity *entities;
	PackKV *kvs;
	const char *entStrings;
	u32 numTriggers;
	PackTrigger *triggers;
	u32 numLandmarks;
	PackLandmark *landmarks;

	u32 numClusters; /* 0 = no visibility data, draw everything */
	u32 *visOffsets;
	u8 *visBase;

	u32 numSkels;
	PackSkel *skels;
	PackBone *npcBones;
	PackSeq *npcSeqs;
	const f32 (*npcSeqMove)[4]; /* NSQM: per sequence, its root motion over the whole sequence:
	                               forward, left, up, yaw (Studio_SeqMovement); NULL in old packs */
	u32 numNpcSeqMove;
	PackBoneFrame *npcFrames;
	PackSeqMask *npcMasks;
	PackSeqLayer *npcLayers;
	PackBoneFrame *npcRest;   /* rest pose, parallel to npcBones */
	PackSkelLook *skelLook;   /* parallel to skels */
	/* aiming pose parameters (cameras, turrets): per skeleton (first, count) of its controls */
	const u16 (*skelAim)[2];
	const u16 (*skelRag)[2];       /* NPRI: per skeleton (first, count) into ragCtls */
	const struct PackRagSolid *ragSolids; /* NPRR */
	const struct PackRagLimit *ragLimits; /* NPRL (NULL in old packs) */
	u32 numSkelRag;
	const struct PackAimCtl *aimCtls;
	const f32 (*aimQuats)[4];
	u32 numSkelAim;
	PackEventTags *eventTags; /* parallel to sceneEvents */
	u32 numSeqEvents;
	PackSeqEvent *seqEvents;
	/* NSQS: animation sound events (footsteps, AE_CL_PLAYSOUND...): option = sound, pad = run variant */
	u32 numSeqSounds;
	PackSeqEvent *seqSounds;
	/* SSCP: soundscapes (scripts/soundscapes*.txt, flattened); env_soundscape entities have "_scape" */
	u32 numScapes, numScapeElems, numScapeWaves;
	struct PackScape *scapes;
	struct PackScapeElem *scapeElems;
	s16 *scapeWaves;
	u32 numNavNodes;
	PackNavNode *navNodes;
	PackNavLink *navLinks;
	u32 numNpcModels;
	PackNpcModel *npcModels;
	PackNpcBatch *npcBatches;
	PackNpcVert *npcVerts;
	u16 *npcIndices;

	PackNpcFace *npcFaces;    /* parallel to npcModels */
	PackFlexCtl *flexCtls;
	PackFlexRule *flexRules;
	PackFlexOp *flexOps;
	PackFlex *flexes;
	PackFlexVA *flexVAs;
	PackAttachment *attachments;
	PackEyeIndex *eyeIndex;   /* parallel to npcModels */
	u32 numWater;
	PackWater *water;
	/* MSTP: per material, its surface (footstep sounds in weapons.hgm's STEP table) */
	u32 numMatSurf;
	u8 *matSurf;
	/* SPCB: per static prop, its ambient cube (6 faces x RGB: +X -X +Y -Y +Z -Z; all 0 = none);
	 * LCUB: per BSP leaf, the same for things that move */
	const u8 (*staticCubes)[18];
	u32 numLeafCubes;
	const u8 (*leafCubes)[18];
	/* SPLO: per static prop, the point it's lit at; WLIT: the map's lights (models get their direct light) */
	const f32 (*staticLightOrg)[3];
	/* SPCL: per static prop (first, count) into spClusters: every cluster its leaves touch (Source draws
	 * it when any is visible); NULL in older packs (then the single cluster in PackStaticProp) */
	const u16 (*spClusterRange)[2];
	const u16 *spClusters;
	u32 numSpClusters;
	u32 numWorldLights;
	const PackWorldLight *worldLights;
	const PackLightStyle *lightStyles; /* 64 */
	u32 numMatFx;
	const PackMatFx *matFx;
	const PackFxOp *fxOps;
	const f32 *fxVars;
	u32 numFxOps, numFxVars;
	u32 numStylePages, numStyleFaces;
	const PackStylePage *stylePages;
	const PackStyleFace *styleFaces;
	/* MFAR: per model, its far LOD (another model; 0xFFFF none) */
	u16 *modelFar;
	u32 numModelFar;
	/* MSRF: per model, its surface ($surfaceprop: impact sounds in weapons.hgm's SURF table) */
	u16 *modelSurf;
	u32 numModelSurf;
	u32 numGibSets;
	PackGibSet *gibSets;
	u16 *gibModels;
	PackEye *eyes;
	u16 numPhonKeys, numPhonemes;
	const u32 *phonKeys;      /* controller names */
	const u8 *phonemes;       /* per phoneme: u32 name, f32 weights[numPhonKeys] (-1 = leave alone) */
	PackSoundPhon *soundPhon; /* parallel to sounds */
	PackPhoneme *phonemeRecs;
	PackEventFlex *eventFlex; /* parallel to sceneEvents */
	PackFlexTrack *flexTracks;
	PackFlexSample *flexSamples;

	u32 numScenes;
	PackScene *scenes;
	PackSceneEvent *sceneEvents;
	u32 numSounds;
	PackSound *sounds;
	const u32 *soundCaptions; /* parallel to sounds: entStrings offset of the caption, 0 none */

	f32 spawnOrigin[3]; /* feet */
	f32 spawnAngles[3];
	s16 sky[6]; /* rt lf bk ft up dn, -1 if missing */
} Pack;

/* Returns 0 on success, otherwise prints a reason and returns -1. */
int Pack_Load(Pack *p, const char *path);
/* Same, for a pack already in memory (32-byte aligned), e.g. one linked into the .dol. */
int Pack_LoadMemory(Pack *p, u8 *data, u32 size);
/* Any chunk by tag (NULL if absent). */
void *Pack_Chunk(const Pack *p, const char *tag, u32 *size);

#endif
