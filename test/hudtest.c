/* HUD: with the real weapons.hgm and fonts, pick things up and draw the HUD through the GX checker:
 * the pickup history's entries, the weapon selection's icon, squad and poison panels; no GX errors. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "gxv.h"
#include "../engine/source/weapons.c"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
static Pack W;
int Pack_LoadMemory(Pack *p, u8 *d, u32 n) { (void)p; (void)d; (void)n; return -1; }
void *Pack_Chunk(const Pack *p, const char *t, u32 *n) { (void)p; (void)t; *n = 0; return NULL; }
static u8 *slurp(const char *p, u32 *n) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, 2); *n = ftell(f); rewind(f);
	u8 *d = aligned_alloc(32, (*n + 31) & ~31u); fread(d, 1, *n, f); fclose(f); return d; }
int main(int argc, char **argv)
{
	/* argv: map, hl2gc dir */
	char p[512]; u32 n;
	snprintf(p, sizeof p, "%s/font_title.bin", argv[2]); u8 *d = slurp(p, &n); printf("title font %d\n", d ? Text_LoadFont(FONT_TITLE, d, n) : -1);
	snprintf(p, sizeof p, "%s/font_icons.bin", argv[2]); d = slurp(p, &n); printf("icon font %d\n", d ? Text_LoadFont(FONT_ICONS, d, n) : -1);
	snprintf(p, sizeof p, "%s/font.bin", argv[2]); d = slurp(p, &n); if (d) Text_LoadFont(FONT_CAPTION, d, n);
	snprintf(p, sizeof p, "%s/weapons.hgm", argv[2]);
	if (load(&W, p)) { printf("weapons.hgm failed\n"); return 1; }
	u32 sz, isz, wsz, asz, csz;
	u8 *wc = chunk("WEAP", &sz), *ic = chunk("ITEM", &isz), *sc = chunk("WSND", &wsz), *ac = chunk("AMMX", &asz), *icn = chunk("WICN", &csz);
	swap(wc, 1, "4"); u32 nw = *(u32 *)wc; swap(wc + 4, nw, "4221121111444" "222222222222");
	swap(ic, 1, "4"); u32 ni = *(u32 *)ic; swap(ic + 4, ni, "421122");
	swap(sc, wsz / 2, "2"); swap(ac, asz / 2, "2");
	Weap_Setup(&W, wc + 4, nw, ic + 4, ni, (s16 *)sc, wsz / 2, (s16 *)ac, asz / 2);
	wp = W; loaded = 1;
	if (icn) memcpy(wIcon, icn, csz < sizeof(wIcon) ? csz : sizeof(wIcon));
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Weap_LevelStart(&P, &pl);
	inv.suit = 1;
	printf("icons: smg1 '%c' ammo '%c' alt '%c'\n", wIcon[weapon_named("weapon_smg1")][0], wIcon[weapon_named("weapon_smg1")][1], wIcon[weapon_named("weapon_smg1")][2]);
	give_weapon(weapon_named("weapon_smg1"));
	give_ammo(A_SMG1, 45); give_ammo(A_SMG1, 45); give_ammo(A_SMG1_GRENADE, 1);
	inv.armor = 20; { WorldItem wi = {0}; for (u32 i = 0; i < numItems; i++) if (items[i].kind == K_SUIT) { wi.item = (s16)i; take_item(&wi); break; } }
	for (int i = 0; i < MAX_HIST; i++) if (hist[i].alive) printf("  history %d: kind %d icon '%c' second '%c' count %d\n", i, hist[i].kind, hist[i].icon ? hist[i].icon : ' ', hist[i].icon2 ? hist[i].icon2 : ' ', hist[i].count);
	Weap_HurtFrom(50, DMG_POISON, NULL);
	printf("poison: health %.0f, %0.f to come back\n", inv.health, poisonLeft);
	f32 proj[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
	gxv.maxDist = 40000;
	selShowUntil = wclock + 2;
	for (int f = 0; f < 30; f++) { Text_Begin(640, 480); Weap_DrawHud(640, 480); Text_End(proj); gxv_end_frame(); }
	for (int t = 0; t < 66 * 8; t++) Weap_Tick(1.0f / 66);
	printf("after 8 s: health %.0f, poison left %.0f; history entries alive %d\n", inv.health, poisonLeft, hist[0].alive + hist[1].alive + hist[2].alive + hist[3].alive + hist[4].alive);
	printf("draws %lu vertices %lu errors %lu overflow %lu\n", gxv.draws, gxv.vertices, gxv.errors, gxv.overflow);
	return 0;
}
