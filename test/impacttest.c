/* Bullet impacts: shoot the walls around the start of a map with the real weapons code and pack and
 * check the holes land on the surfaces (solid just behind, open just in front), what kinds of
 * surface they hit, and that the dust clears. usage: impacttest <map.hgm> <weapons.hgm> */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "../engine/source/weapons.c"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static Pack W;
int main(int argc, char **argv)
{
	if (argc < 3 || load(&W, argv[2])) return 1;
	u32 fsz = 0, msz = 0;
	u8 *fc = chunk("SURF", &fsz), *mc = chunk("SMAT", &msz);
	if (fc) { surfDefault = (u32)(fc[2] << 8 | fc[3]); surfFlesh = (u32)(fc[4] << 8 | fc[5]); }
	if (mc) { u8 *cp = malloc(msz); memcpy(cp, mc, msz); surfMat = cp + 2; numSurfMat = (u32)(mc[0] << 8 | mc[1]); }
	printf("surface materials: %u\n", numSurfMat);
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	Ent_Spawn(&P, &pl, 1, &cb);
	map = &P; player = &pl;
	memcpy(eye, pl.origin, 12); eye[2] += 64;
	int hist[128] = {0}, shots = 0;
	for (int i = 0; i < 64; i++) {
		f32 yaw = i * 2 * (f32)M_PI / 64, pitch = ((i % 5) - 2) * 0.25f;
		f32 d[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
		f32 end[3] = {eye[0] + d[0] * 8192, eye[1] + d[1] * 8192, eye[2] + d[2] * 8192};
		Trace tr; trace_line(&tr, eye, end, 0);
		if (tr.fraction < 1 && tr.surface != SURF_SKY) hist[surface_material(tr.ent > 0 ? Weap_EntSurface(tr.ent) : tr.surface) & 127]++;
		shoot_bullet(d, 1);
		shots++;
	}
	int nd = 0, np = Weap_ImpactCounts(&nd), onSurf = 0;
	for (int i = 0; i < MAX_DECALS; i++) {
		if (!decals[i].alive) continue;
		f32 n[3] = {decals[i].u[1] * decals[i].v[2] - decals[i].u[2] * decals[i].v[1], decals[i].u[2] * decals[i].v[0] - decals[i].u[0] * decals[i].v[2], decals[i].u[0] * decals[i].v[1] - decals[i].u[1] * decals[i].v[0]};
		f32 front[3], back[3];
		for (int k = 0; k < 3; k++) { front[k] = decals[i].p[k] + n[k] * 1; back[k] = decals[i].p[k] - n[k] * 2; }
		onSurf += !(CM_PointContents(front) & CONTENTS_SOLID) && (CM_PointContents(back) & CONTENTS_SOLID);
	}
	printf("%d shots: %d holes (%d sitting on a solid surface), %d dust/spark puffs\n", shots, nd, onSurf, np);
	printf("surfaces hit:");
	for (int c = 'A'; c <= 'Z'; c++) if (hist[c]) printf(" %c=%d", c, hist[c]);
	printf("\n");
	for (int t = 0; t < 66 * 2; t++) tick_puffs(1.0f / 66);
	np = Weap_ImpactCounts(&nd);
	printf("after 2 s: %d holes, %d puffs\n", nd, np);
	return 0;
}
