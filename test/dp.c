#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int main(int c, char **v){
	load(&P, v[1]); CM_Init(&P);
	static Player pl; Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	u32 n; Entity *e = Ent_List(&n); Entity *d = NULL;
	for (u32 i = 1; i < n; i++) if (e[i].name && !strcmp(e[i].name, "barney_door_1")) d = &e[i];
	printf("door flags %x mdl %d npcModel %d cls %d origin %.0f %.0f %.0f\n", d->flags, d->mdl, d->npcModel, d->cls, d->origin[0], d->origin[1], d->origin[2]);
	for (u32 i = 1; i < n; i++) {
		f32 dx = e[i].origin[0]-d->origin[0], dy = e[i].origin[1]-d->origin[1];
		if (dx*dx+dy*dy < 200*200 && (e[i].flags & EF_SOLID)) printf("  solid near: %s %s flags %x npc %d (%.0f %.0f %.0f)\n", e[i].classname, e[i].name?e[i].name:"-", e[i].flags, e[i].npcModel, e[i].origin[0], e[i].origin[1], e[i].origin[2]);
	}
	f32 R0[3][3]; Ent_AngleMatrix(d->ang1, R0); f32 mid[3], a[3], b[3];
	for (int k = 0; k < 3; k++) mid[k] = d->pos1[k] + R0[k][0] * 24;
	for (int k = 0; k < 3; k++) { a[k] = mid[k] - R0[k][1] * 40; b[k] = mid[k] + R0[k][1] * 40; }
	a[2] = b[2] = d->pos1[2] - 54 + 2;
	f32 mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72};
	d->frac = 0; d->moveState = 0; memcpy(d->angles, d->ang1, 12); Ent_AngleMatrix(d->angles, d->R); Ent_UpdateCollision();
	Trace t; CM_SetSkipBoxes(1); CM_BoxTrace(&t, a, b, mn, mx, MASK_PLAYERSOLID); CM_SetSkipBoxes(0);
	printf("door R0 angles %.0f %.0f %.0f pos1 %.0f %.0f %.0f  a %.0f %.0f %.0f b %.0f %.0f %.0f  closed(no npcs) %.2f\n", d->ang1[0], d->ang1[1], d->ang1[2], d->pos1[0], d->pos1[1], d->pos1[2], a[0],a[1],a[2],b[0],b[1],b[2], t.fraction);
	return 0; }
/* door-only crossing */
int door_only(void);
