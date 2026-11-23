/* Host test: get into each vehicle on the map and drive: the view looks where the vehicle goes (it used to
 * face backwards), the eyes face the way the model's front is, and it keeps moving (doesn't get stuck) */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "vehicle.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static u32 cnt;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Veh_LevelStart(&P, &pl);
	int bad = 0, tried = 0;
	Entity *ents = Ent_List(&cnt);
	for (u32 i = 1; i < cnt; i++) {
		if (ents[i].cls != EC_VEHICLE || (ents[i].flags & EF_DISABLED)) continue;
		ents[i].locked = 0;
		if (!Veh_TryEnter((s32)i)) continue;
		tried++;
		f32 o0[3]; memcpy(o0, ents[i].origin, 12);
		f32 eye[3], p, y, r;
		Veh_View(eye, &p, &y, &r);
		/* the driver's eye attachment looks the same way as the view */
		Npc *n = Npc_OfEntity((s32)i);
		f32 ep[3], R[3][3];
		f32 attYaw = (n && Npc_Attachment(n, "vehicle_driver_eyes", ep, R)) ? atan2f(R[1][0], R[0][0]) * 180 / (f32)M_PI : y;
		f32 moved = 0, maxStill = 0, still = 0, along = 0;
		for (int t = 0; t < (int)(4 / TICK_INTERVAL); t++) {
			Veh_Controls(1, 0, 0, 0, 0);
			f32 a[3]; memcpy(a, ents[i].origin, 12);
			Veh_Tick(TICK_INTERVAL);
			Ent_Tick(TICK_INTERVAL);
			f32 dx = ents[i].origin[0] - a[0], dy = ents[i].origin[1] - a[1];
			f32 d = sqrtf(dx * dx + dy * dy);
			moved += d;
			Veh_View(eye, &p, &y, &r);
			along += dx * cosf(y * (f32)M_PI / 180) + dy * sinf(y * (f32)M_PI / 180);
			if (d < 0.05f && t > 30) { still += TICK_INTERVAL; if (still > maxStill) maxStill = still; } else still = 0;
		}
		f32 dyaw = fmodf(fabsf(attYaw - y) + 540, 360) - 180;
		int ok = along > 0.5f * moved && fabsf(dyaw) < 5;
		printf("%s %s: eyes vs view %.0f deg, drove %.0f units, %.0f of it the way the view looks, longest stop %.1f s: %s\n",
		       argv[1], ents[i].classname, dyaw, moved, along, maxStill, ok ? "ok" : "WRONG");
		bad += !ok;
		Veh_Controls(0, 0, 0, 0, 0);
		for (int t = 0; t < 120; t++) { Veh_Tick(TICK_INTERVAL); }
		Veh_LevelStart(&P, &pl);
		memcpy(ents[i].origin, o0, 12);
	}
	printf("%d vehicles, %d wrong\n", tried, bad);
	return bad != 0;
}
