/* hl2gc: hopper mines (combine_mine.cpp) */
#ifndef HL2GC_MINES_H
#define HL2GC_MINES_H
#include "pack.h"
void Mines_Spawn(void);
void Mines_Tick(f32 dt);
int Mines_Input(s32 ent, const char *in);
int Mines_Probe(s32 ent); /* (tests) 0 dormant 1 armed 2 warning 3 hopping 4 gone, -1 not a mine */
#endif
