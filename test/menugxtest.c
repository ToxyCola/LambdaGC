#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include "menu.h"
#include "audio.h"
#include "text.h"
#include "weapons.h"
#include "gxv.h"
Cheats gCheats;
void Caption_SetEnabled(int on) { (void)on; }
u32 Audio_Play(const PackSound *s, const f32 *p, f32 v, f32 l) { (void)s; (void)p; (void)v; (void)l; return 1; }
void Audio_Stop(u32 h) { (void)h; }
int Audio_IsPlaying(u32 h) { (void)h; return 1; }
void Audio_SetVolume(f32 v) { (void)v; }
static u8 *slurp(const char *p, u32 *n) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, 2); *n = ftell(f); rewind(f);
	u8 *d = aligned_alloc(32, (*n + 31) & ~31u); fread(d, 1, *n, f); fclose(f); return d; }
int main(int c, char **v)
{
	u32 n; u8 *d;
	d = slurp(v[1], &n); printf("font %d\n", Text_LoadFont(FONT_MENU, d, n));
	d = slurp(v[2], &n); printf("menu %d\n", Menu_Load(d, n));
	d = slurp(v[3], &n); printf("xmenu %d\n", Menu_LoadXbox(d, n));
	f32 proj[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
	const char *map; u32 seq[] = {0,0,0,PAD_BUTTON_DOWN,PAD_BUTTON_A,0,PAD_BUTTON_A,0,PAD_BUTTON_B,0,PAD_BUTTON_B,0,PAD_BUTTON_UP,PAD_BUTTON_A,0};
	gxv.maxDist = 40000;
	Menu_Open(0);
	for (int k = 0; k < 15; k++)
		for (int i = 0; i < 40; i++) {
			Menu_Update(i ? 0 : seq[k], 0, 1 / 60.0f, &map);
			Text_Begin(640, 480); Menu_Draw(640, 480); Text_End(proj); gxv_end_frame();
		}
	Menu_ShowError(0, "Error", "Some long text to wrap over the error box so we test the wrapping code path properly.");
	for (int i = 0; i < 60; i++) { Menu_Update(0, 0, 1 / 60.0f, &map); Text_Begin(640, 480); Menu_Draw(640, 480); Text_End(proj); gxv_end_frame(); }
	printf("draws %lu vertices %lu errors %lu stray %lu overflow %lu\n", gxv.draws, gxv.vertices, gxv.errors, gxv.strayBytes, gxv.overflow);
	return gxv.errors != 0;
}
