/* Barney's helmet: after his init relay parents the helmet and faceplate to his "helmet_attachment",
 * where do their models sit relative to his eyes? Prints offsets and axes (numbers only). */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	Ent_Spawn(&P, &pl, 1, &cb);
	int secs = argc > 2 ? atoi(argv[2]) : 2;
	{ u32 ne0; Entity *E0 = Ent_List(&ne0);
	  for (u32 i = 1; i < ne0; i++) { const char *tn = Ent_KV((s32)i, "targetname"); if (tn && !strcmp(tn, "gordon_cop_template")) Ent_Input((s32)i, "ForceSpawn", "", 0); } (void)E0; }
	for (int t = 0; t < 66 * secs; t++) Ent_Tick(TICK_INTERVAL);
	u32 ne; Entity *E = Ent_List(&ne);
	s32 b = -1;
	for (u32 i = 1; i < ne; i++) { const char *tn = Ent_KV((s32)i, "targetname"); if (tn && !strcmp(tn, "barney") && E[i].cls == EC_NPC) b = (s32)i; }
	if (b < 0) { printf("no barney\n"); return 0; }
	Npc *n = Npc_OfEntity(b);
	f32 ep[3], eR[3][3], hp[3], hR[3][3];
	int he = Npc_Attachment(n, "eyes", ep, eR), hh = Npc_Attachment(n, "helmet_attachment", hp, hR);
	printf("barney at %.1f %.1f %.1f yaw %.0f; eyes %s at +%.1f %.1f %.1f fwd %.2f %.2f %.2f; helmet_attachment %s at +%.1f %.1f %.1f\n",
	       E[b].origin[0], E[b].origin[1], E[b].origin[2], n->yaw, he ? "found" : "MISSING", ep[0] - E[b].origin[0], ep[1] - E[b].origin[1], ep[2] - E[b].origin[2],
	       eR[0][0], eR[1][0], eR[2][0], hh ? "found" : "MISSING", hp[0] - E[b].origin[0], hp[1] - E[b].origin[1], hp[2] - E[b].origin[2]);
	for (u32 i = 1; i < ne; i++) {
		const char *tn = Ent_KV((s32)i, "targetname");
		if (!tn || !strstr(tn, "elmet")) continue;
		Entity *e = &E[i];
		if (e->mdl < 0) { printf("%s: no model\n", tn); continue; }
		const PackModel *m = &P.models[e->mdl];
		f32 c[3] = {(m->mins[0] + m->maxs[0]) / 2, (m->mins[1] + m->maxs[1]) / 2, (m->mins[2] + m->maxs[2]) / 2}, w[3];
		for (int k = 0; k < 3; k++) w[k] = e->origin[k] + e->R[k][0] * c[0] + e->R[k][1] * c[1] + e->R[k][2] * c[2];
		printf("%s: parent %d attach '%s' %s; model box %.1f..%.1f %.1f..%.1f %.1f..%.1f; its centre is %.1f %.1f %.1f from the eyes (%.1f ahead along the eyes' forward); model x axis . eyes fwd %.2f, z . eyes up %.2f\n",
		       tn, e->parent, e->parentAttach, (e->flags & EF_DISABLED) ? "hidden" : "shown", m->mins[0], m->maxs[0], m->mins[1], m->maxs[1], m->mins[2], m->maxs[2],
		       w[0] - ep[0], w[1] - ep[1], w[2] - ep[2], (w[0] - ep[0]) * eR[0][0] + (w[1] - ep[1]) * eR[1][0] + (w[2] - ep[2]) * eR[2][0],
		       e->R[0][0] * eR[0][0] + e->R[1][0] * eR[1][0] + e->R[2][0] * eR[2][0], e->R[0][2] * eR[0][2] + e->R[1][2] * eR[1][2] + e->R[2][2] * eR[2][2]);
	}
	return 0;
}
