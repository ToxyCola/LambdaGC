/* Host test: Trainstation 2's food queue moves up (front citizen leaves for the exit, the others step forward),
 * and Trainstation 1's turnstile turns when the player walks into it and lets him through. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static u32 cnt;
static Entity *named(const char *n) { Entity *e = Ent_List(&cnt); for (u32 i = 1; i < cnt; i++) if (e[i].name && !strcasecmp(e[i].name, n)) return &e[i]; return NULL; }
static void go(f32 secs, f32 fwd, f32 yaw) { UserCmd c = {0}; c.forwardmove = fwd; c.yaw = yaw;
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c);
		f32 cy = cosf(yaw * (f32)M_PI / 180), sy = sinf(yaw * (f32)M_PI / 180); Phys_SetWish(cy * fwd * 190, sy * fwd * 190);
		Ent_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Entity *ts = named("turnstyle_1");
	if (ts) {
		for (int dir = 0; dir < 8; dir++) {
			f32 th = dir * 45.0f * (f32)M_PI / 180;
			f32 side = 16; /* aim off-centre so the bars get pushed round */
			f32 at[3] = {ts->hingePivot[0] - cosf(th) * 72 - sinf(th) * side, ts->hingePivot[1] - sinf(th) * 72 + cosf(th) * side,
			             ts->origin[2] + P.bmodels[ts->bmodel].mins[2] + 2};
			Player_Spawn(&pl, at, 0, 0);
			f32 a0 = ts->angles[1], p0[3]; memcpy(p0, pl.origin, 12);
			for (int k = 0; k < 6; k++) { go(0.5f, 1, dir * 45.0f); if (0) printf("    vel %.1f ang %.1f player (%.0f %.0f) r %.0f\n", ts->hingeVel, ts->angles[1], pl.origin[0]-ts->hingePivot[0], pl.origin[1]-ts->hingePivot[1], sqrtf((pl.origin[0]-ts->hingePivot[0])*(pl.origin[0]-ts->hingePivot[0])+(pl.origin[1]-ts->hingePivot[1])*(pl.origin[1]-ts->hingePivot[1]))); }
			f32 along = (pl.origin[0] - p0[0]) * cosf(th) + (pl.origin[1] - p0[1]) * sinf(th);
			printf("turnstile from %3d deg: start off by (%.0f %.0f), turned %4.0f deg, got %4.0f units along\n", dir * 45,
			       p0[0] - at[0], p0[1] - at[1], ts->angles[1] - a0, along);
		}
	}
	Entity *relay = named("relay_setup_food_queue");
	if (relay) {
		Entity *q[3] = {named("citizen_queue_start_1"), named("citizen_queue_start_2"), named("citizen_queue_start_3")};
		Ent_FireOutput((s32)(relay - Ent_List(&cnt)), "ontrigger", 0);
		go(8, 0, 0);
		for (int k = 0; k < 3; k++) printf("queue: %s at (%.0f %.0f)\n", q[k]->name, q[k]->origin[0], q[k]->origin[1]);
		Entity *st = named("trigger_start_food_queue");
		if (st) Ent_FireOutput((s32)(st - Ent_List(&cnt)), "ontrigger", 0);
		go(30, 0, 0);
		for (int k = 0; k < 3; k++) printf("queue after MoveQueueUp: %s at (%.0f %.0f)\n", q[k]->name, q[k]->origin[0], q[k]->origin[1]);
	}
	return 0;
}
