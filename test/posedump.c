/* posedump <pack> <skeleton> <base seq> <gesture or -> <cycle> <time> [lookYaw lookPitch]: skinning matrices */
#include "ent.h"
#include "collide.h"
#include "npc.h"
#define main sim_main
#include "sim.c"
#undef main
static f32 o[256][3][4];
int main(int argc, char **argv)
{
	if (argc < 7 || load(&P, argv[1]))
		return 1;
	const PackSkel *sk = &P.skels[atoi(argv[2])];
	s16 base = Anim_FindSeq(&P, sk, argv[3]);
	AnimLayer L = {Anim_FindSeq(&P, sk, argv[4]), (f32)atof(argv[5]), 1.0f, (f32)atof(argv[6])};
	AnimExtra ex = {&L, L.seq >= 0, argc > 8 ? (f32)atof(argv[7]) : 0, argc > 8 ? (f32)atof(argv[8]) : 0, argc > 8 ? 1 : 0};
	Anim_PoseEx(&P, sk, base, 0, 0, -1, 0, 1, &ex, o);
	printf("%u\n", sk->numBones);
	for (u32 i = 0; i < sk->numBones; i++)
		for (int r = 0; r < 3; r++)
			printf("%.5f %.5f %.5f %.5f\n", o[i][r][0], o[i][r][1], o[i][r][2], o[i][r][3]);
	return 0;
}
