#ifndef HL2GC_RENDER_H
#define HL2GC_RENDER_H

#include <gccore.h>
#include "pack.h"
#include "world.h"

typedef struct {
	u32 faces, verts, materials;
	u32 worldVerts, modelVerts, npcVerts; /* where the vertices went */
} RenderStats;
/* after the main view's frame: steer the level of detail by how many vertices it took */
void Render_AdaptDetail(const RenderStats *st);
f32 Render_Detail(void);

int Render_Init(const Pack *p);
void Render_Free(void);
void Render_Frame(const Pack *p, const World *w, const Camera *cam, Mtx view, RenderStats *st);
/* Monitor screens: draw a frame between Begin and End to update the _rt_Camera texture. */
void Render_CameraBegin(void);
void Render_CameraEnd(u16 fbWidth, u16 efbHeight);
void Render_CameraOff(void);
/* skinned models from another pack (weapons.hgm): textures set up once, drawn with any pose */
int Render_InitSkinPack(const Pack *wp);
int Render_SkinTexture(s32 idx);
/* frame buffer size (for the water's refraction copy) */
void Render_SetScreen(u16 w, u16 h);
void Render_Model(const Pack *p, u32 mdl, const f32 origin[3], const f32 R[3][3], const u8 tint[4], Mtx view);
/* the HEV flashlight: lights the world in front of the camera (after Render_Frame) */
void Render_Flashlight(const Pack *p, const World *w, const Camera *cam, Mtx view);
void Render_SkinnedModel(const Pack *wp, u32 model, f32 (*pose)[3][4], u32 numBones, const f32 origin[3],
                         const f32 R[3][3], const u8 tint[4], Mtx view);

/* env_sprite / env_lightglow, after the world and translucent surfaces */
void Render_Sprites(Mtx view, const f32 eye[3], f32 dt, int (*occluded)(const f32 a[3], const f32 b[3]));

/* the ambient cube (6 x RGB, +X -X +Y -Y +Z -Z, the models' tint scale) of the map at pos; 0 if none */
/* light models with the player's flashlight this frame (call before Render_Frame; 0 turns it off) */
void Render_ModelFlashlight(int on, const Camera *cam);
int Render_CubeAt(const Pack *p, const f32 pos[3], u8 out[18]);

/* the 3D skybox for this frame: its visibility from the sky camera, that camera and view; NULL for none */
void Render_SetSky(const World *w, const Camera *c, Mtx v, int fog, f32 fogStart, f32 fogEnd, const u8 col[3]);

#endif
