/* Host test: cameras and turrets (sentries.c) look at the player: the "eyes" attachment ends up pointing at
 * him (within 10 degrees), the camera fires OnFoundPlayer, the turret deploys and hits him */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "sentries.h"
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
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); } }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl, NULL, hurtcb, hurtfrom, NULL};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Entity *ents = Ent_List(&cnt);
	int tested = 0;
	for (u32 i = 1; i < cnt && tested < 4; i++) {
		const char *cn = ents[i].classname;
		int cam = !strcmp(cn, "npc_combine_camera");
		if (!cam && strcmp(cn, "npc_turret_ceiling")) continue;
		Npc *n = Npc_OfEntity((s32)i);
		if (!n) { printf("  %s %u: no NPC (model missing?)\n", cn, i); continue; }
		ents[i].flags &= ~EF_DISABLED; /* (some come out of a point_template later) */
		Ent_Input((s32)i, "Enable", "", 0);
		/* put the player somewhere it can see: ahead of it, below, off to one side */
		f32 fw[3] = {ents[i].R[0][0], ents[i].R[1][0], 0};
		f32 lf[3] = {ents[i].R[0][1], ents[i].R[1][1], 0};
		f32 best[3]; int found = 0;
		for (int a = 0; a < 16 && !found; a++) {
			f32 side = (a % 2 ? 1 : -1) * (f32)(a / 2) * 25;
			f32 tp[3] = {ents[i].origin[0] + fw[0] * 220 + lf[0] * side, ents[i].origin[1] + fw[1] * 220 + lf[1] * side, ents[i].origin[2] + 8};
			f32 dn[3] = {tp[0], tp[1], tp[2] - 400}, mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72};
			Trace tr; CM_BoxTrace(&tr, tp, dn, mn, mx, MASK_PLAYERSOLID);
			if (tr.startsolid || tr.fraction >= 1) continue;
			f32 eye[3]; f32 R[3][3];
			if (!Npc_Attachment(n, "eyes", eye, R)) memcpy(eye, ents[i].origin, 12);
			f32 c[3] = {tr.endpos[0], tr.endpos[1], tr.endpos[2] + 40};
			Trace t2; CM_TraceWorld(&t2, eye, c, MASK_SHOT);
			if (t2.fraction < 0.98f) continue;
			memcpy(best, tr.endpos, 12); found = 1;
		}
		if (!found) { printf("  %s %u: no spot in view\n", cn, i); continue; }
		printf("  (%s flags %x dead %d prop %d locked %d)\n", cn, ents[i].flags, n->dead, n->prop, n->locked);
		Ent_SetNoTarget(0);
		Ent_GlobalState("gordon_precriminal");
		memcpy(pl.origin, best, 12);
		int h0 = hurt;
		tick(4);
		f32 eye[3], R[3][3];
		int ok = Npc_Attachment(n, "eyes", eye, R);
		f32 to[3] = {pl.origin[0] - eye[0], pl.origin[1] - eye[1], pl.origin[2] + 40 - eye[2]};
		f32 l = sqrtf(to[0]*to[0]+to[1]*to[1]+to[2]*to[2]);
		f32 dot = (to[0]*R[0][0] + to[1]*R[1][0] + to[2]*R[2][0]) / l;
		f32 ay, ap; s32 tg; int st;
		Sentries_Probe((s32)i, &ay, &ap, &tg, &st);
		f32 ang = acosf(dot > 1 ? 1 : dot < -1 ? -1 : dot) * 180 / (f32)M_PI;
		printf("  %s %s: disposition %d, target %d, state %d, aim yaw %.0f pitch %.0f, eyes off the player by %.0f deg, hits %d\n",
		       cn, ents[i].name ? ents[i].name : "", Rel_Disposition((s32)i, 0), tg, st, ay, ap, ang, hurt - h0);
		if (ok && tg == 0) CHECK(ang < 12, "%s %u doesn't look at the player (%.0f degrees off)", cn, i, ang);
		if (!cam && tg == 0) CHECK(hurt > h0, "turret %u didn't hit the player", i);
		tested++;
	}
	printf("%s: %d sentries\n%s\n", argv[1], Sentries_Count(), fails ? "FAILED" : "sentrytest: ok");
	return fails != 0;
}
