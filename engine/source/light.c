#include "light.h"
#include "collide.h"
#include "world.h"
#include "ent.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIGHT_CUTOFF 0.004f /* linear light below notice (tools/props.py) */
#define SUN_REACH 16384.0f
#define RELIGHT_MOVE 24.0f   /* a moving thing is re-lit after going this far... */
#define RELIGHT_FRAMES 90    /* ...or this many frames (lights switch, doors open) */
#define RELIGHT_BUDGET 16    /* trace cost a frame for re-lighting (a short trace 1, the sun's long one 4) */

static f32 dec[256]; /* u8 -> linear */
static int decReady;

static const Pack *litPack;
static u8 (*litStatic)[18];

static u8 *pvs;
static u32 pvsSize;
static int pvsCluster = -2;

typedef struct {
	u8 cube[18];
	u8 ok;
	u8 age;
	f32 at[3];
} EntLight;
static EntLight *entLight;
static u32 entLightCap;
static int budget = RELIGHT_BUDGET;
static int traceCost; /* of the last Light_AddDirect */

/* light styles: Source steps patterns at 10 letters a second */
static u8 styleOn[LIGHT_STYLES];
static char stylePat[LIGHT_STYLES][31];
static u8 styleLen[LIGHT_STYLES];
static f32 styleVal[LIGHT_STYLES];
static f32 styleClock;

static f32 pattern_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		c = (char)(c - 'A' + 'a');
	return c <= 'a' ? 0.0f : (c - 'a') / 12.0f;
}

static void style_update(int s)
{
	if (!styleOn[s]) {
		styleVal[s] = 0;
		return;
	}
	if (styleLen[s] == 0) {
		styleVal[s] = 1;
		return;
	}
	u32 k = (u32)(styleClock * 10.0f) % styleLen[s];
	styleVal[s] = pattern_value(stylePat[s][k]);
}

static void styles_reset(const Pack *p)
{
	styleClock = 0;
	for (int s = 0; s < LIGHT_STYLES; s++) {
		const PackLightStyle *ls = p && p->lightStyles ? &p->lightStyles[s] : NULL;
		styleOn[s] = ls ? ls->on64 >= 32 : 1;
		styleLen[s] = ls ? (ls->patLen > 30 ? 30 : ls->patLen) : 0;
		memset(stylePat[s], 0, sizeof(stylePat[s]));
		if (ls)
			memcpy(stylePat[s], ls->pattern, styleLen[s]);
		style_update(s);
	}
}

f32 Light_StyleValue(int style)
{
	if (style <= 0 || style >= LIGHT_STYLES)
		return 1.0f;
	return styleVal[style];
}

void Light_StyleTick(f32 dt)
{
	styleClock += dt;
	if (styleClock > 3600.0f)
		styleClock -= 3600.0f;
	for (int s = 1; s < LIGHT_STYLES; s++)
		style_update(s);
}

void Light_StyleSwitch(int style, int on)
{
	if (style <= 0 || style >= LIGHT_STYLES)
		return;
	styleOn[style] = on < 0 ? !styleOn[style] : on != 0;
	style_update(style);
	/* moving things pick it up as they're re-lit (static props keep what they started with) */
}

void Light_StylePattern(int style, const char *pattern)
{
	if (style <= 0 || style >= LIGHT_STYLES || !pattern)
		return;
	size_t n = strlen(pattern);
	if (n > 30)
		n = 30;
	memset(stylePat[style], 0, sizeof(stylePat[style]));
	memcpy(stylePat[style], pattern, n);
	styleLen[style] = (u8)n;
	style_update(style);
}

static void init_tables(void);
static u8 enc(f32 lin);
static void add_dir(f32 lin[18], const f32 L[3], f32 s, const f32 col[3]);

/* ---- light_dynamic */
#define MAX_DLIGHTS 16
static struct {
	s32 ent;
	u8 on;
	f32 color[3]; /* _light rgb / 255 */
	f32 exponent; /* "brightness": the light is color x 2^exponent */
	f32 radius;   /* "distance" */
	f32 inner, outer; /* spot cone half-angles' cosines (outer 0: a point light) */
} dl[MAX_DLIGHTS];
static int numDl;

static f32 kvnum(const Entity *e, const char *key, f32 def)
{
	const char *v = Ent_KV((s32)(e - Ent_List(&(u32){0})), key);
	return v && *v ? (f32)atof(v) : def;
}

void Light_DynamicSpawn(s32 ent)
{
	if (numDl >= MAX_DLIGHTS)
		return;
	u32 n;
	Entity *E = Ent_List(&n);
	if (ent <= 0 || (u32)ent >= n)
		return;
	const Entity *e = &E[ent];
	int k = numDl++;
	dl[k].ent = ent;
	dl[k].on = !(e->spawnflags & 1); /* (no "start off" flag in HL2's light_dynamic: on) */
	dl[k].on = 1;
	const char *c = Ent_KV(ent, "_light");
	f32 r = 255, g = 255, b = 255;
	if (c)
		sscanf(c, "%f %f %f", &r, &g, &b);
	dl[k].color[0] = r / 255.0f;
	dl[k].color[1] = g / 255.0f;
	dl[k].color[2] = b / 255.0f;
	dl[k].exponent = kvnum(e, "brightness", 0);
	dl[k].radius = kvnum(e, "distance", 120);
	f32 cone = kvnum(e, "_cone", 0), inner = kvnum(e, "_inner_cone", cone);
	dl[k].outer = cone > 0 && cone < 90 ? cosf(cone * (f32)M_PI / 180.0f) : 0;
	dl[k].inner = cone > 0 && cone < 90 ? cosf(inner * (f32)M_PI / 180.0f) : 0;
}

void Light_DynamicInput(s32 ent, const char *in, const char *param)
{
	for (int k = 0; k < numDl; k++) {
		if (dl[k].ent != ent)
			continue;
		if (!strcasecmp(in, "turnon"))
			dl[k].on = 1;
		else if (!strcasecmp(in, "turnoff"))
			dl[k].on = 0;
		else if (!strcasecmp(in, "toggle"))
			dl[k].on = !dl[k].on;
		else if (!strcasecmp(in, "brightness"))
			dl[k].exponent = (f32)atof(param);
		else if (!strcasecmp(in, "distance"))
			dl[k].radius = (f32)atof(param);
		else if (!strcasecmp(in, "color")) {
			f32 r = 255, g = 255, b = 255;
			sscanf(param, "%f %f %f", &r, &g, &b);
			dl[k].color[0] = r / 255.0f;
			dl[k].color[1] = g / 255.0f;
			dl[k].color[2] = b / 255.0f;
		} else if (!strcasecmp(in, "_cone") || !strcasecmp(in, "_inner_cone")) {
			f32 a = (f32)atof(param);
			f32 cs = a > 0 && a < 90 ? cosf(a * (f32)M_PI / 180.0f) : 0;
			if (in[1] == 'c' || in[1] == 'C')
				dl[k].outer = cs;
			else
				dl[k].inner = cs;
		}
	}
}

int Light_AddDynamic(const f32 pos[3], u8 cube[18])
{
	if (!numDl)
		return 0;
	u32 n;
	Entity *E = Ent_List(&n);
	f32 lin[18];
	int any = 0;
	for (int k = 0; k < numDl; k++) {
		if (!dl[k].on || dl[k].ent <= 0 || (u32)dl[k].ent >= n)
			continue;
		const Entity *e = &E[dl[k].ent];
		if (e->flags & (EF_KILLED | EF_DISABLED))
			continue;
		f32 d[3] = {e->origin[0] - pos[0], e->origin[1] - pos[1], e->origin[2] - pos[2]};
		f32 d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], r = dl[k].radius;
		if (r <= 0 || d2 >= r * r)
			continue;
		f32 dist = sqrtf(d2 > 1 ? d2 : 1);
		f32 L[3] = {d[0] / dist, d[1] / dist, d[2] / dist};
		/* Source's dlights on models: falls to nothing at the radius */
		f32 f = 1.0f - d2 / (r * r);
		if (dl[k].outer > 0) {
			/* aimed at its target (or along its angles) */
			f32 dir[3] = {e->R[0][0], e->R[1][0], e->R[2][0]};
			const char *t = Ent_KV(dl[k].ent, "target");
			if (t && *t) {
				for (u32 i = 1; i < n; i++)
					if (E[i].name && !strcasecmp(E[i].name, t)) {
						f32 v[3] = {E[i].origin[0] - e->origin[0], E[i].origin[1] - e->origin[1], E[i].origin[2] - e->origin[2]};
						f32 l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
						if (l > 0.01f)
							for (int a = 0; a < 3; a++)
								dir[a] = v[a] / l;
						break;
					}
			}
			f32 c = -(L[0] * dir[0] + L[1] * dir[1] + L[2] * dir[2]);
			if (c <= dl[k].outer)
				continue;
			if (c < dl[k].inner && dl[k].inner > dl[k].outer)
				f *= (c - dl[k].outer) / (dl[k].inner - dl[k].outer);
		}
		f32 scale = f * powf(2.0f, dl[k].exponent);
		if (scale <= 0.001f)
			continue;
		if (!any) {
			init_tables();
			for (int i = 0; i < 18; i++)
				lin[i] = dec[cube[i]];
			any = 1;
		}
		add_dir(lin, L, scale, dl[k].color);
	}
	if (any)
		for (int i = 0; i < 18; i++)
			cube[i] = enc(lin[i]);
	return any;
}

static void init_tables(void)
{
	if (decReady)
		return;
	for (int i = 0; i < 256; i++)
		dec[i] = powf(i / 127.5f, 2.2f);
	decReady = 1;
}

static u8 enc(f32 lin)
{
	/* the nearest table entry (dec is increasing) */
	if (lin <= 0)
		return 0;
	if (lin >= dec[255])
		return 255;
	int lo = 0, hi = 255;
	while (hi - lo > 1) {
		int mid = (lo + hi) >> 1;
		if (dec[mid] <= lin)
			lo = mid;
		else
			hi = mid;
	}
	return (u8)((lin - dec[lo] < dec[hi] - lin) ? lo : hi);
}

static int cube_ok(const u8 *c)
{
	for (int i = 0; i < 18; i++)
		if (c[i])
			return 1;
	return 0;
}

/* the PVS row of the cluster at pos (NULL = everything potentially visible) */
static const u8 *pvs_row(const Pack *p, int cluster)
{
	if (cluster < 0 || !p->numClusters || !p->visBase || !p->visOffsets || (u32)cluster >= p->numClusters)
		return NULL;
	if (pvsSize < p->numClusters) {
		free(pvs);
		pvs = malloc(p->numClusters);
		pvsSize = pvs ? p->numClusters : 0;
		pvsCluster = -2;
		if (!pvs)
			return NULL;
	}
	if (cluster == pvsCluster)
		return pvs;
	u32 n = p->numClusters;
	memset(pvs, 0, n);
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
				pvs[c + bit] = 1;
		c += 8;
		in++;
	}
	pvsCluster = cluster;
	return pvs;
}

static void add_dir(f32 lin[18], const f32 L[3], f32 s, const f32 col[3])
{
	for (int a = 0; a < 3; a++) {
		f32 d = L[a];
		int face = a * 2 + (d < 0 ? 1 : 0);
		f32 w = fabsf(d) * s;
		for (int k = 0; k < 3; k++)
			lin[face * 3 + k] += w * col[k];
	}
}

void Light_AddDirect(const Pack *p, const f32 pos[3], u8 cube[18])
{
	if (!p || !p->numWorldLights || !p->worldLights)
		return;
	init_tables();
	f32 lin[18];
	for (int i = 0; i < 18; i++)
		lin[i] = dec[cube[i]];
	const u8 *row = pvs_row(p, World_PointCluster(p, pos));
	int added = 0;
	traceCost = 0;
	for (u32 i = 0; i < p->numWorldLights; i++) {
		const PackWorldLight *wl = &p->worldLights[i];
		f32 sv = Light_StyleValue(wl->style);
		if (sv <= 0.01f)
			continue; /* (switched off) */
		f32 col[3] = {wl->intensity[0] * sv, wl->intensity[1] * sv, wl->intensity[2] * sv};
		f32 imax = fmaxf(col[0], fmaxf(col[1], col[2]));
		Trace tr;
		if (wl->type == 3) {
			/* the sun: lit if the way towards it ends in sky */
			f32 L[3] = {-wl->normal[0], -wl->normal[1], -wl->normal[2]};
			f32 end[3] = {pos[0] + L[0] * SUN_REACH, pos[1] + L[1] * SUN_REACH, pos[2] + L[2] * SUN_REACH};
			CM_TraceWorld(&tr, pos, end, CONTENTS_SOLID);
			traceCost += 4;
			if (tr.startsolid || (tr.fraction < 1 && tr.surface != SURF_SKY))
				continue;
			add_dir(lin, L, 1.0f, col);
			added = 1;
			continue;
		}
		f32 d[3] = {wl->origin[0] - pos[0], wl->origin[1] - pos[1], wl->origin[2] - pos[2]};
		f32 d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (d2 > wl->reach * wl->reach)
			continue;
		if (row && wl->cluster >= 0 && (u32)wl->cluster < p->numClusters && !row[wl->cluster])
			continue;
		if (d2 < 1)
			d2 = 1;
		f32 dist = sqrtf(d2);
		f32 L[3] = {d[0] / dist, d[1] / dist, d[2] / dist};
		f32 facing = -(L[0] * wl->normal[0] + L[1] * wl->normal[1] + L[2] * wl->normal[2]);
		f32 f;
		if (wl->type == 0) {
			/* a light-emitting surface: falls off with the square of distance, and only lights what it faces */
			if (facing <= 0)
				continue;
			f = facing / d2;
		} else {
			f32 den = wl->c + wl->l * dist + wl->q * d2;
			if (den <= 0)
				continue;
			f = 1.0f / den;
			if (wl->type == 2) {
				if (facing <= wl->stopdot2)
					continue;
				if (facing < wl->stopdot) {
					f32 r = (facing - wl->stopdot2) / (wl->stopdot - wl->stopdot2);
					if (wl->exponent != 0 && wl->exponent != 1)
						r = powf(r, wl->exponent);
					f *= r;
				}
			}
		}
		if (f * imax < LIGHT_CUTOFF)
			continue;
		/* can it see us (the map only: props and people don't block it, as in Source) */
		f32 end[3] = {wl->origin[0] - L[0] * 2, wl->origin[1] - L[1] * 2, wl->origin[2] - L[2] * 2};
		CM_TraceWorld(&tr, pos, end, CONTENTS_SOLID);
		traceCost++;
		if (tr.fraction < 1 && !tr.startsolid)
			continue;
		add_dir(lin, L, f, col);
		added = 1;
	}
	if (added)
		for (int i = 0; i < 18; i++)
			cube[i] = enc(lin[i]);
}

static int leaf_cube(const Pack *p, const f32 pos[3], u8 out[18])
{
	if (!p || !p->leafCubes || !p->numNodes)
		return 0;
	int leaf = World_PointLeaf(p, pos);
	if (leaf < 0 || (u32)leaf >= p->numLeafCubes || !cube_ok(p->leafCubes[leaf]))
		return 0;
	memcpy(out, p->leafCubes[leaf], 18);
	return 1;
}

void Light_Shutdown(void)
{
	numDl = 0;
	free(litStatic);
	free(entLight);
	free(pvs);
	litStatic = NULL;
	entLight = NULL;
	pvs = NULL;
	entLightCap = pvsSize = 0;
	pvsCluster = -2;
	litPack = NULL;
}

void Light_Init(const Pack *p)
{
	Light_Shutdown();
	init_tables();
	styles_reset(p);
	litPack = p;
	if (!p || !p->staticCubes || !p->numStaticProps)
		return;
	litStatic = malloc(p->numStaticProps * 18);
	if (!litStatic)
		return;
	for (u32 i = 0; i < p->numStaticProps; i++) {
		memcpy(litStatic[i], p->staticCubes[i], 18);
		if (p->staticLightOrg && cube_ok(litStatic[i]))
			Light_AddDirect(p, p->staticLightOrg[i], litStatic[i]);
	}
}

const u8 *Light_StaticCube(const Pack *p, u32 prop)
{
	if (!p || prop >= p->numStaticProps)
		return NULL;
	const u8 *c = (litStatic && p == litPack) ? litStatic[prop] : p->staticCubes ? p->staticCubes[prop] : NULL;
	return (c && cube_ok(c)) ? c : NULL;
}

void Light_Frame(void) { budget = RELIGHT_BUDGET; }

int Light_EntityCube(const Pack *p, s32 ent, const f32 pos[3], u8 out[18])
{
	if (p != litPack) {
		/* (a pack other than the map's, e.g. a host test that never called Light_Init) */
		return leaf_cube(p, pos, out);
	}
	if (ent < 0) {
		if (!leaf_cube(p, pos, out))
			return 0;
		Light_AddDirect(p, pos, out);
		return 1;
	}
	if ((u32)ent >= entLightCap) {
		u32 cap = (u32)ent + 64;
		EntLight *n = realloc(entLight, cap * sizeof(EntLight));
		if (!n)
			return leaf_cube(p, pos, out);
		memset(n + entLightCap, 0, (cap - entLightCap) * sizeof(EntLight));
		entLight = n;
		entLightCap = cap;
	}
	EntLight *e = &entLight[ent];
	f32 dx = pos[0] - e->at[0], dy = pos[1] - e->at[1], dz = pos[2] - e->at[2];
	int moved = dx * dx + dy * dy + dz * dz > RELIGHT_MOVE * RELIGHT_MOVE;
	if (e->ok && e->age < 255)
		e->age++;
	if (!e->ok || ((moved || e->age >= RELIGHT_FRAMES) && budget > 0)) {
		u8 c[18];
		if (!leaf_cube(p, pos, c)) {
			e->ok = 0;
			/* (no light baked here, like the G-Man's black void: only the dynamic lights, if any reach) */
			memset(out, 0, 18);
			return Light_AddDynamic(pos, out);
		}
		/* the first time always (nothing to show yet); after that within the frame's budget */
		Light_AddDirect(p, pos, c);
		if (e->ok)
			budget -= traceCost > 0 ? traceCost : 1;
		memcpy(e->cube, c, 18);
		memcpy(e->at, pos, sizeof(e->at));
		e->ok = 1;
		e->age = 0;
	}
	memcpy(out, e->cube, 18);
	Light_AddDynamic(pos, out);
	return 1;
}
