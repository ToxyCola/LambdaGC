#define main sim_main
#include "sim.c"
#undef main
int main(int argc, char **argv) {
	Pack B; load(&B, argv[1]); CM_Init(&B);
	f32 p[3] = {atof(argv[2]), atof(argv[3]), atof(argv[4])};
	Player pl; Player_Spawn(&pl, p, 0, 0);
	f32 c[3] = {pl.origin[0], pl.origin[1], pl.origin[2] + 36};
	s32 node = 0;
	while (node >= 0) { const PackNode *n = &B.nodes[node]; const PackPlane *pl2 = &B.planes[n->plane];
		node = n->children[(c[0]*pl2->n[0]+c[1]*pl2->n[1]+c[2]*pl2->n[2]-pl2->d) >= 0 ? 0 : 1]; }
	const PackLeaf *lf = &B.leafs[-1 - node];
	printf("origin (%.1f %.1f %.1f) leaf %d cluster %d brushes %u contents 0x%x\n", pl.origin[0], pl.origin[1], pl.origin[2], -1-node, lf->cluster, lf->numleafbrushes, CM_PointContents(c));
	for (u32 i = 0; i < lf->numleafbrushes; i++) { const PackBrush *b = &B.brushes[B.leafBrushes[lf->firstleafbrush + i]];
		printf("  brush contents 0x%x sides %u\n", b->contents, b->numsides); }
	f32 mn[3] = {-16,-16,0}, mx[3] = {16,16,72}; Trace t; CM_BoxTrace(&t, pl.origin, pl.origin, mn, mx, MASK_PLAYERSOLID);
	printf("box startsolid %d allsolid %d\n", t.startsolid, t.allsolid);
	return 0;
}
