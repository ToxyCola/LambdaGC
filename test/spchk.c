/* static props near a point: model index, bounds, cluster (the pack's and the point's), triangle count */
#define main sim_main
#include "sim.c"
#undef main
#include "world.h"
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	f32 x = atof(argv[2]), y = atof(argv[3]), z = atof(argv[4]), r = argc > 5 ? atof(argv[5]) : 64;
	for (u32 i = 0; i < P.numStaticProps; i++) {
		PackStaticProp *s = &P.staticProps[i];
		if (fabsf(s->origin[0] - x) > r || fabsf(s->origin[1] - y) > r || fabsf(s->origin[2] - z) > r) continue;
		const PackModel *m = &P.models[s->model];
		printf("sp %u model %u at %.0f %.0f %.0f ang %.0f %.0f %.0f solid %u flags %x cluster %d (point says %d) fade %d; model box %.0f..%.0f %.0f..%.0f %.0f..%.0f\n",
		       i, s->model, s->origin[0], s->origin[1], s->origin[2], s->angles[0], s->angles[1], s->angles[2], s->solid, s->flags, s->cluster, World_PointCluster(&P, s->origin), s->pad,
		       m->mins[0], m->maxs[0], m->mins[1], m->maxs[1], m->mins[2], m->maxs[2]);
		printf("  meshes %u first %u; mesh0 indices %u frac %x\n", m->numMeshes, m->firstMesh, m->numMeshes ? P.meshes[m->firstMesh].numIndices : 0, m->numMeshes ? P.meshes[m->firstMesh].frac : 0);
		if (P.spClusterRange) { printf("  clusters:"); for (u32 k = 0; k < P.spClusterRange[i][1]; k++) printf(" %u", P.spClusters[P.spClusterRange[i][0] + k]); printf("\n"); }
	}
	return 0;
}
