/* Host test with the real audio code: plays a map (the player walks its story like storywalk) and reports
 * how many sounds went unheard: no free channel, no room to load, or a playing one pushed out for a new
 * one, and how many sounds were playing at the busiest moment.  usage: soundsim map.hgm vo.bin [seconds] */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "scene.h"
#include "audio.h"
void shim_mix(s32 n);
static FILE *bankf;
static int reader(u32 off, u32 len, void *buf) { if (fseek(bankf, off, SEEK_SET)) return -1; return fread(buf, 1, len, bankf) == len ? 0 : -1; }
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl; static u32 cnt; static f32 T; static int peak, peakHeld;
static void tick(f32 secs)
{
	UserCmd c = {0};
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
		Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Phys_Tick(TICK_INTERVAL); T += TICK_INTERVAL;
		shim_mix((s32)(AUDIO_RATE * TICK_INTERVAL));
		f32 ear[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 64};
		Audio_Update(ear, pl.yaw);
		int h, n = Audio_DebugCounts(&h);
		if (n > peak) peak = n;
		if (n + h > peakHeld) peakHeld = n + h;
	}
}
static u8 visited[8192];
static int has_outputs(s32 i) { return Ent_KV(i, "OnTrigger") || Ent_KV(i, "OnStartTouch") || Ent_KV(i, "ontrigger") || Ent_KV(i, "onstarttouch"); }
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	bankf = fopen(argv[2], "rb"); if (!bankf) return 1;
	f32 secs = argc > 3 ? (f32)atof(argv[3]) : 180;
	Audio_Init(reader);
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	tick(3);
	u32 last[4] = {0};
	while (T < secs) {
		Entity *E = Ent_List(&cnt);
		s32 best = -1; f32 bd = 1e30f;
		for (u32 i = 1; i < cnt && i < 8192; i++) {
			if (visited[i] || (E[i].flags & (EF_KILLED | EF_DISABLED))) continue;
			int trig = (!strcmp(E[i].classname, "trigger_once") || !strcmp(E[i].classname, "trigger_multiple")) && (E[i].spawnflags & 1) && has_outputs((s32)i);
			if (!trig) continue;
			const char *nm = E[i].name ? E[i].name : "";
			if (strstr(nm, "escape") || strstr(nm, "fail")) continue;
			f32 mn[3], mx[3]; Ent_WorldBounds((s32)i, mn, mx);
			f32 d = 0; for (int k = 0; k < 3; k++) { f32 c = (mn[k]+mx[k])/2 - pl.origin[k]; d += c*c; }
			if (d < bd) { bd = d; best = (s32)i; }
		}
		if (best >= 0) {
			visited[best] = 1;
			f32 mn[3], mx[3]; Ent_WorldBounds(best, mn, mx);
			pl.origin[0] = (mn[0]+mx[0])/2; pl.origin[1] = (mn[1]+mx[1])/2; pl.origin[2] = mn[2] + 1;
		}
		tick(10);
		const u32 *d = Audio_DebugDrops();
		if (d[0] != last[0] || d[1] != last[1] || d[2] != last[2])
			printf("[%6.1f] at %.0f %.0f %.0f: no channel %u, no room %u, pushed out %u (loops parked %u)\n", T, pl.origin[0], pl.origin[1], pl.origin[2], d[0] - last[0], d[1] - last[1], d[2] - last[2], d[3] - last[3]);
		memcpy(last, d, sizeof(last));
	}
	const u32 *d = Audio_DebugDrops();
	printf("%.0f s: most sounds at once %d (with paused %d); unheard: no channel %u, no room %u, pushed out %u\n", T, peak, peakHeld, d[0], d[1], d[2]);
	extern void *audioStopLog[]; extern int audioStopN;
	printf("sounds stopped part way: %d\n", audioStopN);
	for (int i = 0; i < audioStopN; i++) fprintf(stderr, "STOPAT %p\n", audioStopLog[i]);
	int ok = d[0] == 0 && d[1] == 0;
	printf("%s\n", ok ? "soundsim: ok" : "FAILED");
	return !ok;
}
