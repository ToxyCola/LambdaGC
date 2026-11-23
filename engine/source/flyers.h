/* hl2gc: manhacks and scanners (npc_manhack.cpp, npc_scanner.cpp) */
#ifndef HL2GC_FLYERS_H
#define HL2GC_FLYERS_H

#include "pack.h"

void Flyers_Spawn(void);
void Flyers_Free(void);
void Flyers_Tick(f32 dt);
int Flyers_Input(s32 ent, const char *in, const char *param);
int Flyers_Count(void);
int Flyers_Glow(int i, f32 pos[3], u32 *rgba, f32 *size);
int Flyers_Probe(s32 ent, f32 vel[3]); /* (tests) its kind, 0 if not one */

#endif
