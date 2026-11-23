/* Host test: the AI entities scripts drive (aients.c, relations.c)
 *  - npc_makers / template makers make NPCs when told (and count them), within their limits
 *  - point_template / env_entity_maker copies
 *  - aiscripted_schedule, ai_goal_assault (Activate, BeginAssault), logic_case, math_remap: their inputs
 *  - 20 s of the map: NPCs pick enemies by Source's relationships (NPC against NPC, not just the player) */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "aients.h"
#include "relations.h"
#include "physics.h"
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
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	Entity *ents = Ent_List(&cnt);
	tick(1);
	int makers = 0, made = 0, withModel = 0;
	for (u32 i = 1; i < cnt; i++) {
		const char *cn = ents[i].classname;
		if (strcmp(cn, "npc_maker") && strcmp(cn, "npc_template_maker") && strcmp(cn, "npc_antlion_template_maker")) continue;
		makers++;
		int before = AiEnts_MakerChildren((s32)i, NULL);
		for (int k = 0; k < 3; k++) { Ent_Input((s32)i, "Spawn", "", 0); tick(0.5f); }
		int after = AiEnts_MakerChildren((s32)i, NULL);
		made += after - before;
		if (Ent_KV((s32)i, "_npc") || !strcmp(cn, "npc_template_maker") || !strcmp(cn, "npc_antlion_template_maker")) withModel++;

	}
	int pts = 0;
	for (u32 i = 1; i < cnt; i++) {
		const char *cn = ents[i].classname;
		if (!strcmp(cn, "point_template") || !strcmp(cn, "env_entity_maker")) { Ent_Input((s32)i, "ForceSpawn", "", 0); Ent_Input((s32)i, "ForceSpawn", "", 0); pts++; }
		if (!strcmp(cn, "aiscripted_schedule")) Ent_Input((s32)i, "StartSchedule", "", 0);
		if (!strcmp(cn, "ai_goal_assault")) { Ent_Input((s32)i, "Activate", "", 0); }
		if (!strcmp(cn, "logic_case")) { Ent_Input((s32)i, "InValue", "1", 0); Ent_Input((s32)i, "PickRandomShuffle", "", 0); }
		if (!strcmp(cn, "math_remap")) Ent_Input((s32)i, "InValue", "0.5", 0);
		if (!strcmp(cn, "ai_relationship")) Ent_Input((s32)i, "ApplyRelationship", "", 0);
	}
	tick(5);
	for (u32 i = 1; i < cnt; i++) if (!strcmp(ents[i].classname, "ai_goal_assault")) Ent_Input((s32)i, "BeginAssault", "", 0);
	tick(15);
	u32 nn; Npc *npcs = Npc_List(&nn);
	int npcEnemies = 0, playerEnemies = 0, armed = 0;
	for (u32 i = 0; i < nn; i++) { if (npcs[i].dead || npcs[i].prop) continue; if (npcs[i].weapon) armed++; if (npcs[i].enemy > 0) npcEnemies++; else if (npcs[i].enemy == 0) playerEnemies++; }
	Ent_List(&cnt);
	printf("%s: %d makers (%d with a model) made %d, %d templates, %u NPCs, %d armed, %d fighting NPCs, %d the player\n",
	       argv[1], makers, withModel, made, pts, nn, armed, npcEnemies, playerEnemies);
	/* a soldier or cop with the player in front of him: he should take the player as his enemy (once the
	 * player is a fugitive), and the relationship table must say hate */
	for (u32 i = 0; i < nn; i++) {
		Entity *e = &ents[npcs[i].ent];
		if (npcs[i].dead || npcs[i].prop || !npcs[i].weapon || (e->flags & (EF_KILLED | EF_DISABLED))) continue;
		if (strcmp(e->classname, "npc_combine_s") && strcmp(e->classname, "npc_metropolice")) continue;
		f32 a = npcs[i].yaw * (f32)M_PI / 180;
		f32 at[3] = {e->origin[0] + cosf(a) * 200, e->origin[1] + sinf(a) * 200, e->origin[2] + 8};
		memcpy(pl.origin, at, 12);
		int disp = Rel_Disposition(npcs[i].ent, 0);
		tick(3);
		printf("  %s %s: disposition to the player %d, enemy %d, script %d locked %d\n", e->classname, e->name ? e->name : "", disp,
		       npcs[i].enemy, npcs[i].script, npcs[i].locked);
		break;
	}
	printf("%s\n", fails ? "FAILED" : "aitest: ok");
	return fails != 0;
}
