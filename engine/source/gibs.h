/*
 * hl2gc break pieces: what crates, bottles, glass and other breakables fall apart into
 * (Source's breakable_model / func_breakable gibs). Each piece is a small rigid body that tumbles,
 * bounces, settles and fades out after its fade time, like on PC.
 */
#ifndef HL2GC_GIBS_H
#define HL2GC_GIBS_H

#include "pack.h"

void Gibs_Init(const Pack *p);
void Gibs_Clear(void);
/* a prop with this model broke at centre, pushed with velocity (may be NULL) */
void Gibs_BreakModel(s32 model, const f32 centre[3], const f32 R[3][3], const f32 vel[3], const u8 tint[4]);
/* a func_breakable / window of this material (-1 wood, -2 glass, -3 metal, -4 concrete) and box broke */
void Gibs_BreakBox(int material, const f32 mins[3], const f32 maxs[3], const f32 vel[3], const u8 tint[4]);
/* env_shooter: one model thrown from at with vel, fading after life seconds */
void Gibs_Throw(s32 model, const f32 at[3], const f32 R[3][3], const f32 vel[3], f32 life, const u8 tint[4]);
void Gibs_Tick(f32 dt);
#ifndef HOST_TEST
#include <gccore.h>
void Gibs_Draw(Mtx view);
#endif
int Gibs_Count(void);
int Gibs_Probe(int i, f32 pos[3], int *resting, f32 *life);

#endif
