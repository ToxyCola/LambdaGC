/*
 * hl2gc collision: swept axis-aligned boxes against the map.
 *  - brushes, by walking the BSP tree (world + solid brush entities)
 *  - displacement terrain, as triangles (Source collides displacements as meshes, not brushes)
 *  - static props and prop entities, using the convex pieces from their .phy collision models
 *  - moving/rotating brush entities (doors), traced in their own frame
 */
#ifndef HL2GC_COLLIDE_H
#define HL2GC_COLLIDE_H

#include "pack.h"

#define DIST_EPSILON 0.03125f
#define SURF_SKY 254 /* Trace.surface of a sky face (tools/convert.py) */

typedef struct {
	f32 fraction;      /* 0..1 along the move */
	f32 endpos[3];
	f32 normal[3];     /* surface hit */
	s32 startsolid;    /* started inside something */
	s32 allsolid;      /* never left it */
	s32 contents;
	s32 ent;           /* entity hit, -1 = the world or a static prop */
	s32 surface;       /* surface property (footsteps) of what was hit, -1 unknown */
} Trace;

/* A solid entity: a brush model or a studio model's collision hulls, placed and rotated. */
typedef struct {
	s32 bmodel, mdl;
	f32 origin[3];
	f32 R[3][3];
	u8 rotated;
	f32 mins[3], maxs[3]; /* world-space bounds for quick rejection */
	s32 ent;              /* owning entity index (traces can ignore one) */
	u8 physOnly;          /* func_clip_vphysics: only physics objects collide with it */
} CMEnt;

int CM_Init(const Pack *p);
void CM_SetEntities(const CMEnt *ents, u32 n);
void CM_Shutdown(void);
void CM_BoxTrace(Trace *tr, const f32 start[3], const f32 end[3], const f32 mins[3], const f32 maxs[3], s32 mask);
int CM_PointContents(const f32 p[3]);
/* a line against the map alone (brushes and displacements; no props or entities): light visibility */
void CM_TraceWorld(Trace *tr, const f32 start[3], const f32 end[3], s32 mask);
/* a box against one entity's solids only (is the player inside the train that just moved?) */
void CM_TraceOnly(Trace *tr, const f32 start[3], const f32 end[3], const f32 mins[3], const f32 maxs[3], s32 mask,
                  s32 ent);
/* While set, traces ignore plain-box entities (NPC bodies), so NPCs don't collide with themselves. */
void CM_SetSkipBoxes(int on);
/* while on, traces are a physics object's: they meet func_clip_vphysics too */
void CM_SetPhysicsTrace(int on);
/* While >= 0, traces ignore the solid owned by this entity (a physics prop tracing its own motion). */
void CM_SetIgnoreEnt(s32 ent);
/* up to 4 more entities traces pass through (an NPC's func_brush "excludednpc" exceptions); n = 0 clears */
void CM_SetIgnoreExtra(const s32 *ents, int n);
/* a box inside a brush model's own brushes (triggers touch by their brushes, not their bounding box) */
int CM_BoxInBModel(u32 bmodel, const f32 morigin[3], const f32 (*R)[3], const f32 pos[3], const f32 mins[3],
                   const f32 maxs[3]);

#endif
