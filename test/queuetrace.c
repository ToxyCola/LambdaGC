/* Host test: Trainstation 2's food queue and the station's walkers: no NPC jumps (teleports) or climbs
 * onto furniture while walking (z above where it started by more than a step) */
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
static f32 last[2048][3], z0[2048];
static int jumps, climbs;
static void tick(f32 secs) {
	UserCmd c = {0};
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL);
		Entity *e = Ent_List(&cnt);
		for (u32 i = 1; i < cnt && i < 2048; i++) {
			if (e[i].cls != EC_NPC || (e[i].flags & EF_KILLED)) continue;
			Npc *n = Npc_OfEntity(i); if (!n || n->dead || !n->moving) { memcpy(last[i], e[i].origin, 12); continue; }
			f32 dx = e[i].origin[0] - last[i][0], dy = e[i].origin[1] - last[i][1];
			if (dx * dx + dy * dy > 40 * 40 && last[i][0] != 0) { jumps++; printf("  jump: %s %.0f units at (%.0f %.0f %.0f)\n", e[i].name ? e[i].name : e[i].classname, sqrtf(dx*dx+dy*dy), e[i].origin[0], e[i].origin[1], e[i].origin[2]); }
			if (e[i].origin[2] > z0[i] + 24) { climbs++; printf("  climb: %s z %.0f -> %.0f at (%.0f %.0f)\n", e[i].name ? e[i].name : e[i].classname, z0[i], e[i].origin[2], e[i].origin[0], e[i].origin[1]); z0[i] = e[i].origin[2]; }
			memcpy(last[i], e[i].origin, 12);
		}
	}
}
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Entity *e = Ent_List(&cnt);
	for (u32 i = 1; i < cnt && i < 2048; i++) { z0[i] = e[i].origin[2]; memcpy(last[i], e[i].origin, 12); }
	tick(2);
	for (u32 i = 1; i < cnt && i < 2048; i++) z0[i] = e[i].origin[2];
	Entity *relay = named("relay_setup_food_queue");
	if (relay) Ent_FireOutput((s32)(relay - Ent_List(&cnt)), "ontrigger", 0);
	tick(8);
	Entity *st = named("trigger_start_food_queue");
	if (st) Ent_FireOutput((s32)(st - Ent_List(&cnt)), "ontrigger", 0);
	tick(60);
	printf("%s: %d jumps, %d climbs\n", argv[1], jumps, climbs);
	return 0;
}
