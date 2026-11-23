/* the HL1 test chamber in the intro's background: its switchable light styles after relay_start_hl1_scene */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include "light.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Light_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	for (int t = 0; t < 66 * 26; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); Light_StyleTick(TICK_INTERVAL); }
	for (int s = 32; s < 40; s++) printf("style %d value %.2f\n", s, Light_StyleValue(s));
	u32 cnt[64]={0}; for(u32 i=0;i<P.numStyleFaces;i++) cnt[P.styleFaces[i].style&63]++;
	for(int s=32;s<40;s++) printf("style %d: %u face layers\n",s,cnt[s]);
	return 0;
}
