/* Host test: hopper mines, mounted guns, headcrab canisters, thumpers, snipers, launchers and APC drivers */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "mines.h"
#include "tanks.h"
#include "objects.h"
#include "relations.h"
#include "physics.h"
static int hurt; static f32 hurtAmt;
static void hurtcb(f32 d) { hurt++; hurtAmt += d; }
static void hurtfrom(f32 d, u32 t, const f32 *f) { (void)t; (void)f; hurt++; hurtAmt += d; }
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt; static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void tick(f32 secs, const f32 *hold) { UserCmd c = {0}; for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) { if (hold) memcpy(pl.origin, hold, 12); Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); } }
static int ground_near(const f32 at[3], f32 r, f32 out[3])
{
	for (int a = 0; a < 24; a++) {
		f32 ang = a * 0.2618f;
		f32 tp[3] = {at[0] + cosf(ang) * r, at[1] + sinf(ang) * r, at[2] + 64};
		f32 dn[3] = {tp[0], tp[1], tp[2] - 400}, mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72};
		Trace tr; CM_BoxTrace(&tr, tp, dn, mn, mx, MASK_PLAYERSOLID);
		if (tr.startsolid || tr.fraction >= 1) continue;
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
	Ent_GlobalState("gordon_precriminal");
	Entity *ents = Ent_List(&cnt);
	int did[16] = {0};
	if (getenv("OBJ_SKIP")) did[atoi(getenv("OBJ_SKIP"))] = 1;
	for (u32 i = 1; i < cnt; i++) {
		const char *cn = ents[i].classname;
		s32 e = (s32)i;
		if (!strcmp(cn, "combine_mine") && !did[0] && Mines_Probe(e) == 1) {
			f32 sp[3]; if (!ground_near(ents[i].origin, 60, sp)) continue;
			int h0 = hurt; f32 a0 = hurtAmt;
			tick(3, sp);
			printf("  mine %u: state %d, player hurt %d times (%.0f)\n", i, Mines_Probe(e), hurt - h0, hurtAmt - a0);
			CHECK(Mines_Probe(e) == 4 && hurt > h0, "mine %u didn't go off on the player", i);
			did[0] = 1;
		}
		if (!strcmp(cn, "func_tank") && !did[1] && (ents[i].spawnflags & 0x20)) {
			s32 ctl; f32 ay; int kind = Tanks_Probe(e, &ctl, &ay);
			const char *vn = Ent_KV(e, "control_volume"); s32 vol = vn ? Ent_FindActor(vn) : -1;
			if (vol <= 0 || kind != 2) continue;
			f32 mn[3], mx[3]; Ent_Bounds(vol, mn, mx);
			f32 sp[3] = {(mn[0]+mx[0])/2, (mn[1]+mx[1])/2, mn[2] + 1};
			memcpy(pl.origin, sp, 12);
			/* look at the gun, use it, then look 30 degrees to the side and fire */
			f32 eye[3] = {sp[0], sp[1], sp[2] + 64};
			f32 to[3] = {ents[i].origin[0]-eye[0], ents[i].origin[1]-eye[1], ents[i].origin[2]-eye[2]};
			f32 l = sqrtf(to[0]*to[0]+to[1]*to[1]+to[2]*to[2]);
			for (int k = 0; k < 3; k++) to[k] /= l;
			int got = Tanks_PlayerUse(eye, to);
			f32 yaw0 = atan2f(to[1], to[0]);
			f32 side[3] = {cosf(yaw0 + 0.5f), sinf(yaw0 + 0.5f), 0};
			Tanks_SetView(eye, side);
			tick(2, sp);
			Tanks_Probe(e, &ctl, &ay);
			/* the barrel should point at what his crosshair is on */
			f32 end[3] = {eye[0] + side[0] * 8192, eye[1] + side[1] * 8192, eye[2]}; Trace vt; CM_TraceWorld(&vt, eye, end, MASK_SHOT);
			s32 mdl = Ent_FindActor(Ent_KV(e, "parentname")); f32 bp[3], bR[3][3]; f32 err = -1;
			if (mdl > 0 && Npc_Attachment(Npc_OfEntity(mdl), "muzzle", bp, bR)) {
				f32 d[3] = {vt.endpos[0]-bp[0], vt.endpos[1]-bp[1], vt.endpos[2]-bp[2]}; f32 dl = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
				f32 cs = dl > 32 ? (d[0]*bR[0][0]+d[1]*bR[1][0]+d[2]*bR[2][0]) / dl : 1; cs = cs > 1 ? 1 : cs < -1 ? -1 : cs;
				err = acosf(cs) * 180 / (f32)M_PI; }
			printf("  func_tank %s: manned %d, controller %d, aim yaw %.0f, barrel off the crosshair by %.0f deg\n", ents[i].name ? ents[i].name : "", got, ctl, ay, err);
			CHECK(got && ctl == 0 && fabsf(ay) > 10, "func_tank %u: the player can't man it / it doesn't follow his view", i);
			CHECK(err >= 0 && err < 8, "func_tank %u: the barrel doesn't point where he looks", i);
			Tanks_PlayerLeave();
			did[1] = 1;
		}
		if (!strcmp(cn, "func_tank") && !did[2] && (ents[i].spawnflags & 0x400)) {
			/* an NPC brought near its man point goes to it and takes the gun */
			const char *mpn = Ent_KV(e, "npc_man_point"); s32 mp = mpn ? Ent_FindActor(mpn) : -1;
			for (u32 j = 1; j < cnt && mp > 0; j++) if (!(ents[j].flags & (EF_TEMPLATE | EF_KILLED)) && Npc_OfEntity((s32)j) && Npc_OfEntity((s32)j)->script < 0 && (!strcmp(ents[j].classname, "npc_citizen") || !strcmp(ents[j].classname, "npc_combine_s") || !strcmp(ents[j].classname, "npc_metropolice")) && Npc_OfEntity((s32)j) && !Npc_OfEntity((s32)j)->dead) {
				f32 sp[3]; if (ground_near(ents[mp].origin, 150, sp)) { ents[j].flags &= ~EF_DISABLED; Npc_Teleport(Npc_OfEntity((s32)j), sp, 0); printf("    (brought %s %u next to its man point; tank sf %x findNpcs)\n", ents[j].classname, j, ents[i].spawnflags); } else printf("    (no floor by the man point)\n"); break; }
			tick(8, NULL);
			s32 ctl; f32 ay; Tanks_Probe(e, &ctl, &ay);
			{ const char *mpn2 = Ent_KV(e, "npc_man_point"); s32 mp2 = mpn2 ? Ent_FindActor(mpn2) : -1;
			  if (mp2 > 0) for (u32 j = 1; j < cnt; j++) if (!strcmp(ents[j].classname, "npc_metropolice") || !strcmp(ents[j].classname, "npc_citizen") || !strcmp(ents[j].classname, "npc_combine_s")) { f32 dx = ents[j].origin[0]-ents[mp2].origin[0], dy = ents[j].origin[1]-ents[mp2].origin[1], dz = ents[j].origin[2]-ents[mp2].origin[2]; if (dx*dx+dy*dy < 300*300) printf("    %s %u now %.0f from the man point (dz %.0f) moving %d script %d enemy %d\n", ents[j].classname, j, sqrtf(dx*dx+dy*dy), dz, Npc_OfEntity((s32)j) ? Npc_OfEntity((s32)j)->moving : -1, Npc_OfEntity((s32)j) ? Npc_OfEntity((s32)j)->script : -9, Npc_OfEntity((s32)j) ? Npc_OfEntity((s32)j)->enemy : -9); } }
			printf("  func_tank %s: NPC controller %d (%s)\n", ents[i].name ? ents[i].name : "", ctl, ctl > 0 ? ents[ctl].classname : "-");
			{ int manned = ctl > 0; for (u32 j = 1; j < cnt && !manned; j++) { s32 c2; f32 a2; if (!strncmp(ents[j].classname, "func_tank", 9) && Tanks_Probe((s32)j, &c2, &a2) && c2 > 0) { manned = 1; printf("    (%s mans %s)\n", ents[c2].classname, ents[j].name ? ents[j].name : "a gun"); } }
			  if (!manned) printf("    (no NPC reached a gun here in 8 s)\n"); }
			did[2] = 1;
		}
		int st; f32 pos[3]; int k = Objects_Probe(e, &st, pos);
		if (k == 1 && !did[3]) {
			f32 p0[3]; memcpy(p0, pos, 12);
			u32 c0; Ent_List(&c0); int crabs0 = 0;
			for (u32 j = 1; j < c0; j++) if (!strncmp(Ent_List(NULL)[j].classname, "npc_headcrab", 12) && !(Ent_List(NULL)[j].flags & (EF_KILLED|EF_DISABLED)) && Npc_OfEntity((s32)j)) crabs0++;
			Ent_Input(e, "FireCanister", "", 0);
			tick(0.2f, NULL);
			Objects_Probe(e, &st, pos);
			f32 mid[3]; memcpy(mid, pos, 12);
			tick(8, NULL);
			u32 c2; Ent_List(&c2);
			int crabs = 0; for (u32 j = 1; j < c2; j++) if (!strncmp(Ent_List(NULL)[j].classname, "npc_headcrab", 12) && !(Ent_List(NULL)[j].flags & (EF_KILLED|EF_DISABLED)) && Npc_OfEntity((s32)j)) crabs++;
			Ent_Input(e, "OpenCanister", "", 0); tick(2, NULL); Ent_Input(e, "SpawnHeadcrabs", "", 0); tick(5, NULL);
			Ent_List(&c2); int crabs2 = 0; for (u32 j = 1; j < c2; j++) if (!strncmp(Ent_List(NULL)[j].classname, "npc_headcrab", 12) && !(Ent_List(NULL)[j].flags & (EF_KILLED|EF_DISABLED)) && Npc_OfEntity((s32)j)) crabs2++;
			Objects_Probe(e, &st, pos);
			printf("  canister %s: flew %.0f units in 0.2 s, landed state %d, headcrabs %d -> %d -> %d, _crab %s\n", ents[i].name ? ents[i].name : "",
			       sqrtf((mid[0]-p0[0])*(mid[0]-p0[0])+(mid[1]-p0[1])*(mid[1]-p0[1])+(mid[2]-p0[2])*(mid[2]-p0[2])), st & 255, crabs0, crabs, crabs2, Ent_KV(e, "_crab") ? Ent_KV(e, "_crab") : "none");
			CHECK((st & 255) >= 2, "canister %u never landed", i);
			if (Ent_KV(e, "_crab") && atoi(Ent_KV(e, "headcrabcount") ? Ent_KV(e, "headcrabcount") : "6") > 0) CHECK(crabs2 > crabs0, "canister %u let out no headcrabs", i);
			did[3] = 1;
		}
		if (k == 2 && !did[4]) {
			/* an antlion next to it runs out of its radius */
			s32 ant = -1; for (u32 j = 1; j < cnt; j++) if (!strcmp(ents[j].classname, "npc_antlion") && !(ents[j].flags & (EF_TEMPLATE | EF_KILLED)) && Npc_OfEntity((s32)j) && Npc_OfEntity((s32)j)->script < 0) { ant = (s32)j; break; }
			Npc *an = ant > 0 ? Npc_OfEntity(ant) : NULL;
			Ent_Input(e, "Enable", "", 0);
			ents[i].flags &= ~EF_DISABLED;
			if (an) {
				f32 sp[3]; if (ground_near(pos, 200, sp)) { ents[ant].flags &= ~EF_DISABLED; Npc_Teleport(an, sp, 0); }
				f32 d0 = sqrtf((ents[ant].origin[0]-pos[0])*(ents[ant].origin[0]-pos[0])+(ents[ant].origin[1]-pos[1])*(ents[ant].origin[1]-pos[1]));
				tick(10, NULL);
				f32 d1 = sqrtf((ents[ant].origin[0]-pos[0])*(ents[ant].origin[0]-pos[0])+(ents[ant].origin[1]-pos[1])*(ents[ant].origin[1]-pos[1]));
				printf("  thumper %s: an antlion %.0f -> %.0f units away\n", ents[i].name ? ents[i].name : "", d0, d1);
				CHECK(d1 > d0 + 100, "thumper %u doesn't drive antlions off", i);
			} else printf("  thumper %s: (no antlion on the map to test with)\n", ents[i].name ? ents[i].name : "");
			did[4] = 1;
		}
		if (k == 3 && !did[5]) {
			Ent_Input(e, "EnableSniper", "", 0);
			Rel_SetRelationship(e, "!player D_HT 99");
			f32 sp[3]; if (!ground_near(pos, 400, sp)) continue;
			f32 c[3] = {sp[0], sp[1], sp[2] + 40}; Trace tr; CM_TraceWorld(&tr, pos, c, MASK_SHOT);
			if (tr.fraction < 0.98f) continue;
			int h0 = hurt;
			tick(4, sp);
			printf("  sniper %s: hits on the player %d\n", ents[i].name ? ents[i].name : "", hurt - h0);
			CHECK(hurt > h0, "sniper %u never shot the player", i);
			did[5] = 1;
		}
		if (k == 4 && !did[6]) {
			Ent_Input(e, "TurnOn", "", 0);
			Ent_Input(e, "SetEnemyEntity", "!player", 0);
			f32 sp[3]; if (!ground_near(pos, 600, sp)) continue;
			int h0 = hurt, inair = 0;
			for (int q = 0; q < 120; q++) { tick(0.1f, sp); for (int m = 0; m < Objects_NumMissiles(); m++) { f32 mp[3]; inair += Objects_Missile(m, mp); } }
			printf("  launcher %s: missile-ticks in the air %d, target %d, player hurt %d times\n", ents[i].name ? ents[i].name : "", inair, Ent_FindActor("!player"), hurt - h0);
			CHECK(inair > 0, "launcher %u never fired", i);
			if (0) printf("  launcher %s: player hurt %d times\n", ents[i].name ? ents[i].name : "", hurt - h0);
			did[6] = 1;
		}
		if ((k == 5 || k == 6) && !did[7]) {
			const char *vn = Ent_KV(e, "vehicle"); s32 v = vn ? Ent_FindActor(vn) : -1;
			s32 path = -1;
			for (u32 j = 1; j < cnt && path < 0; j++) if (!strcmp(ents[j].classname, "path_track")) {
				f32 d = sqrtf((ents[j].origin[0]-ents[v].origin[0])*(ents[j].origin[0]-ents[v].origin[0])+(ents[j].origin[1]-ents[v].origin[1])*(ents[j].origin[1]-ents[v].origin[1]));
				if (d > 200 && d < 4000 && ents[j].name) path = (s32)j; }
			if (v > 0 && k == 6 && !did[8]) {
				/* its guns: StartFiring with the player in the open near it */
				Ent_Input(e, "StartFiring", "", 0);
				Rel_SetRelationship(e, "!player D_HT 99");
				f32 sp[3]; f32 gun[3] = {ents[v].origin[0], ents[v].origin[1], ents[v].origin[2] + 96};
				int okp = 0;
				for (int r = 300; r <= 900 && !okp; r += 200) if (ground_near(ents[v].origin, (f32)r, sp)) { f32 c[3] = {sp[0], sp[1], sp[2] + 40}; Trace tr; CM_SetIgnoreEnt(v); CM_TraceWorld(&tr, gun, c, MASK_SHOT); CM_SetIgnoreEnt(-1); okp = tr.fraction > 0.98f; }
				int guns = !(ents[i].spawnflags & 0x20000) || !(ents[i].spawnflags & 0x10000);
				if (okp && guns && !(ents[v].flags & EF_DISABLED)) { int h0 = hurt; tick(6, sp); printf("  APC %s: player hit %d times\n", ents[v].name, hurt - h0); CHECK(hurt > h0, "APC %d never hit the player", v); did[8] = 1; }
			}
			{ const char *own = Ent_KV(e, "target"); s32 op = own ? Ent_FindActor(own) : -1; if (op > 0) path = op; }
			if (v > 0 && (ents[v].flags & EF_DISABLED)) { printf("  (%s %u: its vehicle isn't in the world yet)\n", cn, i); continue; }
			if (v <= 0 || path < 0) { printf("  (%s %u: vehicle %d, no path_track nearby)\n", cn, i, v); continue; }
			f32 v0[3]; memcpy(v0, ents[v].origin, 12);
			Ent_Input(e, "GotoPathCorner", ents[path].name, 0);
			tick(3, NULL);
			f32 moved = sqrtf((ents[v].origin[0]-v0[0])*(ents[v].origin[0]-v0[0])+(ents[v].origin[1]-v0[1])*(ents[v].origin[1]-v0[1]));
			printf("  %s %s: drove its %s %.0f units toward %s\n", cn, ents[i].name ? ents[i].name : "", ents[v].classname, moved, ents[path].name);
			CHECK(moved > 100, "%s %u didn't drive", cn, i);
			did[7] = 1;
		}
	}
	printf("%s\n%s\n", argv[1], fails ? "FAILED" : "objtest: ok");
	return fails != 0;
}
