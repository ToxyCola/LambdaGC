/* Host test: Alyx walks from Point Insertion's end into Red Letter Day (NPCs through a level change) */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt; static Pack P2;
static const PackLandmark *lm(const Pack *p, const char *n) { for (u32 i = 0; i < p->numLandmarks; i++) if (!strcmp(p->landmarks[i].name, n)) return &p->landmarks[i]; return NULL; }
int main(int argc, char **argv)
{
	const char *mark = "landmark_trainstation_04-05";
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 0, &cb);
	Entity *ents = Ent_List(&cnt);
	for (u32 i = 1; i < cnt; i++) { const char *tn = Ent_KV((s32)i, "templatename"); if (tn && !strcasecmp(tn, "alyx")) { Ent_Input((s32)i, "Spawn", "", 0); printf("04 maker %u\n", i); break; } }
	ents = Ent_List(&cnt);
	s32 a = Ent_FindActor("alyx"), vol = -1;
	printf("04 alyx %d name %s flags %x\n", a, a > 0 && ents[a].name ? ents[a].name : "-", a > 0 ? ents[a].flags : 0);
	for (u32 i = 1; i < cnt; i++) if (!strcmp(ents[i].classname, "trigger_transition") && ents[i].name && !strcmp(ents[i].name, mark)) vol = (s32)i;
	f32 mn[3], mx[3]; Ent_Bounds(vol, mn, mx);
	f32 in[3] = {(mn[0]+mx[0])/2, (mn[1]+mx[1])/2, mn[2] + 2};
	ents[a].flags &= ~EF_DISABLED; Npc_Teleport(Npc_OfEntity(a), in, 90);
	const PackLandmark *from = lm(&P, mark);
	printf("vol %d %.0f %.0f %.0f .. %.0f %.0f %.0f prop %d dead %d\n", vol, mn[0],mn[1],mn[2],mx[0],mx[1],mx[2], Npc_OfEntity(a)->prop, Npc_OfEntity(a)->dead);
	printf("04: alyx %d at %.0f %.0f %.0f, landmark %s\n", a, ents[a].origin[0], ents[a].origin[1], ents[a].origin[2], from ? "found" : "MISSING");
	Ent_PrepareCarry(mark, from->origin);
	Ent_Free();
	load(&P2, argv[2]); CM_Init(&P2); Player_Spawn(&pl, P2.spawnOrigin, 0, P2.spawnAngles[1]);
	Ent_Spawn(&P2, &pl, 0, &cb);
	const PackLandmark *to = lm(&P2, mark);
	ents = Ent_List(&cnt);
	for (u32 i = 1; i < cnt; i++) if (ents[i].name && !strcmp(ents[i].name, "template_alyx")) printf("maker %u flags %x\n", i, ents[i].flags);
	printf("player ent0 %.0f %.0f %.0f\n", ents[0].origin[0], ents[0].origin[1], ents[0].origin[2]);
	Ent_FinishCarry(to->origin);
	if (Ent_FindActor("alyx") < 0) { Ent_Input(679, "Spawn", "", 0); printf("after direct Spawn: %d\n", Ent_FindActor("alyx")); }
	ents = Ent_List(&cnt);
	a = Ent_FindActor("alyx");
	Npc *n = a > 0 ? Npc_OfEntity(a) : NULL;
	printf("05: alyx %d flags %x npc %d at %.0f %.0f %.0f\n", a, a > 0 ? ents[a].flags : 0, n != NULL, a > 0 ? ents[a].origin[0] : 0, a > 0 ? ents[a].origin[1] : 0, a > 0 ? ents[a].origin[2] : 0);
	int ok = a > 0 && n && !(ents[a].flags & (EF_DISABLED | EF_KILLED | EF_TEMPLATE));
	printf("%s\n", ok ? "carrytest: ok" : "FAILED");
	return !ok;
}
