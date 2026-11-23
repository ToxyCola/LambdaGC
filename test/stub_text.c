/* Host stand-ins for caption.c and text.c (the real audio.c is linked: soundsim): count what would be heard and shown. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "audio.h"
#include "caption.h"

int stubSoundsPlayed, stubCaptions, stubEverywhere, stubFar;
extern const f32 *stubListener;
const f32 *stubListener;
f32 stubSoundSeconds;
static u32 next = 1;

/* (a sound counts as playing until stopped: soundscape loops aren't restarted every frame) */

int Caption_Init(u8 *d, u32 s) { (void)d; (void)s; return 0; }
void Caption_Add(const char *t, f32 s) { (void)s; if (t && *t) stubCaptions++; }
void Caption_Clear(void) {}
void Caption_Tick(f32 dt) { (void)dt; }
void Caption_Draw(u16 w, u16 h, f32 p[4][4]) { (void)w; (void)h; (void)p; }
void Caption_SetEnabled(int on) { (void)on; }
int Caption_Enabled(void) { return 1; }

/* text.c stand-ins (HUD messages, hints) */
#include "text.h"
int stubHudMessages, stubHints;
void Hud_Message(int ch, const char *t, f32 x, f32 y, u32 c, f32 a, f32 b, f32 d) { (void)ch; (void)x; (void)y; (void)c; (void)a; (void)b; (void)d; if (t && *t) stubHudMessages++; }
void Hud_MessageFx(int ch, const char *t, f32 x, f32 y, u32 c, u32 c2, int fx, f32 a, f32 k, f32 b, f32 d) { (void)c2; (void)fx; (void)k; Hud_Message(ch, t, x, y, c, a, b, d); }
void Hud_Hint(const char *t, int s) { (void)t; if (s) stubHints++; }
void Hud_Clear(void) {}
int stubCredits;
void Hud_ShowLogo(const char *t, f32 a, f32 b, f32 c) { (void)t; (void)a; (void)b; (void)c; stubCredits |= 1; }
void Hud_RollCredits(const char *n, const char *p) { (void)n; (void)p; stubCredits |= 2; }
void Hud_StopCredits(void) {}
