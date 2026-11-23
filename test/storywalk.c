/* Host test: walks a map's story like a player would. The player goes to the nearest trigger the map has
 * switched on (one meant for him, with outputs), waits there while the scenes play, and goes on; it stops
 * at the level change. Reports how far it got, NPCs that walked without getting anywhere, and (with
 * ENT_TRACE) the map's I/O and the scenes' stalls.  usage: storywalk map.hgm [seconds] */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "scene.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static u8 visited[8192];
static f32 mvT[8192], mvFrom[8192][3];
static int stuckReports;
static f32 T;
static void tick(f32 secs)
{
	UserCmd c = {0};
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); T += TICK_INTERVAL;
		Entity *E = Ent_List(&cnt);
		for (u32 i = 1; i < cnt && i < 8192; i++) {
			if (E[i].cls != EC_NPC || (E[i].flags & (EF_KILLED | EF_DISABLED))) continue;
			Npc *n = Npc_OfEntity((s32)i);
			if (!n || n->dead || !n->moving) { mvT[i] = 0; continue; }
			if (mvT[i] == 0) memcpy(mvFrom[i], E[i].origin, 12);
			mvT[i] += TICK_INTERVAL;
			if (mvT[i] >= 6.0f) {
				f32 d[3] = {E[i].origin[0] - mvFrom[i][0], E[i].origin[1] - mvFrom[i][1], E[i].origin[2] - mvFrom[i][2]};
				f32 l = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
				if (l < 48) { stuckReports++; printf("[%7.2f] STUCK %s (%s) walked 6 s, got %.0f units, at %.0f %.0f %.0f script %d, going to %.0f %.0f %.0f (path %d/%d)\n", T, E[i].name ? E[i].name : "-", E[i].classname, l, E[i].origin[0], E[i].origin[1], E[i].origin[2], n->script, n->goal[0], n->goal[1], n->goal[2], n->pathPos, n->pathLen); }
				mvT[i] = 0;
			}
		}
	}
}
static int has_outputs(s32 i) { return Ent_KV(i, "OnTrigger") || Ent_KV(i, "OnStartTouch") || Ent_KV(i, "ontrigger") || Ent_KV(i, "onstarttouch"); }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	f32 secs = argc > 2 ? (f32)atof(argv[2]) : 600;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	tick(3);
	int steps = 0;
	f32 prev[3]; memcpy(prev, pl.origin, 12);
	while (T < secs) {
		Entity *E = Ent_List(&cnt);
		s32 best = -1; f32 bd = 1e30f;
		for (u32 i = 1; i < cnt && i < 8192; i++) {
			if (visited[i] || (E[i].flags & (EF_KILLED | EF_DISABLED)) || E[i].locked) continue;
			const char *c = E[i].classname;
			int trig = (!strcmp(c, "trigger_once") || !strcmp(c, "trigger_multiple")) && (E[i].spawnflags & 1) && has_outputs((s32)i);
			int chg = !strcmp(c, "trigger_changelevel") && !(E[i].spawnflags & 2);
			int suit = !strcmp(c, "item_suit");
			if (!trig && !chg && !suit) continue;
			if (trig && !E[i].enabled) continue; /* (switched off for now: come back when the map enables it) */
			const char *nm = E[i].name ? E[i].name : "";
			if (strstr(nm, "escape") || strstr(nm, "fail") || strstr(nm, "death") || strstr(nm, "kill")) continue;
			f32 mn[3], mx[3]; Ent_WorldBounds((s32)i, mn, mx);
			f32 c3[3] = {(mn[0]+mx[0])/2, (mn[1]+mx[1])/2, (mn[2]+mx[2])/2};
			f32 d = 0; for (int k = 0; k < 3; k++) d += (c3[k]-pl.origin[k])*(c3[k]-pl.origin[k]);
			if (chg) d += 1e12f; /* (the level change last) */
			if (d < bd) { bd = d; best = (s32)i; }
		}
		if (best < 0) { tick(2); continue; }
		Entity *b = &E[best];
		if (!strcmp(b->classname, "trigger_changelevel")) { printf("[%7.2f] REACHED the level change after %d steps\n", T, steps); printf("storywalk: ok\n"); return 0; }
		visited[best] = 1; steps++;
		f32 mn[3], mx[3]; Ent_WorldBounds(best, mn, mx);
		pl.origin[0] = (mn[0]+mx[0])/2; pl.origin[1] = (mn[1]+mx[1])/2; pl.origin[2] = mn[2] + 1;
		printf("[%7.2f] WALK to %s %s (%.0f %.0f %.0f)\n", T, b->classname, b->name ? b->name : "-", pl.origin[0], pl.origin[1], pl.origin[2]);
		/* (items are picked up by the weapons code, not built into this test: do what it does) */
		if (!strncmp(b->classname, "item_", 5)) {
			Ent_FireOutput(best, "onplayertouch", 0); Ent_FireOutput(best, "onplayerpickup", 0);
			b->flags |= EF_KILLED;
		}
		/* touch it, then step back the way he came (out of the doorway the NPCs walk through) */
		f32 back[3] = {prev[0] - pl.origin[0], prev[1] - pl.origin[1], 0};
		tick(0.5f);
		f32 bl = sqrtf(back[0]*back[0] + back[1]*back[1]);
		if (bl > 1) {
			f32 to[3] = {pl.origin[0] + back[0] / bl * 72, pl.origin[1] + back[1] / bl * 72, pl.origin[2]};
			Trace tr; f32 pm[3], px[3]; Player_Bounds(&pl, pm, px);
			CM_BoxTrace(&tr, pl.origin, to, pm, px, MASK_PLAYERSOLID);
			if (!tr.startsolid) memcpy(pl.origin, tr.endpos, 12);
		}
		memcpy(prev, pl.origin, 12);
		/* wait while the story plays: scenes running or someone on a script walking (up to a minute) */
		f32 w0 = T;
		for (;;) {
			tick(1);
			int busy = 0; u32 who = 0;
			Entity *E2 = Ent_List(&cnt);
			for (u32 i = 1; i < cnt && !busy; i++) {
				if (!strcmp(E2[i].classname, "logic_choreographed_scene") && Scene_IsPlaying((s32)i)) busy = 1, who = i;
				Npc *n = E2[i].cls == EC_NPC ? Npc_OfEntity((s32)i) : NULL;
				if (n && !n->dead && n->moving && n->script >= 0) busy = 2, who = i;
			}
			if (!busy && T - w0 > 3) break;
			if (T - w0 > 20) {
				printf("[%7.2f] WAITED 20 s and the story is still busy (%s %s", T, busy == 1 ? "scene" : "scripted walk", E2[who].name ? E2[who].name : E2[who].classname);
				if (busy == 2) { Npc *n = Npc_OfEntity((s32)who); printf(" at %.0f %.0f %.0f script %s, going to %.0f %.0f %.0f", E2[who].origin[0], E2[who].origin[1], E2[who].origin[2], n->script > 0 && E2[n->script].name ? E2[n->script].name : "-", n->goal[0], n->goal[1], n->goal[2]); }
				printf(")\n");
				break;
			}
		}
	}
	printf("[%7.2f] did not reach the level change (%d steps, %d stuck reports)\nFAILED\n", T, steps, stuckReports);
	return 1;
}
