/* Host test: the view riding Trainstation 1's train, as main.c draws it at 60 Hz from 66 Hz ticks. The
 * camera interpolates between the last two ticks while the train is drawn where it is: the camera's
 * position relative to the drawn train should hold still when the player does (it jittered by up to a
 * tick of the train's motion before main.c kept the interpolation relative to the ride). */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)l; (void)m; }
static Player pl;
static Entity *named(const char *n)
{
	u32 c; Entity *e = Ent_List(&c);
	for (u32 i = 1; i < c; i++) if (e[i].name && !strcasecmp(e[i].name, n)) return &e[i];
	return NULL;
}
static void ride_run(int relative, const char *map)
{
	load(&P, map);
	CM_Init(&P);
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	Ent_Spawn(&P, &pl, 1, &cb);
	Entity *train = named("intro_train_2");
	UserCmd c = {0};
	f32 accum = 0, prev[3], dev = 0, mean[3] = {0, 0, 0}, sq = 0;
	int frames = 0;
	f32 rel[4000][3];
	for (int f = 0; f < 60 * 90; f++) {
		accum += 1.0f / 60;
		while (accum >= TICK_INTERVAL) {
			memcpy(prev, pl.origin, 12);
			Player_Tick(&pl, &c);
			Ent_Tick(TICK_INTERVAL);
			if (relative) {
				f32 car[3];
				Ent_PlayerCarry(car);
				for (int k = 0; k < 3; k++) prev[k] += car[k];
			}
			accum -= TICK_INTERVAL;
		}
		f32 a = accum / TICK_INTERVAL;
		int riding = pl.groundEnt == (s32)(train - Ent_List(&(u32){0}));
		f32 sp = 0;
		for (int k = 0; k < 2; k++) sp += (train->origin[k] - train->pos1[k]) * 0;
		if (riding && Ent_Now() > 64 && Ent_Now() < 85 && frames < 4000) {
			f32 w[3];
			for (int k = 0; k < 3; k++) w[k] = prev[k] + (pl.origin[k] - prev[k]) * a - train->origin[k];
			for (int k = 0; k < 3; k++) rel[frames][k] = train->R[0][k] * w[0] + train->R[1][k] * w[1] + train->R[2][k] * w[2];
			frames++;
		}
	}
	/* frame-to-frame change of the camera's place in the train (in the train's own frame) */
	f32 worst = 0, sum = 0;
	for (int i = 1; i < frames; i++) {
		f32 d = 0;
		for (int k = 0; k < 2; k++) d += (rel[i][k] - rel[i - 1][k]) * (rel[i][k] - rel[i - 1][k]);
		d = sqrtf(d);
		sum += d;
		if (d > worst) worst = d;
		if (d > 2 && relative) printf("   frame %d jump %.1f rel (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n", i, d, rel[i-1][0], rel[i-1][1], rel[i-1][2], rel[i][0], rel[i][1], rel[i][2]);
	}
	printf("%s: %d frames riding; camera vs drawn train moves %.2f units a frame on average, %.2f at worst\n",
	       relative ? "ride-relative interpolation" : "plain interpolation", frames, frames > 1 ? sum / (frames - 1) : 0, worst);
	(void)prev; (void)dev; (void)mean; (void)sq;
}
int main(int argc, char **argv)
{
	ride_run(0, argv[1]);
	ride_run(1, argv[1]);
	return 0;
}
