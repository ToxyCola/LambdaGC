/* Host test for physics props: drop every physics prop from 48 units up and check it settles on
 * something (not in solid, not fallen through), then push one by walking into it. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	static Player pl; Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	u32 n; Entity *e = Ent_List(&n);
	int count = 0, settled = 0, fell = 0, solid = 0, broke = 0; f32 maxTilt = 0;
	f32 start[4096][3];
	for (u32 i = 1; i < n && i < 4096; i++) {
		if (e[i].mdl < 0 || strncmp(e[i].classname, "prop_physics", 12) || (e[i].spawnflags & 8)) continue;
		memcpy(start[i], e[i].origin, 12);
		/* lift it: move the entity and its body up */
		count++;
	}
	/* lift all by teleporting through the entity and re-initialising the bodies */
	for (u32 i = 1; i < n && i < 4096; i++)
		if (e[i].mdl >= 0 && !strncmp(e[i].classname, "prop_physics", 12) && !(e[i].spawnflags & 8)) e[i].origin[2] += 48;
	Phys_Init(&P, &pl);
	for (u32 i = 1; i < n; i++) Phys_Wake((s32)i);
	pl.origin[2] += 10000; /* keep the player out of the way */
	for (int t = 0; t < 66 * 6; t++) Ent_Tick(TICK_INTERVAL);
	for (u32 i = 1; i < n && i < 4096; i++) {
		if (e[i].mdl < 0 || strncmp(e[i].classname, "prop_physics", 12) || (e[i].spawnflags & 8)) continue;
		const PackModel *m = &P.models[e[i].mdl];
		f32 c[3]; for (int k = 0; k < 3; k++) c[k] = e[i].origin[k] + e[i].R[k][0]*(m->mins[0]+m->maxs[0])*0.5f + e[i].R[k][1]*(m->mins[1]+m->maxs[1])*0.5f + e[i].R[k][2]*(m->mins[2]+m->maxs[2])*0.5f;
		f32 dz = e[i].origin[2] - start[i][2];
		if (e[i].flags & EF_KILLED) { broke++; continue; } /* (glass that broke landing: Source's impact damage) */
		if (dz < -200) fell++;
		else if (CM_PointContents(c) & CONTENTS_SOLID) solid++;
		else if (fabsf(dz) < 40) settled++;
		f32 tilt = acosf(fminf(1, fabsf(e[i].R[2][2]))) * 57.3f; if (tilt > maxTilt && fabsf(dz) < 40) maxTilt = tilt;
	}
	printf("physics props %d: settled near their spot %d, centre in solid %d, fell out %d, broke landing %d\n", count, settled, solid, fell, broke);
	/* push test: first light prop, player 40 units behind it, walking at it */
	pl.origin[2] -= 10000;
	for (u32 i = 1; i < n; i++) {
		if (e[i].mdl < 0 || strncmp(e[i].classname, "prop_physics", 12) || (e[i].flags & EF_KILLED)) continue;
		const char *ms = Ent_KV((s32)i, "_mass"); if (!ms || atof(ms) > 40) continue;
		f32 o[3]; memcpy(o, e[i].origin, 12);
		Player_Spawn(&pl, (f32[3]){o[0] - 48, o[1], o[2] + 2}, 0, 0);
		if (pl.origin[0] != o[0] - 48) continue; /* spawn spot moved: try another */
		UserCmd go = {1.0f, 0, 0, 0, 0};
		Phys_SetWish(190, 0);
		for (int t = 0; t < 66; t++) { Player_Tick(&pl, &go); Ent_Tick(TICK_INTERVAL); }
		Phys_SetWish(0, 0);
		printf("pushed %s (mass %s): moved %.0f units, player moved %.0f\n", e[i].classname, ms,
		       sqrtf((e[i].origin[0]-o[0])*(e[i].origin[0]-o[0]) + (e[i].origin[1]-o[1])*(e[i].origin[1]-o[1])), pl.origin[0] - (o[0] - 48));
		/* pick it up */
		f32 eye[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64}, fwd[3] = {1, 0, -0.3f};
		Phys_SetView(eye, fwd);
		int took = Phys_TryPickup((s32)i);
		for (int t = 0; t < 66; t++) { Phys_SetView(eye, fwd); Ent_Tick(TICK_INTERVAL); }
		printf("pickup %d, held at %.0f units from the eye, height above feet %.0f\n", took,
		       sqrtf((e[i].origin[0]-eye[0])*(e[i].origin[0]-eye[0])+(e[i].origin[1]-eye[1])*(e[i].origin[1]-eye[1])), e[i].origin[2] - pl.origin[2]);
		Phys_Drop(1);
		for (int t = 0; t < 66 * 3; t++) Ent_Tick(TICK_INTERVAL);
		printf("thrown: landed %.0f units away\n", sqrtf((e[i].origin[0]-eye[0])*(e[i].origin[0]-eye[0])+(e[i].origin[1]-eye[1])*(e[i].origin[1]-eye[1])));
		break;
	}
	return 0;
}
