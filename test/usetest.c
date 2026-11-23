/* Use: stand 48 units in front of each button and door, aim roughly at it (the stick's precision: up
 * to 10 degrees off), press use, and count how many respond (button pressed / door moving).
 * usage: usetest <map.hgm>... */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	int tried = 0, worked = 0;
	for (int a = 1; a < argc; a++) {
		static const f32 off[5][2] = {{0, 0}, {8, 0}, {-8, 0}, {0, 8}, {6, -6}};
		for (int o = 0; o < 5; o++) {
			Ent_Free();
			if (load(&P, argv[a])) continue;
			CM_Init(&P);
			Player_Spawn(&pl, P.spawnOrigin, 0, 0);
			EntCallbacks cb = {cl};
			Ent_Spawn(&P, &pl, 1, &cb);
			u32 n; Entity *E = Ent_List(&n);
			for (u32 i = 1; i < n; i++) {
				Entity *e = &E[i];
				int btn = e->cls == EC_FUNC_BUTTON && !(e->spawnflags & 256) && !e->locked;
				int door = e->cls == EC_PROP_DOOR && !e->locked && e->moveState == 0 && !(e->spawnflags & 32768);
				if (!btn && !door) continue;
				if (e->flags & (EF_DISABLED | EF_KILLED)) continue;
				f32 mn[3], mx[3];
				Ent_WorldBounds((s32)i, mn, mx);
				f32 c[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
				/* stand in front of it: try the 4 sides, take one with open floor and a clear view */
				int placed = 0; f32 eye[3];
				for (int s = 0; s < 4 && !placed; s++) {
					f32 d[2] = {s == 0 ? 1.f : s == 1 ? -1.f : 0, s == 2 ? 1.f : s == 3 ? -1.f : 0};
					f32 half = d[0] ? (mx[0] - mn[0]) / 2 : (mx[1] - mn[1]) / 2;
					f32 eyeZ = btn ? c[2] : mn[2] + 64;
					f32 p[3] = {c[0] + d[0] * (half + 48), c[1] + d[1] * (half + 48), eyeZ};
					Trace tr; f32 z[3] = {0, 0, 0};
					CM_BoxTrace(&tr, p, (f32[3]){c[0], c[1], eyeZ}, z, z, CONTENTS_SOLID);
					if (tr.startsolid || (tr.ent != (s32)i && tr.fraction < 0.9f)) continue;
					memcpy(eye, p, 12); placed = 1;
				}
				if (!placed) continue;
				f32 d[3] = {c[0] - eye[0], c[1] - eye[1], (btn ? c[2] : mn[2] + 50) - eye[2]};
				f32 yaw = atan2f(d[1], d[0]) * 57.29578f + off[o][0], pitch = -atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * 57.29578f + off[o][1];
				f32 fwd[3] = {cosf(pitch / 57.29578f) * cosf(yaw / 57.29578f), cosf(pitch / 57.29578f) * sinf(yaw / 57.29578f), -sinf(pitch / 57.29578f)};
				u8 before = e->moveState; int pressed0 = e->blocked;
				pl.origin[0] = eye[0]; pl.origin[1] = eye[1]; pl.origin[2] = eye[2] - 64;
				Ent_Use(eye, fwd);
				Ent_Tick(TICK_INTERVAL);
				tried++;
				int ok = e->moveState != before || e->blocked != pressed0;
				worked += ok;
				if (!ok && getenv("DBG")) printf("  %s %s aim off %.0f/%.0f: no response\n", btn ? "button" : "door", Ent_KV((s32)i, "targetname") ? Ent_KV((s32)i, "targetname") : "-", off[o][0], off[o][1]);
			}
		}
	}
	printf("%d uses tried, %d responded\n", tried, worked);
	return 0;
}
