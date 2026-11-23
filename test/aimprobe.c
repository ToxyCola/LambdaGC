#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt;
int main(int argc, char **argv)
{
	load(&P, argv[1]); CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	Entity *ents = Ent_List(&cnt);
	for (u32 i = 1; i < cnt; i++) {
		if (strcmp(ents[i].classname, argv[2]) && !(ents[i].name && !strcmp(ents[i].name, argv[2]))) continue;
		Npc *n = Npc_OfEntity((s32)i); if (!n) continue;
		Npc_PlaySequence(n, "idlealert", 1);
		u32 si = (u32)(n->skel - P.skels);
		printf("skel %u of %u, aim table %u, prop %d\n", si, P.numSkels, P.numSkelAim, n->prop);
		if (si < P.numSkelAim) { const u16 *ai = P.skelAim[si]; printf("  controls %u..+%u\n", ai[0], ai[1]);
			for (u32 c = ai[0]; c < (u32)ai[0] + ai[1]; c++) printf("   bone %u param %u count %u delta %u %.0f..%.0f q%u\n", P.aimCtls[c].bone, P.aimCtls[c].param, P.aimCtls[c].count, P.aimCtls[c].delta, P.aimCtls[c].start, P.aimCtls[c].end, P.aimCtls[c].firstQuat); }
		printf("%s angles %.0f %.0f %.0f\n", argv[2], ents[i].angles[0], ents[i].angles[1], ents[i].angles[2]);
		f32 vals[5] = {-60, -30, 0, 30, 60};
		for (int pp = 0; pp < 2; pp++)
			for (int k = 0; k < 5; k++) {
				n->aimOn = 1; n->aimYaw = pp ? 0 : vals[k]; n->aimPitch = pp ? vals[k] : 0;
				f32 at[3], R[3][3];
				if (!Npc_Attachment(n, "eyes", at, R)) Npc_Attachment(n, "muzzle", at, R);
				f32 yaw = atan2f(R[1][0], R[0][0]) * 180 / M_PI, pitch = -asinf(R[2][0]) * 180 / M_PI;
				printf("  %s %+4.0f: eyes forward yaw %7.1f pitch %6.1f (down +)\n", pp ? "aim_pitch" : "aim_yaw  ", vals[k], yaw, pitch);
			}
		break;
	}
	return 0;
}
