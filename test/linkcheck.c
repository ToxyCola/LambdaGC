/* Host test: every node-graph link Source's NPCs can walk should be walkable in our collision (a 26-wide hull
 * from node to node at step height). Reports links that are blocked and what blocks them (invisible walls). */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
int main(int argc, char **argv)
{
	if (load(&P, argv[1])) return 1;
	CM_Init(&P); Player_Spawn(&pl, P.spawnOrigin, 0, 0);
	EntCallbacks cb = {cl}; Ent_Spawn(&P, &pl, 1, &cb);
	int links = 0, blocked = 0, byWorld = 0, byEnt = 0;
	int show = argc > 2 ? atoi(argv[2]) : 10;
	u32 ne; Entity *E = Ent_List(&ne);
	for (u32 i = 0; i < P.numNavNodes; i++) {
		const PackNavNode *a = &P.navNodes[i];
		if (a->type != NAVN_GROUND) continue;
		for (u32 l = 0; l < a->numLinks; l++) {
			const PackNavLink *L = &P.navLinks[a->firstLink + l];
			if (L->node <= i || !(L->moves & NAVM_WALK)) continue;
			const PackNavNode *b = &P.navNodes[L->node];
			links++;
			f32 s[3] = {a->pos[0], a->pos[1], a->pos[2] + 20}, e[3] = {b->pos[0], b->pos[1], b->pos[2] + 20};
			f32 mn[3] = {-12, -12, 0}, mx[3] = {12, 12, 50};
			Trace tr;
			CM_SetSkipBoxes(1);
			CM_BoxTrace(&tr, s, e, mn, mx, MASK_NPCSOLID); /* what NPCs collide with (not player clips) */
			CM_SetSkipBoxes(0);
			if (tr.startsolid || tr.fraction < 1.0f) {
				blocked++;
				if (tr.ent > 0) byEnt++; else byWorld++;
				f32 dir[3] = {e[0]-s[0], e[1]-s[1], e[2]-s[2]}; f32 dl = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
				f32 probe[3]; int pc = 0;
				for (int k = 0; k < 7; k++) { for (int c = 0; c < 3; c++) probe[c] = tr.endpos[c] + dir[c] / dl * (2 + k * 2) + (c == 2 ? 25 : 0); pc |= CM_PointContents(probe); }
				if (show > 0) printf("    contents past the hit: %x\n", pc);
				if (show > 0 && tr.ent < 0) {
					/* which static prop: the one whose rotated hull box holds the hit */
					extern void Ent_AngleMatrix(const f32 angles[3], f32 R[3][3]);
					for (u32 q = 0; q < P.numStaticProps; q++) {
						const PackStaticProp *sp = &P.staticProps[q];
						if (!sp->solid || sp->model >= P.numModels) continue;
						f32 R[3][3]; Ent_AngleMatrix(sp->angles, R);
						const PackModel *pm = &P.models[sp->model];
						for (u32 hh = 0; hh < pm->numHulls; hh++) {
							const PackHull *H = &P.hulls[pm->firstHull + hh];
							f32 lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
							for (int cc = 0; cc < 8; cc++) {
								f32 v[3] = {(cc & 1) ? H->maxs[0] : H->mins[0], (cc & 2) ? H->maxs[1] : H->mins[1], (cc & 4) ? H->maxs[2] : H->mins[2]};
								for (int r = 0; r < 3; r++) { f32 w = R[r][0]*v[0]+R[r][1]*v[1]+R[r][2]*v[2]+sp->origin[r]; if (w < lo[r]) lo[r] = w; if (w > hi[r]) hi[r] = w; }
							}
							if (tr.endpos[0] + 13 > lo[0] && tr.endpos[0] - 13 < hi[0] && tr.endpos[1] + 13 > lo[1] && tr.endpos[1] - 13 < hi[1] &&
							    tr.endpos[2] + 51 > lo[2] && tr.endpos[2] - 1 < hi[2])
								printf("    static prop %u at (%.0f %.0f %.0f) hull %u box (%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n", q, sp->origin[0], sp->origin[1], sp->origin[2], hh, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
						}
					}
				}
				if (show-- > 0)
					printf("  blocked %d->%d at (%.0f %.0f %.0f) frac %.2f startsolid %d by %s %s surface %d\n", i, L->node, tr.endpos[0], tr.endpos[1], tr.endpos[2], tr.fraction, tr.startsolid,
					       tr.ent > 0 ? E[tr.ent].classname : (tr.ent == 0 ? "world" : "world/static prop"), tr.ent > 0 && E[tr.ent].name ? E[tr.ent].name : "", tr.surface);
			}
		}
	}
	printf("%s: %d walk links, %d blocked (%d by entities, %d by world/props)\n", argv[1], links, blocked, byEnt, byWorld);
	return 0;
}
