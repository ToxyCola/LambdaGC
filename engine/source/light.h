/*
 * hl2gc model lighting, the way Source lights models: the ambient cube of where a model is (baked per BSP
 * leaf / per static prop by vrad) plus the direct light of the map's worldlights that can see that point,
 * folded into the same six directions. Cubes are 6 faces x RGB (+X -X +Y -Y +Z -Z), 8-bit gamma at half
 * intensity like the lightmaps (the TEV doubles them).
 */
#ifndef HL2GC_LIGHT_H
#define HL2GC_LIGHT_H

#include "pack.h"

/* a map was loaded: lights the static props (traces; once) */
void Light_Init(const Pack *p);
void Light_Shutdown(void);
/* a static prop's cube with its direct light (NULL: none, use its tint) */
const u8 *Light_StaticCube(const Pack *p, u32 prop);
/* add the direct light of the worldlights visible from pos into cube */
void Light_AddDirect(const Pack *p, const f32 pos[3], u8 cube[18]);
/* a moving thing's cube at a point: leaf cube + direct light, re-lit when it moves or now and then;
 * at most a few re-lights a frame (Light_Frame resets the budget). 0: no leaf data here */
int Light_EntityCube(const Pack *p, s32 ent, const f32 pos[3], u8 out[18]);
void Light_Frame(void);

/* light styles (switchable and flickering lights): brightness now (1 = normal; patterns reach ~2) */
#define LIGHT_STYLES 64
f32 Light_StyleValue(int style);
void Light_StyleTick(f32 dt);
/* a named light's inputs: on 1, off 0, toggle -1 */
void Light_StyleSwitch(int style, int on);
void Light_StylePattern(int style, const char *pattern);

/* light_dynamic (CDynamicLight): moving lights for models (the G-Man's intro): registered at spawn,
 * switched and changed by its inputs; added to every model's cube each frame (no traces) */
void Light_DynamicSpawn(s32 ent);
void Light_DynamicInput(s32 ent, const char *in, const char *param);
/* add the dynamic lights reaching pos to cube; returns whether any did */
int Light_AddDynamic(const f32 pos[3], u8 cube[18]);

#endif
