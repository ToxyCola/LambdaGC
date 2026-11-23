/* Host test: Trainstation 2's can cop. Walking up to him starts his scene: he speaks, his scene fires the
 * knock-off sequence, its animation event pushes the can off the bin (env_physimpact), and it falls */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
extern int stubSoundsPlayed; extern f32 stubSoundSeconds;
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
static s32 named(const char *n) { Entity *e = Ent_List(&cnt); for (u32 i = 1; i < cnt; i++) if (e[i].name && !strcasecmp(e[i].name, n)) return (s32)i; return -1; }
static void go(f32 x, f32 y, f32 z) { pl.origin[0] = x; pl.origin[1] = y; pl.origin[2] = z; }
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	tick(1);
	s32 can = named("cupcop_can"), cop = named("cupcop");
	Entity *E = Ent_List(&cnt);
	if (can < 0 || cop < 0) { printf("no can/cop\n"); return 1; }
	f32 can0[3]; memcpy(can0, E[can].origin, 12);
	printf("can at %.0f %.0f %.0f, cop at %.0f %.0f %.0f\n", can0[0], can0[1], can0[2], E[cop].origin[0], E[cop].origin[1], E[cop].origin[2]);
	{ s32 esc = named("player_escape_trigger"); f32 mn[3], mx[3]; Ent_WorldBounds(esc, mn, mx);
	  printf("escape trigger %.0f %.0f %.0f .. %.0f %.0f %.0f; can is a body %d; player at %.0f %.0f %.0f\n", mn[0],mn[1],mn[2],mx[0],mx[1],mx[2], Phys_IsBody(can), pl.origin[0], pl.origin[1], pl.origin[2]); }
	go(-4268, -808, 64); tick(0.5f); go(-4100, -816, 64); tick(0.3f);
	int s0 = stubSoundsPlayed;
	go(-3912, -816, 64); tick(0.3f);
	go(-3850, -900, 64);
	f32 moved = 0; int spoke = 0;
	for (int i = 0; i < 30; i++) {
		tick(0.5f);
		f32 d[3] = {E[can].origin[0] - can0[0], E[can].origin[1] - can0[1], E[can].origin[2] - can0[2]};
		moved = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
		Npc *n = Npc_OfEntity(cop);
		if (getenv("V")) printf("t %.1f: can moved %.0f (z %.0f), cop at %.0f %.0f seq %d script %d, sounds %d\n", i*0.5f, moved, E[can].origin[2], E[cop].origin[0], E[cop].origin[1], n ? n->seq : -1, n ? n->script : -9, stubSoundsPlayed - s0);
	}
	spoke = stubSoundsPlayed - s0;
	printf("can moved %.0f units, %d sounds after the trigger\n", moved, spoke);
	/* he picks it up and puts it in the bin: the bin's trigger (physics objects, the can by name) fires */
	s32 bin = named("trashcan_trigger");
	int binned = 0;
	if (bin > 0) {
		f32 mn[3], mx[3]; Ent_WorldBounds(bin, mn, mx);
		f32 c[3] = {E[bin].origin[0], E[bin].origin[1], E[bin].origin[2]}; /* (one of its two bins) */
		Phys_Teleport(can, c);
		f32 cm[3], cx[3]; Ent_WorldBounds(can, cm, cx);
		printf("bin trigger %.0f %.0f %.0f .. %.0f %.0f %.0f, can %.0f %.0f %.0f .. %.0f %.0f %.0f\n", mn[0],mn[1],mn[2],mx[0],mx[1],mx[2], cm[0],cm[1],cm[2],cx[0],cx[1],cx[2]);
		tick(2);
		binned = (E[bin].flags & EF_KILLED) != 0;
	}
	printf("can in the bin: trigger fired %d\n", binned);
	int ok = moved > 10 && E[can].origin[2] < can0[2] - 10 && binned;
	printf("%s\n", ok ? "cuptest: ok" : "FAILED");
	return !ok;
}
