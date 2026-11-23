/*
 * hl2gc: the G-Man intro's picture (Source's ViewDrawScene_Intro). The script_intro's camera entity
 * and the normal view are both rendered, then combined with the intro's blend passes.
 */
#ifndef HL2GC_INTRO_H
#define HL2GC_INTRO_H

#include <gccore.h>

#include "ent.h"
#include "pack.h"
#include "render.h"
#include "world.h"

/* Renders the whole 3D picture for this frame; returns 0 if it couldn't (draw normally instead). */
int Intro_Render(const IntroView *iv, const Pack *pack, World *world, const Camera *cam, Mtx view, f32 proj[4][4],
                 u16 fbW, u16 efbH, RenderStats *st);

/* allocates the blend buffers up front (at boot), so they never split the map memory */
void Intro_Init(u16 fbW, u16 efbH);

#endif
