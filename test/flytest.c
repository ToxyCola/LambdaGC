/* Host test: manhacks hunt the player and slice him; scanners come over to look at him and take pictures */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "flyers.h"
#include "relations.h"
#include "physics.h"
static int hurt;
static void hurtcb(f32 d) { (void)d; hurt++; }
static void hurtfrom(f32 d, u32 t, const f32 *f) { (void)d; (void)t; (void)f; hurt++; }
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static u32 cnt;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static int flashes;
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL);
	for (int i = 0; i < Flyers_Count(); i++) { f32 p[3], s; u32 col; if (Flyers_Glow(i, p, &col, &s) && col == 0xFFFFFFFFu) flashes++; } } }
static int spot_near(const f32 from[3], f32 dist, f32 out[3])
{
	for (int a = 0; a < 24; a++) {
		f32 ang = a * 0.2618f;
		f32 tp[3] = {from[0] + cosf(ang) * dist, from[1] + sinf(ang) * dist, from[2]};
		f32 dn[3] = {tp[0], tp[1], tp[2] - 600}, mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72};
		Trace tr; CM_BoxTrace(&tr, tp, dn, mn, mx, MASK_PLAYERSOLID);
		if (tr.startsolid || tr.fraction >= 1) continue;
		f32 c[3] = {tr.endpos[0], tr.endpos[1], tr.endpos[2] + 40};
		Trace t2; CM_TraceWorld(&t2, from, c, MASK_SHOT);
		if (t2.fraction < 0.98f) continue;
		memcpy(out, tr.endpos, 12); return 1;
	}
	return 0;
}
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl, NULL, hurtcb, hurtfrom, NULL};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Entity *ents = Ent_List(&cnt);
	int doneM = 0, doneS = 0;
	for (u32 i = 1; i < cnt; i++) {
		int man = !strcmp(ents[i].classname, "npc_manhack"), scan = !strcmp(ents[i].classname, "npc_cscanner");
		if ((!man || doneM) && (!scan || doneS)) continue;
		Npc *n = Npc_OfEntity((s32)i); if (!n || n->dead) continue;
		ents[i].flags &= ~EF_DISABLED;
		if (scan) Rel_SetRelationship((s32)i, "!player D_HT 99"); /* (the map may have it neutral for now) */
		f32 sp[3];
		if (!spot_near(ents[i].origin, man ? 300 : 450, sp)) continue;
		memcpy(pl.origin, sp, 12);
		f32 d0 = sqrtf((ents[i].origin[0]-sp[0])*(ents[i].origin[0]-sp[0]) + (ents[i].origin[1]-sp[1])*(ents[i].origin[1]-sp[1]));
		int h0 = hurt; flashes = 0;
		tick(man ? 5 : 15);
		memcpy(pl.origin, sp, 12);
		f32 d1 = sqrtf((ents[i].origin[0]-sp[0])*(ents[i].origin[0]-sp[0]) + (ents[i].origin[1]-sp[1])*(ents[i].origin[1]-sp[1]));
		printf("  %s %s: %.0f -> %.0f units from the player, hits %d, photo flashes %d frames\n", ents[i].classname, ents[i].name ? ents[i].name : "", d0, d1, hurt - h0, flashes);
		if (man) { CHECK(hurt > h0, "manhack %u never hit the player", i); doneM = 1; }
		if (scan) printf("    scanner: moving %d script %d locked %d, disposition %d\n", n->moving, n->script, n->locked, Rel_Disposition((s32)i, 0));
		if (scan && Rel_Disposition((s32)i, 0) == 1) { CHECK(d1 < 420 && flashes > 0, "scanner %u didn't come to photograph the player", i); doneS = 1; }
	}
	printf("%s: %d flyers\n%s\n", argv[1], Flyers_Count(), fails ? "FAILED" : "flytest: ok");
	return fails != 0;
}
