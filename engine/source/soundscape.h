/*
 * Soundscapes (env_soundscape, env_soundscape_triggerable, env_soundscape_proxy, trigger_soundscape):
 * the level's ambient sound and its room reverb, chosen by where the player is. See soundscape.c.
 */
#ifndef HL2GC_SOUNDSCAPE_H
#define HL2GC_SOUNDSCAPE_H

#include <gctypes.h>

#include "ent.h"
#include "pack.h"

/* after Ent_Spawn */
void Scape_Init(const Pack *p);
/* every frame: the listener's ears and feet */
void Scape_Tick(f32 dt, const f32 ear[3], const f32 feet[3]);
/* an env_soundscape(_triggerable) takes over now */
void Scape_Force(s32 ent);
/* stops its sounds (level change, menu) */
void Scape_Stop(void);
s32 Scape_Current(void);

#endif
