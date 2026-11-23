#ifndef HL2GC_WORLD_H
#define HL2GC_WORLD_H

#include <gccore.h>
#include "pack.h"

typedef struct {
	guVector origin;
	f32 pitch, yaw; /* degrees, Source convention: +pitch looks down, yaw around +Z */
	guVector forward, right, up;
	f32 fovx, fovy; /* degrees */
} Camera;

void Camera_Update(Camera *c);

typedef struct {
	const Pack *pack;
	int lastCluster;
	u8 *pvs;            /* one byte per cluster: 1 = potentially visible */
	u32 frame;
	u32 *faceFrame;     /* last frame each face was added */
	u16 *visible;       /* visible face list */
	u32 numVisible;
	/* faces bucketed by material (counting sort) */
	u32 *matCount;
	u32 *matStart;
	u16 *sorted;
	int leaf, cluster;
	u32 leafsDrawn;
} World;

int World_Init(World *w, const Pack *p);
void World_Free(World *w);
/* Visibility cluster containing a point, -1 outside the playable space. */
int World_PointCluster(const Pack *p, const f32 pos[3]);
int World_PointLeaf(const Pack *p, const f32 pos[3]);
/* Walk the BSP, apply PVS and frustum culling, and bucket visible faces by material. */
void World_Collect(World *w, const Camera *cam, int lockVis);

#endif
