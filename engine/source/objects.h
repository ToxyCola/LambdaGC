/* hl2gc: headcrab canisters, thumpers, snipers, missile launchers, vehicle and APC drivers, the APC */
#ifndef HL2GC_OBJECTS_H
#define HL2GC_OBJECTS_H

#include "pack.h"

void Objects_Spawn(void);
void Objects_Tick(f32 dt);
int Objects_Input(s32 ent, const char *in, const char *param);
/* missiles in flight (weapons.c draws them) */
int Objects_NumMissiles(void);
int Objects_Missile(int i, f32 pos[3]);
int Objects_Probe(s32 ent, int *state, f32 pos[3]); /* (tests) its kind, 0 if none */

#endif
