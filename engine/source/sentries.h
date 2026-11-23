/* hl2gc: Combine cameras, ceiling turrets and ground turrets (npc_combinecamera.cpp, npc_turret_ceiling.cpp,
 * npc_turret_ground.cpp) */
#ifndef HL2GC_SENTRIES_H
#define HL2GC_SENTRIES_H

#include "pack.h"

void Sentries_Spawn(void); /* after the NPCs */
void Sentries_Tick(f32 dt);
int Sentries_Input(s32 ent, const char *in);
/* their eye glows (weapons.c draws them) */
int Sentries_Count(void);
int Sentries_Glow(int i, f32 pos[3], u32 *rgba, f32 *size);
/* tests */
int Sentries_Probe(s32 ent, f32 *aimYaw, f32 *aimPitch, s32 *target, int *state);

#endif
