/*
 * Host test: runs the real renderer (render.c, world.c, gibs.c) against the GX validator in
 * test/gxv and reports draws whose vertex stream doesn't match the vertex descriptor, vertices
 * that come out absurdly far from the eye, and position matrices that were never loaded.
 * usage: rendertest map.hgm [seconds]
 */
#include "light.h"
#include "fx.h"
#ifdef RENDER_MODEL_STATS
u32 rtModelVerts[4096];
#endif
#include "ent.h"
#include "collide.h"
#include "npc.h"
#include "gibs.h"
#define main sim_main
#include "sim.c"
#undef main
#include "gxv/gxv.h"

/* compiled against the GX validator's headers in a separate unit */
extern int rt_init(const Pack *p);
extern void rt_frame(const Pack *p, const f32 eye[3], f32 pitch, f32 yaw, int flashlight);
extern int rt_intro(const Pack *p, const f32 eye[3], f32 pitch, f32 yaw);

static void cl(const char *m, const char *l) { (void)m; (void)l; }

int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	f32 secs = argc > 2 ? (f32)atof(argv[2]) : 30;
	/* the chunks only the renderer reads */
	u32 sz;
	u8 *c = chunk("TEXI", &sz);
	if (c) {
		swap(c, 1, "4");
		P.numTex = *(u32 *)c;
		swap(c + 4, P.numTex, "221111" "44");
		P.tex = (PackTexInfo *)(c + 4);
	}
	P.texData = chunk("TEXD", &sz);
	c = chunk("VISD", &sz);
	if (c) {
		swap(c, 1, "4");
		P.numClusters = *(u32 *)c;
		swap(c + 4, P.numClusters, "4");
		P.visOffsets = (u32 *)(c + 4);
		P.visBase = c + 4 + 4 * P.numClusters;
	}
	CM_Init(&P);
	Light_Init(&P);
	Fx_Init(&P);
	static Player pl;
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Gibs_Init(&P);
	if (rt_init(&P) != 0) { printf("renderer init failed\n"); return 1; }
	gxv.maxDist = 40000;
	u32 n;
	int frames = 0, introFrames = 0, introCam = 0;
	unsigned introModes = 0;
	for (int t = 0; t < (int)(secs * 66); t++) {
		Ent_Tick(1.0f / 66);
		Gibs_Tick(1.0f / 66);
		if (t == 66) {
			/* break something so gibs get drawn too */
			f32 o[3] = {pl.origin[0] + 64, pl.origin[1], pl.origin[2] + 32}, mn[3], mx[3], v[3] = {0, 0, 100};
			for (int k = 0; k < 3; k++) { mn[k] = o[k] - 16; mx[k] = o[k] + 16; }
			u8 tint[4] = {255, 255, 255, 255};
			Gibs_BreakBox(1, mn, mx, v, tint);
		}
		if (getenv("GMAN") && t == 66 * 20) {
			Entity *e = Ent_List(&n);
			f32 eye[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64};
			unsigned long v0 = gxv.vertices;
			rt_frame(&P, eye, 0, pl.yaw, 0);
			unsigned long with = gxv.vertices - v0;
			s32 gi = -1;
			for (u32 i = 1; i < n; i++) if (e[i].name && !strcmp(e[i].name, "gman")) gi = (s32)i;
			e[gi].flags |= EF_DISABLED;
			v0 = gxv.vertices;
			rt_frame(&P, eye, 0, pl.yaw, 0);
			e[gi].flags &= ~EF_DISABLED;
			printf("player view at 20 s: %lu vertices, %lu without the G-Man\n", with, gxv.vertices - v0);
			return 0;
		}
		if (t % 33)
			continue;
		{
			f32 vo[3], va[3];
			if (Ent_ViewOverride(vo, va)) {
				int r = rt_intro(&P, vo, va[0], va[1]);
				if (r) introFrames++, introModes |= 1u << ((r - 1) % 100 / 10), introCam += (r - 1) >= 100;
			}
		}
		/* cameras: the player, every NPC / prop actor, and moving brush entities */
		Entity *e = Ent_List(&n);
		f32 eye[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64};
		gxvTag = "player";
		for (int y = 0; y < 360; y += 90) { rt_frame(&P, eye, 0, (f32)y, frames & 1); frames++; }
		for (u32 i = 1; i < n; i++) {
			if (e[i].flags & EF_KILLED) continue;
			if (strcmp(e[i].classname, "func_tracktrain") && strcmp(e[i].classname, "npc_citizen") &&
			    strcmp(e[i].classname, "prop_dynamic") && strcmp(e[i].classname, "point_viewcontrol"))
				continue;
			if ((t / 33 + i) % 7) continue; /* a sample of them each time */
			f32 eo[3] = {e[i].origin[0], e[i].origin[1], e[i].origin[2] + 48};
			gxvTag = e[i].classname;
			rt_frame(&P, eo, 0, e[i].angles[1], frames & 1);
			frames++;
		}
	}
	printf("intro: %d frames composed (%d with a camera view), modes 0x%x\n", introFrames, introCam, introModes);
	extern unsigned long rtWorld, rtModel, rtNpc, rtFrames, rtFaces;
#ifdef RENDER_MODEL_STATS
	{
		extern u32 rtModelVerts[4096];
		for (int k = 0; k < 12; k++) {
			u32 best = 0; int bi = -1;
			for (int m = 0; m < 4096; m++) if (rtModelVerts[m] > best) best = rtModelVerts[m], bi = m;
			if (bi < 0) break;
			printf("model %d: %lu vertices a frame (model has %u, far %d)\n", bi, (unsigned long)(best / (rtFrames ? rtFrames : 1)), P.models[bi].numVerts,
			       P.modelFar && (u32)bi < P.numModelFar ? (P.modelFar[bi] == 0xFFFF ? -1 : (int)P.modelFar[bi]) : -1);
			rtModelVerts[bi] = 0;
		}
	}
#endif
	{ extern unsigned long rtSkyFaces; if (rtFrames) printf("3D skybox: %lu faces visible a frame\n", rtSkyFaces / rtFrames); }
	if (rtFrames) printf("per frame: world %lu, props %lu, npcs %lu vertices; %lu draws\n", rtWorld / rtFrames, rtModel / rtFrames, rtNpc / rtFrames, rtFaces / rtFrames);
	printf("%s: %d frames, %lu draws, %lu vertices, %lu errors, %lu far vertices, %lu stray bytes, %lu overflow\n",
	       argv[1], frames, gxv.draws, gxv.vertices, gxv.errors, gxv.farVerts, gxv.strayBytes, gxv.overflow);
	return 0;
}
