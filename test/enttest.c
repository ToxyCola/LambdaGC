/* Run a map's entity logic on the host: spawn, idle through the intro, then walk the player into each
 * trigger; finally check that a prop door's collision follows the door. */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"

static void cl(const char *m, const char *l) { printf("*** CHANGELEVEL %s (%s)\n", m, l); }

static void door_collision(Pack *B, Entity *d)
{
	f32 mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72};
	f32 R0[3][3];
	Ent_AngleMatrix(d->ang1, R0);
	/* the door model spans local x 0..48 (width) and z -54..54 (height): cross the doorway's middle */
	f32 mid[3], a[3], b[3];
	for (int k = 0; k < 3; k++)
		mid[k] = d->pos1[k] + R0[k][0] * 24;
	for (int k = 0; k < 3; k++) {
		a[k] = mid[k] - R0[k][1] * 40;
		b[k] = mid[k] + R0[k][1] * 40;
	}
	a[2] = b[2] = d->pos1[2] - 54 + 2;
	for (int pass = 0; pass < 3; pass++) {
		if (pass < 2) {
			d->frac = pass ? 1 : 0;
			d->moveState = pass ? 2 : 0;
			for (int k = 0; k < 3; k++)
				d->angles[k] = d->ang1[k] + (d->ang2[k] - d->ang1[k]) * d->frac;
			Ent_AngleMatrix(d->angles, d->R);
			Ent_UpdateCollision();
		} else {
			CM_SetEntities(NULL, 0);
		}
		Trace tr, st;
		CM_BoxTrace(&tr, a, b, mn, mx, MASK_PLAYERSOLID);
		{ Trace t3; CM_SetSkipBoxes(1); CM_BoxTrace(&t3, a, b, mn, mx, MASK_PLAYERSOLID); CM_SetSkipBoxes(0); printf("  (without NPC bodies: %.2f)\n", t3.fraction); }
		CM_BoxTrace(&st, a, a, mn, mx, MASK_PLAYERSOLID);
		printf("  %s start in solid %d, crossing the doorway stops at %.2f of the way\n",
		       pass == 0 ? "door closed: " : pass == 1 ? "door open:   " : "no entities: ", st.startsolid, tr.fraction);
	}
	(void)B;
}

int main(int argc, char **argv)
{
	Pack B;
	if (load(&B, argv[1]))
		return 1;
	CM_Init(&B);
	static Player pl;
	f32 sp[3] = {B.spawnOrigin[0], B.spawnOrigin[1], B.spawnOrigin[2]};
	Player_Spawn(&pl, sp, 0, B.spawnAngles[1]);
	EntCallbacks cb = {cl};
	Ent_Spawn(&B, &pl, 1, &cb);
	f32 secs = argc > 2 ? atof(argv[2]) : 30;
	UserCmd idle = {0, 0, 0, 0, B.spawnAngles[1]};
	for (int t = 0; t < (int)(secs / TICK_INTERVAL); t++) {
		Player_Tick(&pl, &idle);
		Ent_Tick(TICK_INTERVAL);
	}
	printf("player after intro: (%.0f %.0f %.0f) yaw %.0f\n", pl.origin[0], pl.origin[1], pl.origin[2], pl.yaw);

	u32 n;
	Entity *e = Ent_List(&n);
	int fired = 0;
	for (u32 i = 1; i < n; i++) {
		if (e[i].cls != EC_TRIGGER_ONCE && e[i].cls != EC_TRIGGER_MULTIPLE)
			continue;
		if (!e[i].enabled || (e[i].flags & EF_KILLED) || e[i].bmodel <= 0)
			continue;
		const PackBModel *bm = &B.bmodels[e[i].bmodel];
		f32 c[3];
		for (int k = 0; k < 3; k++)
			c[k] = (bm->mins[k] + bm->maxs[k]) * 0.5f + e[i].origin[k];
		c[2] = bm->mins[2] + e[i].origin[2] + 1;
		memcpy(pl.origin, c, 12);
		memset(pl.velocity, 0, 12);
		printf("--- player into trigger %s (%s)\n", e[i].name ? e[i].name : "-", e[i].classname);
		for (int t = 0; t < 400; t++)
			Ent_Tick(TICK_INTERVAL);
		fired++;
	}
	printf("walked into %d triggers\n", fired);
	{ extern u32 sceneGestures, sceneLooks; printf("scene gestures played %u, look-at events %u\n", sceneGestures, sceneLooks); }

	int doors = 0, open = 0;
	for (u32 i = 0; i < n; i++)
		if (e[i].cls == EC_FUNC_DOOR || e[i].cls == EC_PROP_DOOR || e[i].cls == EC_FUNC_DOOR_ROTATING) {
			doors++;
			open += e[i].frac > 0.99f;
		}
	printf("%d doors, %d open\n", doors, open);
	for (u32 i = 1; i < n; i++)
		if (e[i].name && !strcmp(e[i].name, "barney_door_1")) {
			printf("barney_door_1 (prop_door_rotating, model %d):\n", e[i].mdl);
			door_collision(&B, &e[i]);
		}
	return 0;
}
