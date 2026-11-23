/*
 * hl2gc facial animation, Source style: flex controllers (set by a scene's flex tracks, lip sync from
 * the line's phonemes, and blinking) run through the model's flex rules into morph-target weights,
 * and the face vertices are morphed on the CPU for the few faces near the camera.
 */
#ifndef HL2GC_FACE_H
#define HL2GC_FACE_H

#include "npc.h"
#include "pack.h"

int Face_Init(const Pack *p);
void Face_Free(void);
/* a scene's flex track (controller name, value already in the controller's range) */
void Face_SetController(Npc *n, const char *name, f32 value);
/* per frame, before drawing NPCs */
void Face_BeginFrame(f32 now);
/* Morphed copy of this NPC's model vertices (indexed like npcVerts from the model's first vertex),
 * or NULL to draw the plain mesh. *base = NVTX index of element 0. */
const PackNpcVert *Face_Morph(const Npc *n, f32 distance, u32 *base);

#endif
