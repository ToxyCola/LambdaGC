/*
 * hl2gc: the map's ropes and effect entities, after Source's
 *   move_rope / keyframe_rope   (rope.cpp, c_rope.cpp, rope_physics.cpp: Verlet cables between keyframes)
 *   env_spark                   (EnvSpark.cpp)
 *   env_explosion               (explode.cpp)
 *   env_physexplosion / env_physimpact (physobj.cpp)
 *   env_beam / point_tesla      (EnvBeam.cpp, tesla.cpp: lightning between points)
 *   env_shooter                 (effects.cpp: throws gibs)
 *   env_fire                    (fire.cpp: burning, hurting, spreading heat)
 *   env_splash / env_muzzleflash / env_gunfire (effects.cpp)
 *   env_dustpuff / env_ar2explosion (func_dust.cpp / hl2's ar2_explosion)
 *   func_physbox inputs (physobj.cpp: EnableMotion, DisableMotion, Wake, Sleep)
 */
#ifndef HL2GC_ENVENTS_H
#define HL2GC_ENVENTS_H

#include "pack.h"

void EnvEnts_Spawn(void);  /* after Ent_Spawn */
void EnvEnts_Free(void);
/* an input to one of these entities; 1 if handled */
int EnvEnts_Input(s32 ent, const char *in, const char *param, s32 activator);
void EnvEnts_Tick(f32 dt);

/* ---- what to draw (weapons.c draws them with the effect textures) */
typedef struct {
	const f32 (*pts)[3];
	int n;
	f32 width;
	u32 rgba;
} RopeDraw;
int EnvEnts_NumRopes(void);
int EnvEnts_Rope(int i, RopeDraw *out); /* 0 if this one isn't drawn */

enum { BEAM_LASER, BEAM_PHYS, BEAM_LIGHTNING };
typedef struct {
	f32 pts[16][3];
	int n;
	f32 width;
	u32 rgba;
	int tex; /* BEAM_* */
} BeamDraw;
int EnvEnts_NumBeams(void);
void EnvEnts_Tracer(s32 owner, const f32 from[3], const f32 to[3], u32 rgba, f32 width); /* a 0.05 s tracer */
int EnvEnts_Beam(int i, BeamDraw *out);

typedef struct {
	f32 pos[3];
	f32 size;  /* units tall */
	f32 alpha; /* 0..1 */
	f32 phase;
} FireDraw;
int EnvEnts_NumFires(void);
int EnvEnts_Fire(int i, FireDraw *out);

/* tests */
int EnvEnts_RopeNodes(int i, f32 (*out)[3], int max);
int EnvEnts_FireOn(s32 ent);
int EnvEnts_SparkCount(void);

#endif
