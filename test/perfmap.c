/* Host profile of a map as the game runs it: 66 Hz ticks, a frame every ~2 ticks from the player's eye
 * (turning), timing ticks and frames separately; plus what each frame sends to the GPU.
 * usage: perfmap map.hgm [seconds] */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "light.h"
#include "fx.h"
#include "gibs.h"
#include <time.h>
#include "gxv.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int rt_init(const Pack *p);
void rt_frame(const Pack *p, const f32 eye[3], f32 pitch, f32 yaw, int flashlight);
extern unsigned long rtWorld, rtModel, rtNpc, rtFrames, rtFaces;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	if (getenv("NOSPCL")) P.spClusterRange = NULL;
	f32 secs = argc > 2 ? (f32)atof(argv[2]) : 20;
	u32 sz;
	u8 *c = chunk("TEXI", &sz);
	if (c) { swap(c, 1, "4"); P.numTex = *(u32 *)c; swap(c + 4, P.numTex, "221111" "44"); P.tex = (PackTexInfo *)(c + 4); }
	P.texData = chunk("TEXD", &sz);
	c = chunk("VISD", &sz);
	if (c) { swap(c, 1, "4"); P.numClusters = *(u32 *)c; swap(c + 4, P.numClusters, "4"); P.visOffsets = (u32 *)(c + 4); P.visBase = c + 4 + 4 * P.numClusters; }
	CM_Init(&P); Light_Init(&P); Fx_Init(&P);
	static Player pl;
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Gibs_Init(&P);
	if (rt_init(&P) != 0) return 1;
	gxv.maxDist = 40000;
	UserCmd cmd = {0};
	double tTick = 0, tFrame = 0;
	int ticks = 0, frames = 0;
	unsigned long maxV = 0, maxD = 0;
	for (int t = 0; t < (int)(secs * 66); t++) {
		double a = now_s();
		Player_Tick(&pl, &cmd);
		Ent_Tick(1.0f / 66);
		Gibs_Tick(1.0f / 66);
		tTick += now_s() - a; ticks++;
		if (t % 2) continue;
		f32 eye[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64};
		unsigned long v0 = rtWorld + rtModel + rtNpc, d0 = rtFaces;
		a = now_s();
		rt_frame(&P, eye, 0, (f32)(t * 2 % 360), 0);
		tFrame += now_s() - a; frames++;
		unsigned long v = rtWorld + rtModel + rtNpc - v0, d = rtFaces - d0;
		if (v > maxV) maxV = v;
		if (d > maxD) maxD = d;
	}
	printf("%s: tick %.3f ms, frame %.3f ms (host); per frame world %lu props %lu npcs %lu verts, %lu draws; worst %lu verts %lu draws\n",
	       argv[1], tTick / ticks * 1e3, tFrame / frames * 1e3, rtWorld / rtFrames, rtModel / rtFrames, rtNpc / rtFrames,
	       rtFaces / rtFrames, maxV, maxD);
	return 0;
}
