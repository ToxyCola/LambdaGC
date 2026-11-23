#include "collide.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	f32 mins[3], maxs[3];
	u32 firstvert, numverts; /* triangle list in pack->verts */
	u16 mat;
	u32 firstGroup, numGroups; /* into dispGroups: bounds of each run of DISP_GROUP triangles */
} DispCol;

#define DISP_GROUP 8
typedef struct {
	f32 mins[3], maxs[3];
} DispGroup;
static DispGroup *dispGroups;

/* a static prop's convex piece, already moved into world space */
typedef struct {
	f32 mins[3], maxs[3];
	u32 firstPlane, numPlanes; /* into propPlanes */
} PropHull;

static const Pack *P;
static u32 *brushCheck;
static u32 checkCount;
static DispCol *disps;
static u32 numDisps;
static PropHull *propHulls;
static u32 numPropHulls;
static PackPlane *propPlanes;

/* static prop hulls bucketed on a 2D grid, so a trace only looks at hulls near it (a map has a thousand
 * or more; testing every one's box for every trace was the biggest cost of a trace) */
#define PGRID_CELL 256.0f
#define PGRID_MAX 128
static f32 pgOx, pgOy;
static int pgNx, pgNy;
static u32 *pgStart; /* pgNx*pgNy+1 */
static u32 *pgItems;
static u32 *pgStamp; /* per hull: last query that tested it */
static u32 pgQuery;
static const CMEnt *ents;
static int skipBoxes;
static int worldOnly;
static s32 onlyEnt = -1; /* CM_TraceOnly: just this entity's solids */
static s32 ignoreEnt = -1;
static s32 ignoreExtra[8];
static int numIgnoreExtra;
static u32 numEnts;

#define MAX_SIDES 256

typedef struct {
	f32 start[3], end[3];     /* box centres, world */
	f32 ext[3];               /* half extents */
	f32 swMins[3], swMaxs[3]; /* bounds of the whole sweep (world) */
	f32 cs[3], ce[3];         /* start/end in the frame currently being tested */
	const f32 (*R)[3];        /* that frame's rotation (NULL = world axes) */
	s32 mask;
	s32 isPoint;
	Trace tr;
} Work;

static inline f32 dot3(const f32 *a, const f32 *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static int boxes_overlap(const f32 *amin, const f32 *amax, const f32 *bmin, const f32 *bmax)
{
	for (int k = 0; k < 3; k++)
		if (amax[k] < bmin[k] || amin[k] > bmax[k])
			return 0;
	return 1;
}

/* ------------------------------------------------------------------ setup */

static void pg_cells(const f32 *mins, const f32 *maxs, int *x0, int *y0, int *x1, int *y1)
{
	*x0 = (int)floorf((mins[0] - pgOx) / PGRID_CELL);
	*y0 = (int)floorf((mins[1] - pgOy) / PGRID_CELL);
	*x1 = (int)floorf((maxs[0] - pgOx) / PGRID_CELL);
	*y1 = (int)floorf((maxs[1] - pgOy) / PGRID_CELL);
	if (*x0 < 0) *x0 = 0;
	if (*y0 < 0) *y0 = 0;
	if (*x1 >= pgNx) *x1 = pgNx - 1;
	if (*y1 >= pgNy) *y1 = pgNy - 1;
}

static void free_prop_grid(void)
{
	free(pgStart);
	free(pgItems);
	free(pgStamp);
	pgStart = pgItems = pgStamp = NULL;
	pgNx = pgNy = 0;
}

static void build_prop_grid(void)
{
	free_prop_grid();
	if (!numPropHulls)
		return;
	f32 lo[2] = {FLT_MAX, FLT_MAX}, hi[2] = {-FLT_MAX, -FLT_MAX};
	for (u32 i = 0; i < numPropHulls; i++)
		for (int k = 0; k < 2; k++) {
			lo[k] = fminf(lo[k], propHulls[i].mins[k]);
			hi[k] = fmaxf(hi[k], propHulls[i].maxs[k]);
		}
	pgOx = lo[0];
	pgOy = lo[1];
	pgNx = (int)((hi[0] - lo[0]) / PGRID_CELL) + 1;
	pgNy = (int)((hi[1] - lo[1]) / PGRID_CELL) + 1;
	if (pgNx > PGRID_MAX) pgNx = PGRID_MAX; /* (huge maps: the edge cells just get bigger) */
	if (pgNy > PGRID_MAX) pgNy = PGRID_MAX;
	u32 cells = (u32)(pgNx * pgNy);
	pgStart = calloc(cells + 1, sizeof(u32));
	pgStamp = calloc(numPropHulls, sizeof(u32));
	if (!pgStart || !pgStamp) {
		free_prop_grid();
		return;
	}
	int x0, y0, x1, y1;
	u32 total = 0;
	for (u32 i = 0; i < numPropHulls; i++) {
		pg_cells(propHulls[i].mins, propHulls[i].maxs, &x0, &y0, &x1, &y1);
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++)
				pgStart[y * pgNx + x + 1]++, total++;
	}
	for (u32 c = 0; c < cells; c++)
		pgStart[c + 1] += pgStart[c];
	pgItems = malloc((total ? total : 1) * sizeof(u32));
	u32 *fill = malloc(cells * sizeof(u32));
	if (!pgItems || !fill) {
		free(fill);
		free_prop_grid();
		return;
	}
	memcpy(fill, pgStart, cells * sizeof(u32));
	for (u32 i = 0; i < numPropHulls; i++) {
		pg_cells(propHulls[i].mins, propHulls[i].maxs, &x0, &y0, &x1, &y1);
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++)
				pgItems[fill[y * pgNx + x]++] = i;
	}
	free(fill);
	pgQuery = 0;
}

static void build_static_props(const Pack *p)
{
	u32 nh = 0, np = 0;
	for (u32 i = 0; i < p->numStaticProps; i++) {
		const PackStaticProp *sp = &p->staticProps[i];
		if (!sp->solid || sp->model >= p->numModels)
			continue;
		const PackModel *m = &p->models[sp->model];
		nh += m->numHulls;
		for (u32 h = 0; h < m->numHulls; h++)
			np += p->hulls[m->firstHull + h].numPlanes;
	}
	propHulls = malloc((nh ? nh : 1) * sizeof(PropHull));
	propPlanes = malloc((np ? np : 1) * sizeof(PackPlane));
	if (!propHulls || !propPlanes)
		return;
	numPropHulls = 0;
	u32 pc = 0;
	for (u32 i = 0; i < p->numStaticProps; i++) {
		const PackStaticProp *sp = &p->staticProps[i];
		if (!sp->solid || sp->model >= p->numModels)
			continue;
		f32 R[3][3];
		extern void Ent_AngleMatrix(const f32 angles[3], f32 R[3][3]);
		Ent_AngleMatrix(sp->angles, R);
		const PackModel *m = &p->models[sp->model];
		for (u32 h = 0; h < m->numHulls; h++) {
			const PackHull *hull = &p->hulls[m->firstHull + h];
			PropHull *ph = &propHulls[numPropHulls++];
			ph->firstPlane = pc;
			ph->numPlanes = hull->numPlanes;
			for (u32 k = 0; k < hull->numPlanes; k++) {
				const PackPlane *src = &p->hullPlanes[hull->firstPlane + k];
				PackPlane *dst = &propPlanes[pc++];
				for (int r = 0; r < 3; r++)
					dst->n[r] = R[r][0] * src->n[0] + R[r][1] * src->n[1] + R[r][2] * src->n[2];
				dst->d = src->d + dot3(dst->n, sp->origin);
			}
			/* world bounds of the rotated piece: transform its 8 corners */
			for (int k = 0; k < 3; k++) {
				ph->mins[k] = FLT_MAX;
				ph->maxs[k] = -FLT_MAX;
			}
			for (int c = 0; c < 8; c++) {
				f32 v[3] = {(c & 1) ? hull->maxs[0] : hull->mins[0], (c & 2) ? hull->maxs[1] : hull->mins[1],
				            (c & 4) ? hull->maxs[2] : hull->mins[2]};
				for (int r = 0; r < 3; r++) {
					f32 w = R[r][0] * v[0] + R[r][1] * v[1] + R[r][2] * v[2] + sp->origin[r];
					if (w < ph->mins[r])
						ph->mins[r] = w;
					if (w > ph->maxs[r])
						ph->maxs[r] = w;
				}
			}
		}
	}
}

int CM_Init(const Pack *p)
{
	CM_Shutdown();
	P = p;
	brushCheck = calloc(p->numBrushes ? p->numBrushes : 1, sizeof(u32));
	checkCount = 0;
	numDisps = 0;
	for (u32 i = 0; i < p->numFaces; i++)
		if (p->faces[i].prim == PRIM_TRIS)
			numDisps++;
	disps = malloc((numDisps ? numDisps : 1) * sizeof(DispCol));
	if (!brushCheck || !disps)
		return -1;
	u32 n = 0;
	for (u32 i = 0; i < p->numFaces; i++) {
		const PackFace *f = &p->faces[i];
		if (f->prim != PRIM_TRIS)
			continue;
		DispCol *d = &disps[n++];
		d->firstvert = f->firstvert;
		d->numverts = f->numverts;
		d->mat = f->mat;
		for (int k = 0; k < 3; k++) {
			d->mins[k] = FLT_MAX;
			d->maxs[k] = -FLT_MAX;
		}
		for (u32 v = 0; v < f->numverts; v++)
			for (int k = 0; k < 3; k++) {
				f32 x = p->verts[f->firstvert + v].pos[k];
				if (x < d->mins[k])
					d->mins[k] = x;
				if (x > d->maxs[k])
					d->maxs[k] = x;
			}
	}
	/* groups of triangles with their own bounds: a trace only looks at the triangles near its path (it
	 * tested every triangle of every displacement its sweep's box touched: 80% of the NPCs' tick on
	 * terrain maps, mostly their long line-of-sight checks) */
	u32 groups = 0;
	for (u32 i = 0; i < numDisps; i++)
		groups += (disps[i].numverts / 3 + DISP_GROUP - 1) / DISP_GROUP;
	dispGroups = malloc((groups ? groups : 1) * sizeof(DispGroup));
	if (!dispGroups)
		return -1;
	groups = 0;
	for (u32 i = 0; i < numDisps; i++) {
		DispCol *d = &disps[i];
		u32 tris = d->numverts / 3;
		d->firstGroup = groups;
		d->numGroups = (tris + DISP_GROUP - 1) / DISP_GROUP;
		for (u32 g = 0; g < d->numGroups; g++) {
			DispGroup *G = &dispGroups[groups++];
			for (int k = 0; k < 3; k++) {
				G->mins[k] = FLT_MAX;
				G->maxs[k] = -FLT_MAX;
			}
			for (u32 v = g * DISP_GROUP * 3; v < (g + 1) * DISP_GROUP * 3 && v < tris * 3; v++)
				for (int k = 0; k < 3; k++) {
					f32 x = p->verts[d->firstvert + v].pos[k];
					if (x < G->mins[k])
						G->mins[k] = x;
					if (x > G->maxs[k])
						G->maxs[k] = x;
				}
		}
	}
	build_static_props(p);
#ifndef CM_NO_PROP_GRID
	build_prop_grid();
#endif
	return (propHulls && propPlanes) ? 0 : -1;
}

void CM_SetSkipBoxes(int on) { skipBoxes = on; }
static int physTrace;
void CM_SetPhysicsTrace(int on) { physTrace = on; }
void CM_SetIgnoreEnt(s32 ent) { ignoreEnt = ent; }
void CM_SetIgnoreExtra(const s32 *ents, int n)
{
	numIgnoreExtra = n < 0 ? 0 : n > 8 ? 8 : n;
	for (int i = 0; i < numIgnoreExtra; i++)
		ignoreExtra[i] = ents[i];
}

void CM_SetEntities(const CMEnt *list, u32 n)
{
	ents = list;
	numEnts = n;
}

void CM_Shutdown(void)
{
	free(brushCheck);
	free(disps);
	free(dispGroups);
	dispGroups = NULL;
	free(propHulls);
	free(propPlanes);
	free_prop_grid();
	brushCheck = NULL;
	disps = NULL;
	propHulls = NULL;
	propPlanes = NULL;
	numDisps = numPropHulls = 0;
	ents = NULL;
	numEnts = 0;
	P = NULL;
}

/* ------------------------------------------------------------------ convex clipping */

/* Sweep the box (w->cs -> w->ce, half extents w->ext) against a convex set of planes. */
static void clip_planes(Work *w, const PackPlane *const *planes, u32 n, s32 contents, const u8 *surf)
{
	u32 leadIdx = 0;
	f32 enterFrac = -1.0f, leaveFrac = 1.0f;
	const PackPlane *lead = NULL;
	int startout = 0, getout = 0;

	for (u32 i = 0; i < n; i++) {
		const PackPlane *pl = planes[i];
		/* push the plane out by the box's extent along its normal */
		f32 dist = pl->d + fabsf(pl->n[0]) * w->ext[0] + fabsf(pl->n[1]) * w->ext[1] + fabsf(pl->n[2]) * w->ext[2];
		f32 d1 = dot3(w->cs, pl->n) - dist;
		f32 d2 = dot3(w->ce, pl->n) - dist;
		if (d2 > 0)
			getout = 1;
		if (d1 > 0)
			startout = 1;
		if (d1 > 0 && (d2 >= DIST_EPSILON || d2 >= d1))
			return; /* entirely in front of this side: misses */
		if (d1 <= 0 && d2 <= 0)
			continue;
		if (d1 > d2) { /* entering */
			f32 f = (d1 - DIST_EPSILON) / (d1 - d2);
			if (f < 0)
				f = 0;
			if (f > enterFrac) {
				enterFrac = f;
				lead = pl;
				leadIdx = i;
			}
		} else { /* leaving */
			f32 f = (d1 + DIST_EPSILON) / (d1 - d2);
			if (f > 1)
				f = 1;
			if (f < leaveFrac)
				leaveFrac = f;
		}
	}

	if (!startout) {
		w->tr.startsolid = 1;
		if (!getout) {
			w->tr.allsolid = 1;
			w->tr.fraction = 0;
			w->tr.contents = contents;
		}
		return;
	}
	if (enterFrac < leaveFrac && enterFrac > -1 && enterFrac < w->tr.fraction && lead) {
		if (enterFrac < 0)
			enterFrac = 0;
		w->tr.fraction = enterFrac;
		if (w->R) {
			for (int r = 0; r < 3; r++)
				w->tr.normal[r] = w->R[r][0] * lead->n[0] + w->R[r][1] * lead->n[1] + w->R[r][2] * lead->n[2];
		} else {
			memcpy(w->tr.normal, lead->n, sizeof(w->tr.normal));
		}
		w->tr.contents = contents;
		w->tr.surface = surf && surf[leadIdx] != 255 ? surf[leadIdx] : -1;
	}
}

static void trace_leaf(Work *w, const PackLeaf *leaf)
{
	const PackPlane *planes[MAX_SIDES];
	u8 surf[MAX_SIDES];
	for (u32 i = 0; i < leaf->numleafbrushes; i++) {
		u16 bi = P->leafBrushes[leaf->firstleafbrush + i];
		if (brushCheck[bi] == checkCount)
			continue;
		brushCheck[bi] = checkCount;
		const PackBrush *b = &P->brushes[bi];
		if (!(b->contents & w->mask))
			continue;
		u32 n = b->numsides < MAX_SIDES ? b->numsides : MAX_SIDES;
		for (u32 s = 0; s < n; s++) {
			const PackBrushSide *bs = &P->brushSides[b->firstside + s];
			planes[s] = &P->planes[bs->plane];
			surf[s] = (u8)(bs->bevel >> 8); /* tools/convert.py: surface id in the high byte, 255 none */
		}
		clip_planes(w, planes, n, b->contents, surf);
		if (w->tr.allsolid)
			return;
	}
}

/* Walk the tree, only descending into the side(s) the swept box touches. */
static void trace_node(Work *w, s32 node, f32 p1f, f32 p2f, const f32 *p1, const f32 *p2)
{
	if (w->tr.fraction <= p1f)
		return; /* already hit something nearer */
	if (node < 0) {
		trace_leaf(w, &P->leafs[-1 - node]);
		return;
	}
	const PackNode *n = &P->nodes[node];
	const PackPlane *pl = &P->planes[n->plane];
	f32 t1 = dot3(p1, pl->n) - pl->d;
	f32 t2 = dot3(p2, pl->n) - pl->d;
	f32 offset = w->isPoint ? 0.0f
	                        : fabsf(pl->n[0]) * w->ext[0] + fabsf(pl->n[1]) * w->ext[1] + fabsf(pl->n[2]) * w->ext[2];

	if (t1 >= offset + 1 && t2 >= offset + 1) {
		trace_node(w, n->children[0], p1f, p2f, p1, p2);
		return;
	}
	if (t1 < -offset - 1 && t2 < -offset - 1) {
		trace_node(w, n->children[1], p1f, p2f, p1, p2);
		return;
	}

	int side;
	f32 frac, frac2;
	if (t1 < t2) {
		f32 idist = 1.0f / (t1 - t2);
		side = 1;
		frac2 = (t1 + offset + DIST_EPSILON) * idist;
		frac = (t1 - offset + DIST_EPSILON) * idist;
	} else if (t1 > t2) {
		f32 idist = 1.0f / (t1 - t2);
		side = 0;
		frac2 = (t1 - offset - DIST_EPSILON) * idist;
		frac = (t1 + offset + DIST_EPSILON) * idist;
	} else {
		side = 0;
		frac = 1;
		frac2 = 0;
	}
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	if (frac2 < 0)
		frac2 = 0;
	if (frac2 > 1)
		frac2 = 1;

	f32 mid[3];
	f32 midf = p1f + (p2f - p1f) * frac;
	for (int i = 0; i < 3; i++)
		mid[i] = p1[i] + frac * (p2[i] - p1[i]);
	trace_node(w, n->children[side], p1f, midf, p1, mid);

	midf = p1f + (p2f - p1f) * frac2;
	for (int i = 0; i < 3; i++)
		mid[i] = p1[i] + frac2 * (p2[i] - p1[i]);
	trace_node(w, n->children[side ^ 1], midf, p2f, mid, p2);
}

/* ------------------------------------------------------------------ displacement triangles */

/* Separating-axis test over the sweep: for every candidate axis, find the time window in which
 * the moving box overlaps the triangle; the hit is the latest entry across all axes. */
static void clip_to_triangle(Work *w, const f32 *v0, const f32 *v1, const f32 *v2)
{
	f32 e[3][3], nrm[3], axes[13][3];
	for (int i = 0; i < 3; i++) {
		e[0][i] = v1[i] - v0[i];
		e[1][i] = v2[i] - v1[i];
		e[2][i] = v0[i] - v2[i];
	}
	nrm[0] = e[0][1] * e[1][2] - e[0][2] * e[1][1];
	nrm[1] = e[0][2] * e[1][0] - e[0][0] * e[1][2];
	nrm[2] = e[0][0] * e[1][1] - e[0][1] * e[1][0];
	f32 nl = sqrtf(dot3(nrm, nrm));
	if (nl < 1e-6f)
		return;
	int na = 0;
	for (int i = 0; i < 3; i++)
		axes[na][i] = nrm[i] / nl;
	na++;
	for (int k = 0; k < 3; k++) {
		for (int i = 0; i < 3; i++)
			axes[na][i] = (i == k) ? 1.0f : 0.0f;
		na++;
	}
	for (int k = 0; k < 3; k++)
		for (int j = 0; j < 3; j++) {
			f32 a[3] = {0, 0, 0};
			int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
			a[k1] = -e[j][k2];
			a[k2] = e[j][k1];
			f32 l = sqrtf(dot3(a, a));
			if (l < 1e-6f)
				continue;
			for (int i = 0; i < 3; i++)
				axes[na][i] = a[i] / l;
			na++;
		}

	f32 d[3] = {w->end[0] - w->start[0], w->end[1] - w->start[1], w->end[2] - w->start[2]};
	f32 tEnter = -FLT_MAX, tExit = FLT_MAX;
	f32 hitN[3] = {0, 0, 1};
	for (int a = 0; a < na; a++) {
		const f32 *ax = axes[a];
		f32 p0 = dot3(ax, v0), p1 = dot3(ax, v1), p2 = dot3(ax, v2);
		f32 tmin = fminf(p0, fminf(p1, p2)), tmax = fmaxf(p0, fmaxf(p1, p2));
		f32 r = fabsf(ax[0]) * w->ext[0] + fabsf(ax[1]) * w->ext[1] + fabsf(ax[2]) * w->ext[2];
		f32 c = dot3(ax, w->start), v = dot3(ax, d);
		f32 lo = tmin - r - c, hi = tmax + r - c;
		if (fabsf(v) < 1e-7f) {
			if (lo > 0 || hi < 0)
				return;
			continue;
		}
		f32 t0 = lo / v, t1 = hi / v;
		f32 sign = -1.0f;
		if (t0 > t1) {
			f32 t = t0;
			t0 = t1;
			t1 = t;
			sign = 1.0f;
		}
		if (t0 > tEnter) {
			tEnter = t0;
			for (int i = 0; i < 3; i++)
				hitN[i] = ax[i] * sign;
		}
		if (t1 < tExit)
			tExit = t1;
		if (tEnter > tExit || tExit < 0 || tEnter > w->tr.fraction)
			return;
	}
	if (tEnter < 0) {
		if (dot3(d, hitN) >= 0)
			return;
		tEnter = 0;
	}
	f32 len = sqrtf(dot3(d, d));
	f32 f = len > 0 ? tEnter - DIST_EPSILON / len : 0;
	if (f < 0)
		f = 0;
	if (f < w->tr.fraction) {
		w->tr.fraction = f;
		memcpy(w->tr.normal, hitN, sizeof(hitN));
		w->tr.contents = CONTENTS_SOLID;
	}
}

/* does the swept box (start to end, half extents ext) touch the box? slab test on the box grown by ext */
static int sweep_hits_box(const Work *w, const f32 mins[3], const f32 maxs[3])
{
	if (!boxes_overlap(w->swMins, w->swMaxs, mins, maxs))
		return 0;
	f32 t0 = 0, t1 = 1;
	for (int k = 0; k < 3; k++) {
		f32 lo = mins[k] - w->ext[k] - 1.0f, hi = maxs[k] + w->ext[k] + 1.0f;
		f32 s = w->start[k], d = w->end[k] - s;
		if (fabsf(d) < 1e-6f) {
			if (s < lo || s > hi)
				return 0;
			continue;
		}
		f32 a = (lo - s) / d, b = (hi - s) / d;
		if (a > b) {
			f32 t = a;
			a = b;
			b = t;
		}
		if (a > t0)
			t0 = a;
		if (b < t1)
			t1 = b;
		if (t0 > t1)
			return 0;
	}
	return 1;
}

static void trace_disps(Work *w)
{
	for (u32 i = 0; i < numDisps; i++) {
		const DispCol *dc = &disps[i];
		if (!sweep_hits_box(w, dc->mins, dc->maxs))
			continue;
		const PackVert *v = &P->verts[dc->firstvert];
		u32 tris = dc->numverts / 3;
		for (u32 g = 0; g < dc->numGroups; g++) {
			const DispGroup *G = &dispGroups[dc->firstGroup + g];
			if (!sweep_hits_box(w, G->mins, G->maxs))
				continue;
			for (u32 t = g * DISP_GROUP * 3; t < (g + 1) * DISP_GROUP * 3 && t + 2 < tris * 3; t += 3) {
				int out = 0;
				for (int k = 0; k < 3 && !out; k++) {
					f32 lo = fminf(v[t].pos[k], fminf(v[t + 1].pos[k], v[t + 2].pos[k]));
					f32 hi = fmaxf(v[t].pos[k], fmaxf(v[t + 1].pos[k], v[t + 2].pos[k]));
					if (w->swMaxs[k] < lo || w->swMins[k] > hi)
						out = 1;
				}
				if (!out) {
					f32 before = w->tr.fraction;
					clip_to_triangle(w, v[t].pos, v[t + 1].pos, v[t + 2].pos);
					if (w->tr.fraction < before)
						w->tr.surface = dc->mat < P->numMatSurf ? P->matSurf[dc->mat] : -1;
				}
			}
		}
	}
}

/* ------------------------------------------------------------------ props and entities */

static void clip_prop_hull(Work *w, const PropHull *h, const PackPlane **planes)
{
	if (!boxes_overlap(w->swMins, w->swMaxs, h->mins, h->maxs))
		return;
	u32 n = h->numPlanes < MAX_SIDES ? h->numPlanes : MAX_SIDES;
	for (u32 k = 0; k < n; k++)
		planes[k] = &propPlanes[h->firstPlane + k];
	clip_planes(w, planes, n, CONTENTS_SOLID, NULL);
}

static void trace_static_props(Work *w)
{
	const PackPlane *planes[MAX_SIDES];
	w->R = NULL;
	memcpy(w->cs, w->start, sizeof(w->cs));
	memcpy(w->ce, w->end, sizeof(w->ce));
	if (pgStart) {
		int x0, y0, x1, y1;
		pg_cells(w->swMins, w->swMaxs, &x0, &y0, &x1, &y1);
		if (x0 > x1 || y0 > y1)
			return;
		if (++pgQuery == 0) { /* (wrapped: start the stamps over) */
			memset(pgStamp, 0, numPropHulls * sizeof(u32));
			pgQuery = 1;
		}
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++) {
				u32 c = (u32)(y * pgNx + x);
				for (u32 j = pgStart[c]; j < pgStart[c + 1] && !w->tr.allsolid; j++) {
					u32 i = pgItems[j];
					if (pgStamp[i] == pgQuery)
						continue;
					pgStamp[i] = pgQuery;
					clip_prop_hull(w, &propHulls[i], planes);
				}
			}
		return;
	}
	for (u32 i = 0; i < numPropHulls && !w->tr.allsolid; i++)
		clip_prop_hull(w, &propHulls[i], planes);
}

/* Bring the sweep into an entity's frame: local = R^T (world - origin). The box keeps its
 * axis-aligned extents, which is exact for the 90-degree steps doors usually rest at. */
static void to_local(Work *w, const CMEnt *e)
{
	f32 s[3], t[3];
	for (int k = 0; k < 3; k++) {
		s[k] = w->start[k] - e->origin[k];
		t[k] = w->end[k] - e->origin[k];
	}
	if (e->rotated) {
		for (int c = 0; c < 3; c++) {
			w->cs[c] = e->R[0][c] * s[0] + e->R[1][c] * s[1] + e->R[2][c] * s[2];
			w->ce[c] = e->R[0][c] * t[0] + e->R[1][c] * t[1] + e->R[2][c] * t[2];
		}
		w->R = e->R;
	} else {
		memcpy(w->cs, s, sizeof(s));
		memcpy(w->ce, t, sizeof(t));
		w->R = NULL;
	}
}

static void trace_entities(Work *w)
{
	const PackPlane *planes[MAX_SIDES];
	for (u32 i = 0; i < numEnts && !w->tr.allsolid; i++) {
		const CMEnt *e = &ents[i];
		if (onlyEnt >= 0 && e->ent != onlyEnt)
			continue;
		if (ignoreEnt >= 0 && e->ent == ignoreEnt)
			continue;
		if (e->physOnly && !physTrace)
			continue;
		if (numIgnoreExtra) {
			int skip = 0;
			for (int k = 0; k < numIgnoreExtra; k++)
				skip |= e->ent == ignoreExtra[k];
			if (skip)
				continue;
		}
		if (!boxes_overlap(w->swMins, w->swMaxs, e->mins, e->maxs))
			continue;
		to_local(w, e);
		f32 before = w->tr.fraction;
		int wasSolid = w->tr.startsolid;
		if (e->bmodel > 0 && (u32)e->bmodel < P->numBModels) {
			f32 s[3], t[3];
			memcpy(s, w->cs, sizeof(s));
			memcpy(t, w->ce, sizeof(t));
			checkCount++;
			trace_node(w, P->bmodels[e->bmodel].headnode, 0, 1, s, t);
		} else if (e->mdl >= 0 && (u32)e->mdl < P->numModels) {
			const PackModel *m = &P->models[e->mdl];
			for (u32 h = 0; h < m->numHulls; h++) {
				const PackHull *hull = &P->hulls[m->firstHull + h];
				u32 n = hull->numPlanes < MAX_SIDES ? hull->numPlanes : MAX_SIDES;
				for (u32 k = 0; k < n; k++)
					planes[k] = &P->hullPlanes[hull->firstPlane + k];
				clip_planes(w, planes, n, CONTENTS_SOLID, NULL);
			}
		} else if (e->bmodel <= 0 && e->mdl < 0 && !skipBoxes) {
			/* plain box (NPC bodies): six axial planes in the entity's frame */
			PackPlane bp[6];
			for (int k = 0; k < 3; k++) {
				PackPlane *hi = &bp[k * 2], *lo = &bp[k * 2 + 1];
				memset(hi, 0, sizeof(*hi));
				memset(lo, 0, sizeof(*lo));
				hi->n[k] = 1.0f;
				hi->d = e->maxs[k] - e->origin[k];
				lo->n[k] = -1.0f;
				lo->d = -(e->mins[k] - e->origin[k]);
			}
			for (int k = 0; k < 6; k++)
				planes[k] = &bp[k];
			clip_planes(w, planes, 6, CONTENTS_SOLID, NULL);
		}
		if (w->tr.fraction < before || (w->tr.startsolid && !wasSolid))
			w->tr.ent = e->ent;
	}
	w->R = NULL;
}

/* ------------------------------------------------------------------ entry points */

void CM_BoxTrace(Trace *tr, const f32 start[3], const f32 end[3], const f32 mins[3], const f32 maxs[3], s32 mask)
{
	Work w;
	memset(&w, 0, sizeof(w));
	w.tr.fraction = 1.0f;
	w.tr.ent = -1;
	w.tr.surface = -1;
	w.mask = mask;
	for (int i = 0; i < 3; i++) {
		f32 c = (mins[i] + maxs[i]) * 0.5f;
		w.ext[i] = (maxs[i] - mins[i]) * 0.5f;
		w.start[i] = start[i] + c;
		w.end[i] = end[i] + c;
		w.swMins[i] = fminf(w.start[i], w.end[i]) - w.ext[i] - 1.0f;
		w.swMaxs[i] = fmaxf(w.start[i], w.end[i]) + w.ext[i] + 1.0f;
	}
	w.isPoint = (w.ext[0] == 0 && w.ext[1] == 0 && w.ext[2] == 0);

	if (onlyEnt >= 0) {
		trace_entities(&w);
	} else if (P && P->numNodes) {
		memcpy(w.cs, w.start, sizeof(w.cs));
		memcpy(w.ce, w.end, sizeof(w.ce));
		checkCount++;
		trace_node(&w, 0, 0, 1, w.start, w.end);
		if (!w.tr.allsolid && (mask & CONTENTS_SOLID)) {
			trace_disps(&w);
			if (!worldOnly) {
				trace_static_props(&w);
				trace_entities(&w);
			}
		}
	}

	*tr = w.tr;
	if (tr->fraction == 1.0f) {
		memcpy(tr->endpos, end, sizeof(tr->endpos));
	} else {
		for (int i = 0; i < 3; i++)
			tr->endpos[i] = start[i] + tr->fraction * (end[i] - start[i]);
	}
}

/* is a box (world, at origin) inside one of a brush model's brushes (a trigger's own shape, not its
 * bounding box: Source touches a trigger by its brushes). origin/R: the model's placement (R may be NULL) */
int CM_BoxInBModel(u32 bmodel, const f32 morigin[3], const f32 (*R)[3], const f32 pos[3], const f32 mins[3],
                   const f32 maxs[3])
{
	if (!P || !bmodel || bmodel >= P->numBModels)
		return 0;
	Work w;
	memset(&w, 0, sizeof(w));
	w.tr.fraction = 1.0f;
	w.tr.ent = -1;
	w.tr.surface = -1;
	w.mask = -1;
	for (int i = 0; i < 3; i++) {
		f32 c = (mins[i] + maxs[i]) * 0.5f;
		w.ext[i] = (maxs[i] - mins[i]) * 0.5f;
		w.start[i] = w.end[i] = pos[i] + c;
	}
	w.isPoint = (w.ext[0] == 0 && w.ext[1] == 0 && w.ext[2] == 0);
	f32 d[3] = {w.start[0] - morigin[0], w.start[1] - morigin[1], w.start[2] - morigin[2]};
	if (R) {
		for (int k = 0; k < 3; k++)
			w.cs[k] = R[0][k] * d[0] + R[1][k] * d[1] + R[2][k] * d[2];
		w.R = R;
	} else {
		memcpy(w.cs, d, sizeof(d));
	}
	memcpy(w.ce, w.cs, sizeof(w.cs));
	f32 a[3], b[3];
	memcpy(a, w.cs, 12);
	memcpy(b, w.ce, 12);
	checkCount++;
	trace_node(&w, P->bmodels[bmodel].headnode, 0, 1, a, b);
	return w.tr.startsolid || w.tr.allsolid;
}

void CM_TraceWorld(Trace *tr, const f32 start[3], const f32 end[3], s32 mask)
{
	static const f32 z[3] = {0, 0, 0};
	worldOnly = 1;
	CM_BoxTrace(tr, start, end, z, z, mask);
	worldOnly = 0;
}

void CM_TraceOnly(Trace *tr, const f32 start[3], const f32 end[3], const f32 mins[3], const f32 maxs[3], s32 mask,
                  s32 ent)
{
	onlyEnt = ent;
	CM_BoxTrace(tr, start, end, mins, maxs, mask);
	onlyEnt = -1;
}

int CM_PointContents(const f32 p[3])
{
	if (!P || !P->numNodes)
		return 0;
	s32 node = 0;
	while (node >= 0) {
		const PackNode *n = &P->nodes[node];
		const PackPlane *pl = &P->planes[n->plane];
		node = n->children[(dot3(p, pl->n) - pl->d) >= 0 ? 0 : 1];
	}
	const PackLeaf *leaf = &P->leafs[-1 - node];
	int contents = 0;
	for (u32 i = 0; i < leaf->numleafbrushes; i++) {
		const PackBrush *b = &P->brushes[P->leafBrushes[leaf->firstleafbrush + i]];
		int inside = 1;
		for (u32 s = 0; s < b->numsides && inside; s++) {
			const PackPlane *pl = &P->planes[P->brushSides[b->firstside + s].plane];
			if (dot3(p, pl->n) - pl->d > 0)
				inside = 0;
		}
		if (inside)
			contents |= b->contents;
	}
	return contents;
}
