#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
#include <string.h>
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	UserCmd c = {0};
	f32 secs = argc > 2 ? atof(argv[2]) : 2;
	for (int t = 0; t < 66 * secs; t++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
	u32 ne; Entity *E = Ent_List(&ne);
	for (u32 i = 1; i < ne; i++)
		if ((E[i].name && strstr(E[i].name, "razor_train_gate")) || (E[i].parent > 0 && E[E[i].parent].name && strstr(E[E[i].parent].name, "razor_train_gate")))
			printf("%u %s %s origin (%.0f %.0f %.0f) frac %.2f state %d flags %x parent %d\n", i, E[i].classname, E[i].name ? E[i].name : "-", E[i].origin[0], E[i].origin[1], E[i].origin[2], E[i].frac, E[i].moveState, E[i].flags, E[i].parent);
	f32 mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72};
	for (f32 x = -4400; x <= -4080; x += 20) {
		f32 a[3] = {x, -700, -20}, b[3] = {x, -150, -20};
		Trace tr; CM_BoxTrace(&tr, a, b, mn, mx, MASK_PLAYERSOLID);
		printf("x %.0f: frac %.2f stop y %.0f ent %d (%s)\n", x, tr.fraction, tr.endpos[1], tr.ent, tr.ent > 0 ? E[tr.ent].classname : "world/static");
	}
	return 0;
}
