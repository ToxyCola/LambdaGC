/* rendertest's renderer side, compiled against the GX validator headers */
#include <gccore.h>
#include <string.h>
#include "render.h"
#include "fx.h"
static float rtClock;
#include "world.h"
#include "gibs.h"
#include "gxv.h"

static World W, SW;
#include "ent.h"
#include <stdlib.h>
#include <stdio.h>
unsigned long rtSkyFaces;
unsigned long rtWorld, rtModel, rtNpc, rtFrames, rtFaces;

int rt_init(const Pack *p)
{
	if (World_Init(&W, p) != 0 || World_Init(&SW, p) != 0 || Render_Init(p) != 0)
		return -1;
	Render_SetScreen(640, 480);
	return 0;
}

static int never_occluded(const f32 a[3], const f32 b[3]) { (void)a; (void)b; return 0; }

void rt_frame(const Pack *p, const f32 eye[3], f32 pitch, f32 yaw, int flashlight)
{
	Camera cam;
	memset(&cam, 0, sizeof(cam));
	cam.origin = (guVector){eye[0], eye[1], eye[2]};
	cam.pitch = pitch;
	cam.yaw = yaw;
	cam.fovy = 59;
	cam.fovx = 75;
	Camera_Update(&cam);
	Mtx view;
	guVector target = {cam.origin.x + cam.forward.x, cam.origin.y + cam.forward.y, cam.origin.z + cam.forward.z};
	guLookAt(view, &cam.origin, &cam.up, &target);
	RenderStats st;
	World_Collect(&W, &cam, 0);
	/* the 3D skybox, as main.c sets it up */
	{
		u32 n; Entity *ents = Ent_List(&n);
		Render_SetSky(NULL, NULL, NULL, 0, 0, 0, NULL);
		for (u32 i = 1; i < n; i++) if (!strcmp(ents[i].classname, "sky_camera")) {
			const char *sc = Ent_KV((s32)i, "scale"); f32 k = sc && atof(sc) > 0 ? (f32)atof(sc) : 16;
			Camera c2 = cam;
			c2.origin = (guVector){ents[i].origin[0] + eye[0] / k, ents[i].origin[1] + eye[1] / k, ents[i].origin[2] + eye[2] / k};
			f32 so[3] = {c2.origin.x, c2.origin.y, c2.origin.z};
			if (World_PointCluster(p, so) < 0) c2.origin = (guVector){ents[i].origin[0], ents[i].origin[1], ents[i].origin[2]};
			Camera_Update(&c2);
			World_Collect(&SW, &c2, 0);
			Mtx sv; guVector t2 = {c2.origin.x + c2.forward.x, c2.origin.y + c2.forward.y, c2.origin.z + c2.forward.z};
			guLookAt(sv, &c2.origin, &c2.up, &t2);
			u8 col[3] = {128, 128, 128};
			Render_SetSky(&SW, &c2, sv, 1, 100, 2000, col);
			rtSkyFaces += SW.numVisible;
			break;
		}
	}
	Render_ModelFlashlight(flashlight, &cam);
	Fx_Tick(rtClock += 0.05f);
	Render_Frame(p, &W, &cam, view, &st);
	if (!getenv("NOADAPT")) Render_AdaptDetail(&st);
	rtWorld += st.worldVerts, rtModel += st.modelVerts, rtNpc += st.npcVerts, rtFrames++, rtFaces += st.faces;
	Gibs_Draw(view);
	Render_ModelFlashlight(0, 0);
	if (flashlight)
		Render_Flashlight(p, &W, &cam, view);
	f32 e[3] = {eye[0], eye[1], eye[2]};
	Render_Sprites(view, e, 1.0f / 60, never_occluded);
	gxv_end_frame();
}

#include "intro.h"
#include "ent.h"
int rt_intro(const Pack *p, const f32 eye[3], f32 pitch, f32 yaw)
{
	IntroView iv;
	if (!Ent_IntroView(&iv))
		return 0;
	Camera cam;
	memset(&cam, 0, sizeof(cam));
	cam.origin = (guVector){eye[0], eye[1], eye[2]};
	cam.pitch = pitch;
	cam.yaw = yaw;
	cam.fovy = 59;
	cam.fovx = 75;
	Camera_Update(&cam);
	Mtx view;
	Mtx44 proj;
	guPerspective(proj, 59, 4.0f / 3.0f, 4, 16384);
	guVector target = {cam.origin.x + cam.forward.x, cam.origin.y + cam.forward.y, cam.origin.z + cam.forward.z};
	guLookAt(view, &cam.origin, &cam.up, &target);
	RenderStats st;
	int r = Intro_Render(&iv, p, &W, &cam, view, proj, 640, 480, &st);
	gxv_end_frame();
	return r ? 1 + iv.mode[0] * 10 + iv.hasCamera * 100 : 0;
}
