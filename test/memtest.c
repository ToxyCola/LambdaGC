/* heap a map needs on top of its pack (host build: 64-bit pointers, so an overestimate) */
#include <malloc.h>
#define main sim_main
#include "sim.c"
#undef main
#include "weapons.h"
#include "gibs.h"
#include "vehicle.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
int main(int argc, char **argv)
{
	size_t base = mallinfo2().uordblks;
	if (load(&P, argv[1])) return 1;
	size_t afterPack = mallinfo2().uordblks;
	CM_Init(&P);
	static Player pl;
	Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
	EntCallbacks cb = {cl};
	if (Ent_Spawn(&P, &pl, 1, &cb) != 0) return 1;
	size_t world = (P.numClusters + P.numFaces * 8 + P.numMats * 8);
	printf("%s pack %zu KB, setup %zu KB (+world ~%zu KB)\n", argv[1], (afterPack - base) / 1024,
	       (mallinfo2().uordblks - afterPack) / 1024, world / 1024);
	return 0;
}
