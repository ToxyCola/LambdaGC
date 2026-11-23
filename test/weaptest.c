/* Host test: weapons, items and damage on a real map, with the real weapons.hgm.
 * usage: weaptest <map.hgm> <weapons.hgm>
 * Prints counts only. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "../engine/source/weapons.c"

static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;

static void aim_at(const f32 target[3], u32 held, u32 down)
{
	f32 e[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64};
	f32 d[3] = {target[0] - e[0], target[1] - e[1], target[2] - e[2]};
	f32 h = sqrtf(d[0] * d[0] + d[1] * d[1]);
	f32 pitch = -atan2f(d[2], h) * 180 / (f32)M_PI, yaw = atan2f(d[1], d[0]) * 180 / (f32)M_PI;
	Weap_Input(held, down, e, pitch, yaw, 0);
}

static void tick(int n)
{
	UserCmd idle = {0};
	for (int i = 0; i < n; i++) {
		Player_Tick(&pl, &idle);
		Ent_Tick(TICK_INTERVAL);
		Weap_Tick(TICK_INTERVAL);
	}
}

static void stand_near(const f32 at[3], f32 dist)
{
	/* the first spot around the target where the player fits and can see it */
	for (int k = 0; k < 16; k++) {
		f32 a = k * (f32)M_PI / 8;
		f32 p[3] = {at[0] + cosf(a) * dist, at[1] + sinf(a) * dist, at[2]};
		Player t;
		if (!Player_Spawn(&t, p, 0, 0))
			continue;
		f32 e[3] = {t.origin[0], t.origin[1], t.origin[2] + 64};
		Trace tr;
		f32 z[3] = {0, 0, 0};
		CM_BoxTrace(&tr, e, at, z, z, CONTENTS_SOLID | CONTENTS_WINDOW);
		if (tr.fraction > 0.9f || tr.ent >= 0) {
			pl = t;
			return;
		}
	}
	Player_Spawn(&pl, at, 0, 0);
}

static int weapon(const char *n) { return weapon_named(n); }

int main(int argc, char **argv)
{
	static Pack W;
	if (argc < 3 || load(&W, argv[2])) { printf("usage: weaptest map.hgm weapons.hgm\n"); return 1; }
	u32 sz, isz, wsz, asz;
	u8 *wc = chunk("WEAP", &sz), *ic = chunk("ITEM", &isz), *sc = chunk("WSND", &wsz), *ac = chunk("AMMX", &asz);
	swap(wc, 1, "4");
	u32 nw = *(u32 *)wc;
	swap(wc + 4, nw, "4221121111444" "222222222222");
	swap(ic, 1, "4");
	u32 ni = *(u32 *)ic;
	swap(ic + 4, ni, "421122");
	swap(sc, wsz / 2, "2");
	swap(ac, asz / 2, "2");
	Weap_Setup(&W, wc + 4, nw, ic + 4, ni, (s16 *)sc, wsz / 2, (s16 *)ac, asz / 2);
	{
		/* the suit's sentences: u16 count, then u8 length + s16 words (swap the words for the host) */
		u32 hsz = 0;
		u8 *hc = chunk("HSEN", &hsz);
		int ns = 0, nwd = 0;
		if (hc) {
			u32 n = (u32)(hc[0] << 8 | hc[1]), off = 2;
			for (u32 i = 0; i < n && off < hsz; i++) {
				u32 len = (u32)(hc[off] << 8 | hc[off + 1]);
				off += 2;
				swap(hc + off, len, "2");
				ns += len > 0;
				nwd += len;
				off += len * 2u;
			}
		}
		Weap_SetSentences(hc, hsz);
		u32 ssz = 0;
		u8 *stc = chunk("STEP", &ssz);
		if (stc) {
			u32 n = (u32)(stc[0] << 8 | stc[1]);
			swap(stc + 4, n * 2, "2");
		}
		Weap_SetSteps(stc, ssz);
		printf("suit voice: %d sentences, %d words\n", ns, nwd);
	}
	printf("weapons pack: %u weapons, %u items, %u models, %u skeletons, %u sounds\n", nw, ni, W.numNpcModels,
	       W.numSkels, W.numSounds);
	int vmOk = 0, seqOk = 0;
	for (u32 i = 0; i < nw; i++) {
		if (weaps[i].vm >= 0) vmOk++;
		for (int s = 0; s < 8; s++) if (weaps[i].seq[s] >= 0) seqOk++;
	}
	printf("  %d viewmodels, %d animations\n", vmOk, seqOk);

	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl, Weap_Strip, Weap_Hurt};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) { printf("spawn failed\n"); return 1; }
	Weap_LevelStart(&P, &pl);
	tick(66);
	printf("map: %u world items\n", numWItems);
	{
		/* the suit's voice: a bad fall at full health, then a hit that leaves it critical */
		extern int stubEverywhere;
		inv.suit = 1; inv.health = 100; inv.armor = 0;
		int before = stubEverywhere;
		Weap_HurtFrom(40, DMG_FALL, NULL);
		tick(66 * 12);
		int afterFall = stubEverywhere - before;
		before = stubEverywhere;
		Weap_HurtFrom(45, DMG_BULLET, NULL);
		tick(66 * 12);
		int afterCrit = stubEverywhere - before;
		before = stubEverywhere;
		inv.armor = 40;
		f32 bat[3] = {0, 0, 0};
		(void)bat;
		suit_say(SUIT_P0 + 7, 30);
		tick(66 * 6);
		printf("suit voice: fall at 100 hp says %d words, critical hit %d words, battery %d words (health %.0f)\n",
		       afterFall, afterCrit, stubEverywhere - before, inv.health);
		f32 eyeP[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64};
		Weap_Input(0, 0, eyeP, 0, 0, 0);
		f32 from[3] = {pl.origin[0] + 200, pl.origin[1], pl.origin[2] + 64};
		inv.health = 100;
		Weap_HurtFrom(10, DMG_BLAST, from);
		printf("damage indicator: front %.1f right %.1f back %.1f left %.1f (hit from +x, yaw %.0f)\n", dmgDir[0],
		       dmgDir[1], dmgDir[2], dmgDir[3], viewYaw);
		inv.health = 100;
		before = stubEverywhere;
		for (int k = 0; k < 66 * 4; k++) {
			Weap_GeigerRange(120);
			tick(1);
		}
		printf("geiger: %d clicks in 4 s at 120 units\n", stubEverywhere - before);
		/* footsteps: the floor under the spawn point, then 3 s of running */
		extern int stubSoundsPlayed;
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
		tick(10);
		int mat = 0;
		int surf = step_surface(pl.origin);
		int found = 0, tried = 0;
		for (u32 i = 0; i < numWItems; i++) {
			if (!witems[i].placed) continue;
			tried++;
			found += step_surface(witems[i].pos) >= 0;
		}
		int b0 = stubSoundsPlayed, e0 = stubEverywhere;
		for (int k = 0; k < 66 * 3; k++) {
			pl.velocity[0] = 250; pl.velocity[1] = 0; pl.onGround = 1;
			footsteps(1.0f / 66);
		}
		printf("footsteps: spawn floor material %d surface %d, %d of %d item spots have a floor, %d steps in 3 s running\n",
		       mat, surf, found, tried, (stubSoundsPlayed - b0) - (stubEverywhere - e0));
		inv.health = 100;
	}

	/* pick-ups: walk onto each item */
	int took = 0, seen = 0;
	for (u32 i = 0; i < numWItems; i++) {
		WorldItem *it = &witems[i];
		if (!it->alive || !it->placed) continue;
		seen++;
		inv.health = 50; inv.armor = 0; inv.suit = 1;
		memset(inv.ammo, 0, sizeof(inv.ammo));
		Player_Spawn(&pl, it->pos, 0, 0);
		tick(3);
		if (!it->alive) took++;
		else if (getenv("DBG")) printf("  not taken: %s at (%.0f %.0f %.0f) player (%.0f %.0f %.0f)\n", wstr(items[it->item].name), it->pos[0], it->pos[1], it->pos[2], pl.origin[0], pl.origin[1], pl.origin[2]);
	}
	printf("pickups: %d of %d placed items taken\n", took, seen);

	Weap_GiveAll();
	printf("give all: %d weapons, health %.0f suit %.0f\n", (int)(inv.have[0] + inv.have[1] + inv.have[2] + inv.have[3] +
	       inv.have[4] + inv.have[5] + inv.have[6] + inv.have[7] + inv.have[8] + inv.have[9]), inv.health, inv.armor);

	/* shoot every NPC that isn't a protected story character */
	u32 ne, nn;
	Entity *ents = Ent_List(&ne);
	Npc *npcs = Npc_List(&nn);
	int targets = 0, killed = 0, deathAnims = 0, shots = 0;
	gCheats.god = 1;
	gCheats.infiniteAmmo = 1;
	for (u32 i = 0; i < nn; i++) {
		Npc *n = &npcs[i];
		Entity *e = &ents[n->ent];
		if (e->flags & (EF_DISABLED | EF_KILLED) || n->dead || Ent_Protected(n->ent)) continue;
		targets++;
		f32 hh = P.npcModels[n->model].height * 0.5f;
		if (hh > 40) hh = 40;
		if (hh < 4) hh = 4;
		f32 c[3] = {e->origin[0], e->origin[1], e->origin[2] + hh};
		stand_near(c, 120);
		select_weapon(weapon("weapon_smg1"));
		tick(70);
		for (int t = 0; t < 400 && !n->dead && !(e->flags & EF_KILLED); t++) {
			c[0] = e->origin[0]; c[1] = e->origin[1]; c[2] = e->origin[2] + hh;
			aim_at(c, WB_FIRE, t == 0 ? WB_FIRE : 0);
			tick(1);
			shots++;
		}
		if (n->dead || (e->flags & EF_KILLED)) { killed++; if (getenv("DBG")) printf("  killed %s seqDie %d seq %d dead %d\n", e->classname, n->skel->seqDie, n->seq, n->dead); }
		else if (getenv("DBG")) {
			f32 ey[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64}; Trace tr; f32 z[3] = {0,0,0};
			CM_BoxTrace(&tr, ey, c, z, z, MASK_SHOT);
			printf("  %s hp %.0f solid %d frac %.2f hitent %d (npc ent %d) dist %.0f clip %d ammo %d cur %d\n", e->classname, n->health, (e->flags & EF_SOLID) != 0, tr.fraction, tr.ent, n->ent,
			       sqrtf((c[0]-ey[0])*(c[0]-ey[0])+(c[1]-ey[1])*(c[1]-ey[1])), inv.clip[inv.cur], inv.ammo[weaps[inv.cur].ammo], (int)inv.cur);
		}
		if (n->dead && n->seq == n->skel->seqDie && n->seq >= 0) deathAnims++;
	}
	printf("npcs: %d targets, %d killed (%d with death animations), %d ticks firing\n", targets, killed, deathAnims, shots);

	/* crowbar every breakable prop */
	int breakables = 0, broke = 0, crates = 0;
	u32 itemsBefore = numWItems;
	select_weapon(weapon("weapon_crowbar"));
	tick(70);
	for (u32 i = 1; i < ne; i++) {
		Entity *e = &ents[i];
		if (e->cls != EC_PROP || (e->flags & (EF_KILLED | EF_DISABLED)) || !Ent_KV((s32)i, "_hp") || Ent_KV((s32)i, "_boom"))
			continue;
		breakables++;
		f32 mn[3], mx[3], c[3];
		Ent_Bounds((s32)i, mn, mx);
		for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f;
		stand_near(c, 50);
		for (int t = 0; t < 600 && !(e->flags & EF_KILLED); t++) {
			Ent_Bounds((s32)i, mn, mx);
			for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f;
			aim_at(c, WB_FIRE, WB_FIRE);
			tick(1);
		}
		if (getenv("DBG") && !(e->flags & EF_KILLED) && breakables < 6) { f32 ey[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64}; printf("  unbroken %s hp %s value %.0f cur %d dist %.0f\n", Ent_KV((s32)i, "model"), Ent_KV((s32)i, "_hp"), e->value, (int)inv.cur, sqrtf((c[0]-ey[0])*(c[0]-ey[0])+(c[1]-ey[1])*(c[1]-ey[1])+(c[2]-ey[2])*(c[2]-ey[2]))); }
		if (e->flags & EF_KILLED) { broke++; if (!strcmp(e->classname, "item_item_crate")) crates++; }
	}
	printf("breakables: %d, %d broken (%d crates, %u items dropped)\n", breakables, broke, crates, numWItems - itemsBefore);

	/* gravity gun: pull a light prop in, hold it, punt it */
	select_weapon(weapon("weapon_physcannon"));
	tick(70);
	int grabbed = 0, tried = 0;
	f32 puntSpeed = 0;
	for (u32 i = 1; i < ne && grabbed < 3; i++) {
		Entity *e = &ents[i];
		if (!Phys_IsBody((s32)i) || (e->flags & (EF_KILLED | EF_DISABLED)) || Phys_Mass((s32)i) > 100 || (e->spawnflags & 8)) continue;
		tried++;
		f32 mn[3], mx[3], c[3];
		Ent_Bounds((s32)i, mn, mx);
		for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f;
		stand_near(c, 300);
		for (int t = 0; t < 300 && Phys_HeldEnt() != (s32)i; t++) {
			Ent_Bounds((s32)i, mn, mx);
			for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f;
			aim_at(c, WB_ALT, t == 0 ? WB_ALT : 0);
			tick(1);
		}
		if (Phys_HeldEnt() != (s32)i) continue;
		grabbed++;
		tick(30);
		f32 before[3];
		memcpy(before, e->origin, 12);
		aim_at(c, WB_FIRE, WB_FIRE);
		tick(1);
		aim_at(c, 0, 0);
		tick(4);
		f32 d = sqrtf((e->origin[0] - before[0]) * (e->origin[0] - before[0]) + (e->origin[1] - before[1]) * (e->origin[1] - before[1]) +
		              (e->origin[2] - before[2]) * (e->origin[2] - before[2]));
		f32 sp = d / (5 * TICK_INTERVAL);
		if (sp > puntSpeed) puntSpeed = sp;
	}
	printf("gravity gun: %d of %d light props pulled and held, fastest punt %.0f units/s\n", grabbed, tried, puntSpeed);

	/* explosives: grenade, rocket, SMG grenade */
	Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	int booms = 0;
	const char *ex[3] = {"weapon_frag", "weapon_rpg", "weapon_smg1"};
	for (int k = 0; k < 3; k++) {
		select_weapon(weapon(ex[k]));
		tick(120);
		if (getenv("DBG")) printf("  %s: cur %d switchTo %d wait %.2f draw %.2f\n", ex[k], (int)inv.cur, (int)switchTo, nextAttack - wclock,
			Anim_SeqDuration(&W, vm_skel(inv.cur), weaps[inv.cur].seq[SQ_DRAW]));
		f32 c[3] = {pl.origin[0] + 200, pl.origin[1], pl.origin[2]};
		aim_at(c, k == 2 ? WB_ALT : WB_FIRE, k == 2 ? WB_ALT : WB_FIRE);
		tick(1);
		aim_at(c, 0, 0);
		int b0 = booms;
		for (int t = 0; t < 330; t++) {
			tick(1);
			for (int i = 0; i < MAX_FX; i++) if (fxs[i].alive && fxs[i].type == WFX_EXPLOSION && fxs[i].t < TICK_INTERVAL * 1.5f) booms++;
		}
		if (getenv("DBG")) printf("  %s: %d explosions (cur %d, ammo %d %d)\n", ex[k], booms - b0, (int)inv.cur, inv.ammo[A_GRENADE], inv.ammo[A_SMG1_GRENADE]);
	}
	int crN = 0; for (u32 i = 1; i < ne; i++) if (!strcmp(ents[i].classname, "item_item_crate")) crN++;
	if (getenv("DBG")) printf("  crates in map: %d\n", crN);
	printf("explosives: %d explosions from 3 launches\n", booms);

	/* every gun's reload */
	int reloads = 0;
	for (u32 w = 0; w < nw; w++) {
		if (weaps[w].clip <= 0) continue;
		select_weapon((s32)w);
		tick(80);
		inv.clip[w] = 0;
		inv.ammo[weaps[w].ammo] = 50;
		aim_at(P.spawnOrigin, 0, WB_RELOAD);
		tick(300);
		if (inv.clip[w] > 0) reloads++;
	}
	printf("reloads: %d guns reloaded\n", reloads);

	/* falling */
	gCheats.god = 0;
	gCheats.infiniteAmmo = 0;
	inv.health = 100;
	inv.armor = 0;
	Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	tick(30);
	for (u32 i = 1; i < ne; i++) {
		Player t2;
		if (!Player_Spawn(&t2, ents[i].origin, 0, 0)) continue;
		f32 up[3] = {t2.origin[0], t2.origin[1], t2.origin[2] + 900}, mn[3], mx[3]; Trace tr;
		Player_Bounds(&t2, mn, mx);
		CM_BoxTrace(&tr, t2.origin, up, mn, mx, MASK_PLAYERSOLID);
		if (tr.fraction > 0.7f) { pl = t2; tick(10); break; }
	}
	{
		f32 up[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 900}, mn[3], mx[3]; Trace tr;
		Player_Bounds(&pl, mn, mx);
		CM_BoxTrace(&tr, pl.origin, up, mn, mx, MASK_PLAYERSOLID);
		printf("  dropping from %.0f units up\n", tr.fraction * 900 - 8);
		pl.origin[2] += tr.fraction * 900 - 8;
		pl.onGround = 0;
	}
	f32 minvz = 0; int landed = 0;
	for (int i = 0; i < 300; i++) { tick(1); if (pl.velocity[2] < minvz) minvz = pl.velocity[2]; if (pl.onGround && !landed) landed = i; }
	if (getenv("DBG")) printf("  fall: min vz %.0f landed tick %d z %.0f\n", minvz, landed, pl.origin[2]);
	printf("landing at 800 units/s: health %.0f\n", inv.health);
	Weap_Strip(0);
	printf("strip: cur %d\n", (int)inv.cur);
	return 0;
}
