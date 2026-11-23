/*
 * hl2gc material effects: Source's material proxies (TextureScroll, AnimatedTexture, Sine, the TV static
 * noise chains...) run once a frame from programs the converter compiled (tools/matfx.py), and what the
 * renderer needs from them per material: the frame's texture, the texture transforms, the colour.
 */
#ifndef HL2GC_FX_H
#define HL2GC_FX_H

#include "pack.h"

typedef struct {
	u8 flags;             /* FX_* */
	s16 tex, tex2;        /* base texture this frame, $texture2 (-1 none) */
	f32 color[3], alpha;  /* $color, $alpha */
	f32 xf1[2][4];        /* $basetexturetransform as a 2x4 texture matrix */
	f32 xf2[2][4];        /* $texture2transform */
} FxState;

void Fx_Init(const Pack *p);
void Fx_Shutdown(void);
/* run every material's proxies at time t (seconds) */
void Fx_Tick(f32 t);
/* the effect state of material `mat`, NULL if it has none */
const FxState *Fx_Of(const Pack *p, u32 mat);

#endif
