/* hl2gc: mounted guns (func_tank and its kinds; func_tank.cpp) */
#ifndef HL2GC_TANKS_H
#define HL2GC_TANKS_H

#include "pack.h"

void Tanks_Spawn(void);
void Tanks_Tick(f32 dt);
int Tanks_Input(s32 ent, const char *in, const char *param);
/* the player on a gun: Use inside its control volume gets on / off, the fire trigger shoots */
void Tanks_SetView(const f32 eye[3], const f32 fwd[3]);
int Tanks_PlayerUse(const f32 eye[3], const f32 fwd[3]); /* 1 if it took the use */
void Tanks_PlayerLeave(void);
int Tanks_PlayerManned(void);
void Tanks_PlayerFire(int held);
int Tanks_Probe(s32 ent, s32 *controller, f32 *aimYaw); /* (tests) 2 a model gun, 1 a brush, 0 none */

#endif
