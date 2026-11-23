/* Host test for milestone 7's world systems on real converted maps: the G-Man intro's script_intro,
 * swimming and drowning, floating props, break pieces, the airboat and the buggy, and carrying a
 * vehicle across a level change. Prints counts and measurements only.
 * usage: worldtest <maps dir> */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "gibs.h"
#include "npc.h"
#include "physics.h"
#include "vehicle.h"
#include "weapons.h"

static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static UserCmd idle;

static Entity *ent_at_test(s32 i)
{
	u32 n;
	Entity *e = Ent_List(&n);
	return (i > 0 && (u32)i < n) ? &e[i] : NULL;
}

static int open_map(const char *dir, const char *name, int newGame)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.hgm", dir, name);
	Ent_Free();
	if (load(&P, path)) {
		printf("%s: missing\n", name);
		return -1;
	}
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl, Weap_Strip, Weap_Hurt};
	if (Ent_Spawn(&P, &pl, newGame, &cb) != 0)
		return -1;
	Weap_LevelStart(&P, &pl);
	Veh_LevelStart(&P, &pl);
	return 0;
}

static void tick(int n, const UserCmd *c)
{
	for (int i = 0; i < n; i++) {
		Player_Tick(&pl, c ? c : &idle);
		Ent_Tick(TICK_INTERVAL);
		Weap_Tick(TICK_INTERVAL);
	}
}

static s32 find_class(const char *cls, int enabledOnly)
{
	u32 n;
	Entity *e = Ent_List(&n);
	for (u32 i = 1; i < n; i++)
		if (!strcmp(e[i].classname, cls) && (!enabledOnly || !(e[i].flags & (EF_DISABLED | EF_KILLED))))
			return (s32)i;
	return -1;
}

/* a point on the surface of some water in the map, with room above it */
static int water_spot(f32 out[3])
{
	f32 best = 0;
	for (u32 i = 0; i < P.numFaces; i++) {
		const PackFace *f = &P.faces[i];
		if (P.mats[f->mat].kind != MAT_WATER || f->numverts < 3)
			continue;
		f32 c[3] = {0, 0, 0};
		for (u32 k = 0; k < f->numverts; k++)
			for (int a = 0; a < 3; a++)
				c[a] += P.verts[f->firstvert + k].pos[a] / f->numverts;
		f32 above[3] = {c[0], c[1], c[2] + 40};
		if (CM_PointContents(above) & (MASK_WATER | CONTENTS_SOLID))
			continue;
		f32 depth = 0;
		for (f32 d = 4; d < 400; d += 4) {
			f32 p[3] = {c[0], c[1], c[2] - d};
			if (!(CM_PointContents(p) & MASK_WATER))
				break;
			depth = d;
		}
		if (depth > best) {
			best = depth;
			memcpy(out, c, 12);
		}
	}
	if (getenv("DBG")) printf("deepest water %.0f\n", best);
	return best >= 40;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* ---- the intro */
	if (open_map(dir, "d1_trainstation_01", 1) == 0) {
		int passes2 = 0, modes = 0, camViews = 0, fadeFrames = 0;
		f32 maxFade = 0;
		s32 gman = -1;
		u32 n;
		Entity *ents = Ent_List(&n);
		for (u32 i = 1; i < n; i++)
			if (ents[i].name && !strcmp(ents[i].name, "gman"))
				gman = (s32)i;
		f32 worstFacing = 1, facingSamples = 0, facingSum = 0;
		for (int t = 0; t < 66 * 62; t++) {
			tick(1, NULL);
			IntroView iv;
			if (Ent_IntroView(&iv)) {
				passes2 += iv.passes == 2;
				modes |= 1 << iv.mode[0];
				if (iv.passes == 2)
					modes |= 1 << iv.mode[1];
				camViews += iv.hasCamera;
				if (iv.fade[3] > 0)
					fadeFrames++;
				if (iv.fade[3] > maxFade)
					maxFade = iv.fade[3];
			}
			f32 vo[3], va[3];
			if (gman > 0 && Ent_ViewOverride(vo, va) && t % 33 == 0) {
				/* does he face the camera? (his forward vs the direction to the viewer) */
				Entity *g = &ents[gman];
				f32 d[2] = {vo[0] - g->origin[0], vo[1] - g->origin[1]};
				f32 l = sqrtf(d[0] * d[0] + d[1] * d[1]);
				if (l > 1 && l < 400) {
					f32 dot = (g->R[0][0] * d[0] + g->R[1][0] * d[1]) / l;
					facingSum += dot;
					facingSamples++;
					if (dot < worstFacing)
						worstFacing = dot;
				}
			}
		}
		printf("intro: crossfades %d ticks, blend modes seen 0x%x, camera-view ticks %d, fade ticks %d (max %.0f)\n",
		       passes2, modes, camViews, fadeFrames, maxFade);
		printf("intro: G-Man faces the camera: mean %.2f, worst %.2f over %.0f samples (1 = straight at it)\n",
		       facingSamples ? facingSum / facingSamples : 0, worstFacing, facingSamples);
	}

	/* ---- water: swimming, drowning, floating */
	if (open_map(dir, getenv("WMAP") ? getenv("WMAP") : "d1_canals_06", 0) == 0) {
		f32 w[3];
		if (water_spot(w)) {
			f32 at[3] = {w[0], w[1], w[2] - 30};
			Player_Spawn(&pl, at, 0, 0);
			tick(66, NULL);
			printf("water: player at the surface: water level %d (type %x)\n", pl.waterLevel, pl.waterType);
			UserCmd dive = {1, 0, 0, 60, 0};
			tick(120, &dive);
			printf("water: swimming down for 2 s: level %d, depth %.0f below the surface\n", pl.waterLevel, w[2] - pl.origin[2]);
			Weap_NewGame();
			Weap_GiveAll(); /* suit on, aux full */
			f32 h0 = 100;
			UserCmd stay = {0, 0, 0, 60, 0};
			int lvl3 = 0;
			for (int i = 0; i < 66 * 12; i++) {
				tick(1, &stay);
				lvl3 += pl.waterLevel == 3;
			}
			printf("water: 12 s under with the suit: under %d ticks, health %d (from %.0f), aux %d\n", lvl3,
			       (int)Weap_DebugHealth(), h0, Weap_DebugAux());
			for (int i = 0; i < 66 * 15; i++)
				tick(1, &stay);
			printf("water: 15 s more: health %d, aux %d\n", (int)Weap_DebugHealth(), Weap_DebugAux());
		} else
			printf("water: no water surface found\n");
		/* floating props */
		u32 n;
		Entity *ents = Ent_List(&n);
		tick(66 * 5, NULL);
		int inWater = 0, floating = 0;
		for (u32 i = 1; i < n; i++) {
			if (!Phys_IsBody((s32)i))
				continue;
			f32 c[3] = {ents[i].origin[0], ents[i].origin[1], ents[i].origin[2]};
			f32 a[3] = {c[0], c[1], c[2] + 24};
			if (CM_PointContents(c) & MASK_WATER || CM_PointContents(a) & MASK_WATER) {
				inWater++;
				if (!(CM_PointContents(a) & MASK_WATER))
					floating++;
			}
		}
		printf("water: %d physics props in water after 5 s, %d riding at the surface\n", inWater, floating);
	}

	/* ---- break pieces */
	if (open_map(dir, "d1_canals_01", 1) == 0) {
		u32 n;
		Entity *ents = Ent_List(&n);
		int broke = 0;
		for (u32 i = 1; i < n && broke < 4; i++) {
			const char *hp = Ent_KV((s32)i, "_hp");
			if (ents[i].cls != EC_PROP || !hp || (ents[i].flags & (EF_KILLED | EF_DISABLED)) || Ent_KV((s32)i, "_boom"))
				continue;
			f32 dir3[3] = {1, 0, 0};
			Ent_Damage((s32)i, 1000, dir3, ents[i].origin);
			broke += (ents[i].flags & EF_KILLED) != 0;
		}
		int pieces = Gibs_Count();
		tick(66 * 4, NULL);
		printf("gibs: %d props broken, %d pieces, %d still around after 4 s (gib sets in map: %u)\n", broke, pieces,
		       Gibs_Count(), P.numGibSets);
	}

	/* ---- the airboat */
	if (open_map(dir, "d1_canals_05", 1) == 0) {
		tick(66, NULL);
		s32 boat = find_class("prop_vehicle_airboat", 1);
		u32 n;
		Entity *ents = Ent_List(&n);
		if (boat > 0) {
			f32 start[3];
			memcpy(start, ents[boat].origin, 12);
			int in = Veh_TryEnter(boat);
			Veh_Controls(1, 0, 0, 0, 0);
			f32 maxSpeed = 0, prev[3];
			memcpy(prev, ents[boat].origin, 12);
			int onWater = 0;
			for (int i = 0; i < 66 * 6; i++) {
				tick(1, NULL);
				f32 d = sqrtf((ents[boat].origin[0] - prev[0]) * (ents[boat].origin[0] - prev[0]) +
				              (ents[boat].origin[1] - prev[1]) * (ents[boat].origin[1] - prev[1])) / TICK_INTERVAL;
				memcpy(prev, ents[boat].origin, 12);
				if (d > maxSpeed)
					maxSpeed = d;
				f32 below[3] = {ents[boat].origin[0], ents[boat].origin[1], ents[boat].origin[2] - 2};
				onWater += (CM_PointContents(below) & MASK_WATER) != 0;
				if (getenv("DBG") && i % 33 == 0) printf("   t=%.1f pos %.0f %.0f %.0f\n", i * TICK_INTERVAL, ents[boat].origin[0], ents[boat].origin[1], ents[boat].origin[2]);
			}
			f32 dist = sqrtf((ents[boat].origin[0] - start[0]) * (ents[boat].origin[0] - start[0]) +
			                 (ents[boat].origin[1] - start[1]) * (ents[boat].origin[1] - start[1]));
			printf("airboat: entered %d, 6 s full throttle: %.0f units, top speed %.0f u/s, on water %d of 396 ticks\n",
			       in, dist, maxSpeed, onWater);
			Veh_Controls(-1, 1, 0, 0, 0);
			tick(66 * 2, NULL);
			printf("airboat: braking and turning: heading changed, still driving %d\n", Veh_Driving() == boat);
			/* carry it into the next level */
			f32 lm[3] = {ents[boat].origin[0] - 100, ents[boat].origin[1], ents[boat].origin[2]};
			Veh_PrepareCarry(lm);
			if (open_map(dir, "d1_canals_06", 0) == 0) {
				f32 lm2[3] = {0, 0, 0};
				f32 w[3];
				if (water_spot(w))
					memcpy(lm2, w, 12), lm2[0] -= 100;
				Veh_FinishCarry(lm2);
				printf("airboat: carried into d1_canals_06: driving %d\n", Veh_Driving() > 0);
				s32 b2 = Veh_Driving();
				Entity *e2 = ent_at_test(b2);
				if (e2) {
					f32 s2[3];
					memcpy(s2, e2->origin, 12);
					Veh_Controls(1, 0.3f, 0, 0, 0);
					int wet = 0;
					f32 top = 0, pz = e2->origin[2];
					for (int i = 0; i < 66 * 5; i++) {
						f32 o[3];
						memcpy(o, e2->origin, 12);
						tick(1, NULL);
						f32 sp = sqrtf((e2->origin[0] - o[0]) * (e2->origin[0] - o[0]) + (e2->origin[1] - o[1]) * (e2->origin[1] - o[1])) / TICK_INTERVAL;
						top = sp > top ? sp : top;
						f32 below[3] = {e2->origin[0], e2->origin[1], e2->origin[2] - 2};
						wet += (CM_PointContents(below) & MASK_WATER) != 0;
					}
					printf("airboat: 5 s on open water: %.0f units, top speed %.0f u/s, in the water %d of 330 ticks, height change %.0f\n",
					       sqrtf((e2->origin[0] - s2[0]) * (e2->origin[0] - s2[0]) + (e2->origin[1] - s2[1]) * (e2->origin[1] - s2[1])), top, wet, e2->origin[2] - pz);
				}
			}
		} else
			printf("airboat: not found\n");
	}

	/* ---- the buggy */
	if (open_map(dir, "d2_coast_01", 1) == 0) {
		tick(66, NULL);
		s32 jeep = -1;
		u32 n;
		Entity *ents = Ent_List(&n);
		for (u32 i = 1; i < n; i++) {
			const char *m = Ent_KV((s32)i, "model");
			if (ents[i].cls == EC_VEHICLE && m && strstr(m, "buggy") && !(ents[i].flags & (EF_DISABLED | EF_KILLED)))
				jeep = (s32)i;
		}
		if (jeep > 0) {
			f32 start[3];
			memcpy(start, ents[jeep].origin, 12);
			int in = Veh_TryEnter(jeep);
			Veh_Controls(1, 0.2f, 0, 0, VB_TURBO);
			f32 minZ = 1e9f, maxZ = -1e9f;
			for (int i = 0; i < 66 * 6; i++) {
				tick(1, NULL);
				if (ents[jeep].origin[2] < minZ)
					minZ = ents[jeep].origin[2];
				if (ents[jeep].origin[2] > maxZ)
					maxZ = ents[jeep].origin[2];
			}
			f32 dist = sqrtf((ents[jeep].origin[0] - start[0]) * (ents[jeep].origin[0] - start[0]) +
			                 (ents[jeep].origin[1] - start[1]) * (ents[jeep].origin[1] - start[1]));
			printf("buggy: entered %d, 6 s of throttle: %.0f units, height range %.0f..%.0f (started %.0f)\n", in, dist,
			       minZ, maxZ, start[2]);
			Veh_Controls(0, 0, 0, 0, VB_EXIT);
			tick(66 * 3, NULL);
			Veh_Controls(0, 0, 0, 0, 0);
			tick(2, NULL);
			Veh_Controls(0, 0, 0, 0, VB_EXIT);
			tick(2, NULL);
			printf("buggy: got out %d, player standing clear %d\n", Veh_Driving() < 0, !pl.frozen);
		} else
			printf("buggy: not found\n");
	}
	return 0;
}
