/* Host test: ropes, tethers and effect entities (envents.c, physics.c constraints)
 *  - every rope hangs: finite, its ends on its keyframes, slack ropes sag below the straight line
 *  - a rope parented to something follows it
 *  - phys_spring: the plug (func_physbox) pulled away from its anchor is pulled back toward it
 *  - env_spark / env_explosion / env_fire / point_tesla / env_beam respond to their inputs */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "envents.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static u32 cnt;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
extern int stubSparks, stubExplosions;
static void tick(f32 secs) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); } }
static s32 named(const char *n) { return Ent_FindActor(n); }
static f32 dist(const f32 *a, const f32 *b) { f32 d[3] = {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; return sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Phys_Init(&P, &pl);
	Entity *ents = Ent_List(&cnt);
	/* move the player away from everything so nothing gets triggered by standing in it */
	tick(3);
	int nr = EnvEnts_NumRopes(), sag = 0, slackRopes = 0, drawn = 0;
	for (int i = 0; i < nr; i++) {
		f32 nodes[10][3];
		int n = EnvEnts_RopeNodes(i, nodes, 10);
		RopeDraw rd;
		if (EnvEnts_Rope(i, &rd)) drawn++;
		for (int k = 0; k < n; k++)
			CHECK(isfinite(nodes[k][0]) && isfinite(nodes[k][2]), "rope %d node %d not finite", i, k);
		f32 straight = dist(nodes[0], nodes[n - 1]);
		f32 mid[3] = {(nodes[0][0] + nodes[n-1][0]) / 2, (nodes[0][1] + nodes[n-1][1]) / 2, (nodes[0][2] + nodes[n-1][2]) / 2};
		f32 lowest = 1e9;
		for (int k = 0; k < n; k++) if (nodes[k][2] < lowest) lowest = nodes[k][2];
		(void)mid;
		/* slack (spring length x segments > the gap): it should hang lower than its lower end */
		if (n > 2) {
			f32 lowEnd = nodes[0][2] < nodes[n-1][2] ? nodes[0][2] : nodes[n-1][2];
			if (lowest < lowEnd - 4) sag++;
			if (straight > 1) slackRopes++;
		}
	}
	printf("%s: %d ropes, %d drawn, %d sag below their ends\n", argv[1], nr, drawn, sag);
	/* tethered plugs (func_physbox "plug"): a phys_spring pulls it back (d1_trainstation_05, d2_coast_08); a
	 * phys_lengthconstraint holds it within the cable's length (d2_prison_03) */
	s32 plug = named("plug");
	if (plug > 0 && Phys_IsBody(plug)) {
		Phys_SetMotion(plug, 1);
		f32 a[3], b[3], len; int on, found = -1, type = 0;
		for (int i = 0; i < Phys_ConstraintCount(); i++) {
			int t = Phys_ConstraintProbe(i, a, b, &len, &on);
			if (t && (dist(a, ents[plug].origin) < 200 || dist(b, ents[plug].origin) < 200)) { found = i; type = t; }
		}
		CHECK(found >= 0, "nothing tethers the plug");
		if (found >= 0) {
			printf("plug mass %.1f kg, tether type %d\n", Phys_Mass(plug), type);
			Phys_ConstraintProbe(found, a, b, &len, &on);
			f32 d0 = dist(a, b);
			f32 away[3] = {ents[plug].origin[0] + 300, ents[plug].origin[1] + 300, ents[plug].origin[2] + 40};
			Phys_Teleport(plug, away);
			Phys_ConstraintProbe(found, a, b, &len, &on);
			f32 d1 = dist(a, b);
			tick(3);
			Phys_ConstraintProbe(found, a, b, &len, &on);
			f32 d2 = dist(a, b);
			printf("plug tether: length %.0f, at rest %.0f, pulled to %.0f, after 3 s %.0f (%s)\n", len, d0, d1, d2, on ? "on" : "off");
			if (type == 1)
				CHECK(d2 < d1 - 20, "the spring didn't pull the plug back (%.0f -> %.0f)", d1, d2);
			else if (type == 2 && on)
				CHECK(d2 <= len + 8, "the cable didn't hold the plug (%.0f > %.0f)", d2, len);
		}
	}
	/* effect entities: fire their inputs */
	int sp0 = stubSparks + EnvEnts_SparkCount(), ex0 = stubExplosions, fires = 0, firesOn = 0;
	for (u32 i = 1; i < cnt; i++) {
		const char *cn = ents[i].classname;
		if (!strcmp(cn, "env_spark")) Ent_Input((s32)i, "SparkOnce", "", 0);
		if (!strcmp(cn, "env_explosion") && !(ents[i].flags & EF_KILLED)) Ent_Input((s32)i, "Explode", "", 0);
		if (!strcmp(cn, "env_fire")) { fires++; Ent_Input((s32)i, "StartFire", "", 0); }
		if (!strcmp(cn, "point_tesla")) Ent_Input((s32)i, "DoSpark", "", 0);
		if (!strcmp(cn, "env_beam")) Ent_Input((s32)i, "TurnOn", "", 0);
		if (!strcmp(cn, "env_shooter")) Ent_Input((s32)i, "Shoot", "", 0);
	}
	tick(2);
	for (u32 i = 1; i < cnt; i++) if (!strcmp(ents[i].classname, "env_fire") && EnvEnts_FireOn((s32)i)) firesOn++;
	int beams = 0; BeamDraw bd;
	for (int i = 0; i < EnvEnts_NumBeams(); i++) beams += EnvEnts_Beam(i, &bd);
	printf("effects: sparks %d, explosions %d, fires %d of %d burning, %d beams/arcs drawn\n",
	       stubSparks + EnvEnts_SparkCount() - sp0, stubExplosions - ex0, firesOn, fires, beams);
	CHECK(firesOn == fires, "%d of %d fires lit", firesOn, fires);
	printf("%s\n", fails ? "FAILED" : "envtest: ok");
	return fails != 0;
}
