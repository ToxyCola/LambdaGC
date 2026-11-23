#include "world.h"

#include <malloc.h>
#include <math.h>
#include <string.h>

#define DEG2RAD(x) ((x) * (f32)M_PI / 180.0f)

void Camera_Update(Camera *c)
{
	f32 sp = sinf(DEG2RAD(c->pitch)), cp = cosf(DEG2RAD(c->pitch));
	f32 sy = sinf(DEG2RAD(c->yaw)), cy = cosf(DEG2RAD(c->yaw));
	c->forward = (guVector){cp * cy, cp * sy, -sp};
	c->right = (guVector){sy, -cy, 0.0f};
	c->up = (guVector){sp * cy, sp * sy, cp};
}

int World_Init(World *w, const Pack *p)
{
	memset(w, 0, sizeof(*w));
	w->pack = p;
	w->lastCluster = -2;
	w->pvs = malloc(p->numClusters ? p->numClusters : 1);
	w->faceFrame = calloc(p->numFaces, sizeof(u32));
	w->visible = malloc(p->numFaces * sizeof(u16));
	w->sorted = malloc(p->numFaces * sizeof(u16));
	w->matCount = malloc(p->numMats * sizeof(u32));
	w->matStart = malloc((p->numMats + 1) * sizeof(u32));
	return (w->pvs && w->faceFrame && w->visible && w->sorted && w->matCount && w->matStart) ? 0 : -1;
}

void World_Free(World *w)
{
	free(w->pvs);
	free(w->faceFrame);
	free(w->visible);
	free(w->sorted);
	free(w->matCount);
	free(w->matStart);
	memset(w, 0, sizeof(*w));
}

static int find_leaf(const Pack *p, const guVector *pos)
{
	if (!p->numNodes)
		return 0;
	s32 node = 0;
	while (node >= 0) {
		const PackNode *n = &p->nodes[node];
		const PackPlane *pl = &p->planes[n->plane];
		f32 d = pl->n[0] * pos->x + pl->n[1] * pos->y + pl->n[2] * pos->z - pl->d;
		node = n->children[d >= 0.0f ? 0 : 1];
	}
	return -1 - node;
}

int World_PointLeaf(const Pack *p, const f32 pos[3])
{
	guVector v = {pos[0], pos[1], pos[2]};
	return find_leaf(p, &v);
}

int World_PointCluster(const Pack *p, const f32 pos[3])
{
	guVector v = {pos[0], pos[1], pos[2]};
	return p->leafs[find_leaf(p, &v)].cluster;
}

/* Source PVS: run-length encoded, a zero byte is followed by a count of zero bytes */
static void decompress_pvs(World *w, int cluster)
{
	const Pack *p = w->pack;
	u32 n = p->numClusters;
	if (cluster < 0 || (u32)cluster >= n) {
		memset(w->pvs, 1, n); /* outside the world: draw everything */
		return;
	}
	memset(w->pvs, 0, n);
	const u8 *in = p->visBase + p->visOffsets[cluster];
	u32 c = 0;
	while (c < n) {
		if (*in == 0) {
			c += 8 * in[1];
			in += 2;
			continue;
		}
		for (int bit = 0; bit < 8 && c + bit < n; bit++)
			if (*in & (1 << bit))
				w->pvs[c + bit] = 1;
		c += 8;
		in++;
	}
}

typedef struct {
	guVector n;
} FrustumPlane;

static void build_frustum(const Camera *c, FrustumPlane fp[4])
{
	/* inward-facing side planes through the eye, widened slightly to avoid edge popping */
	f32 h = DEG2RAD(c->fovx * 0.5f + 5.0f), v = DEG2RAD(c->fovy * 0.5f + 5.0f);
	f32 sh = sinf(h), ch = cosf(h), sv = sinf(v), cv = cosf(v);
	for (int i = 0; i < 3; i++) {
		f32 f = (&c->forward.x)[i], r = (&c->right.x)[i], u = (&c->up.x)[i];
		(&fp[0].n.x)[i] = f * sh + r * ch;
		(&fp[1].n.x)[i] = f * sh - r * ch;
		(&fp[2].n.x)[i] = f * sv - u * cv;
		(&fp[3].n.x)[i] = f * sv + u * cv;
	}
}

static int box_visible(const FrustumPlane fp[4], const guVector *eye, const s16 *mins, const s16 *maxs)
{
	for (int i = 0; i < 4; i++) {
		const guVector *n = &fp[i].n;
		f32 x = (n->x > 0 ? maxs[0] : mins[0]) - eye->x;
		f32 y = (n->y > 0 ? maxs[1] : mins[1]) - eye->y;
		f32 z = (n->z > 0 ? maxs[2] : mins[2]) - eye->z;
		if (n->x * x + n->y * y + n->z * z < 0.0f)
			return 0;
	}
	return 1;
}

void World_Collect(World *w, const Camera *cam, int lockVis)
{
	const Pack *p = w->pack;
	w->frame++;
	w->numVisible = 0;
	w->leafsDrawn = 0;

	if (!lockVis) {
		w->leaf = find_leaf(p, &cam->origin);
		w->cluster = p->leafs[w->leaf].cluster;
	}
	if (p->numClusters && w->cluster != w->lastCluster) {
		decompress_pvs(w, w->cluster);
		w->lastCluster = w->cluster;
	}

	FrustumPlane fp[4];
	build_frustum(cam, fp);

	for (u32 i = 0; i < p->numLeafs; i++) {
		const PackLeaf *l = &p->leafs[i];
		if (!l->numleaffaces)
			continue;
		if (p->numClusters && (l->cluster < 0 || !w->pvs[l->cluster]))
			continue;
		if (!box_visible(fp, &cam->origin, l->mins, l->maxs))
			continue;
		w->leafsDrawn++;
		const u16 *lf = &p->leafFaces[l->firstleafface];
		for (u32 k = 0; k < l->numleaffaces; k++) {
			u16 f = lf[k];
			if (f == NO_FACE || w->faceFrame[f] == w->frame)
				continue;
			w->faceFrame[f] = w->frame;
			w->visible[w->numVisible++] = f;
		}
	}

	/* bucket by material so texture/TEV state changes once per material */
	memset(w->matCount, 0, p->numMats * sizeof(u32));
	for (u32 i = 0; i < w->numVisible; i++)
		w->matCount[p->faces[w->visible[i]].mat]++;
	u32 acc = 0;
	for (u32 m = 0; m < p->numMats; m++) {
		w->matStart[m] = acc;
		acc += w->matCount[m];
	}
	w->matStart[p->numMats] = acc;
	memset(w->matCount, 0, p->numMats * sizeof(u32));
	for (u32 i = 0; i < w->numVisible; i++) {
		u16 f = w->visible[i];
		u16 m = p->faces[f].mat;
		w->sorted[w->matStart[m] + w->matCount[m]++] = f;
	}
}
