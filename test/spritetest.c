/* Host test: env_sprite / env_lightglow / point_spotlight setup on a map. usage: spritetest map.hgm */
#include "ent.h"
#include "collide.h"
#define main sim_main
#include "sim.c"
#undef main
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int main(int argc, char **argv)
{
	if (argc < 2 || load(&P, argv[1])) return 1;
	CM_Init(&P);
	static Player pl;
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	u32 n;
	Entity *e = Ent_List(&n);
	int spr = 0, on = 0, tex = 0, glow = 0, spot = 0, beam = 0, badTex = 0;
	for (u32 i = 1; i < n; i++) {
		if (e[i].cls != EC_SPRITE && e[i].cls != EC_LIGHTGLOW) continue;
		spr++;
		on += e[i].enabled;
		tex += e[i].tex > 0;
		badTex += e[i].tex >= (s32)P.numTex;
		glow += e[i].cls == EC_LIGHTGLOW || e[i].rendermode == 3 || e[i].rendermode == 9;
		if (!strcmp(e[i].classname, "point_spotlight")) { spot++; beam += e[i].tex2 > 0; }
	}
	printf("%s: %d sprites (%d on at start, %d with a texture, %d out of range), %d glows, %d spotlights (%d beams)\n",
	       argv[1], spr, on, tex, badTex, glow, spot, beam);
	return 0;
}
