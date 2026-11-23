/* Host stand-ins for audio.c and caption.c: count what would be heard and shown. */
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

void Audio_Init(AudioReader r) { (void)r; }
u32 Audio_PlaySpeech(const PackSound *s, const f32 *pos, f32 v, f32 rad);
void Audio_Prefetch(const PackSound *s) { (void)s; }
int Audio_Ready(const PackSound *s) { (void)s; return 1; }
u32 Audio_PlaySpeech(const PackSound *s, const f32 *pos, f32 v, f32 rad) { return Audio_Play(s, pos, v, rad); }
u32 Audio_Play(const PackSound *s, const f32 *pos, f32 v, f32 rad)
{
	(void)pos; (void)v; (void)rad;
	if (!s) return 0;
	stubSoundsPlayed++;
	{ static int lg = -1; if (lg < 0) lg = getenv("SNDLOG") != 0; if (lg) fprintf(stderr, "SND %u pos %d\n", s->offset, pos != 0); }
	if (!pos) stubEverywhere++;
	else if (stubListener) { f32 d = 0; for (int k = 0; k < 3; k++) d += (pos[k]-stubListener[k])*(pos[k]-stubListener[k]); f32 lvl = rad < 0 ? (f32)Sound_Level(s) : rad; f32 mult = powf(10.0f, (60.0f - lvl) / 20.0f) / 36.0f; if (lvl > 0 && sqrtf(d) * mult > 50.0f) stubFar++; }
	stubSoundSeconds += Audio_Duration(s);
	return next++;
}
static u32 stopped[256]; static int nstopped;
void Audio_Stop(u32 h) { if (nstopped < 256) stopped[nstopped++] = h; }
void Audio_StopAll(void) {}
/* (a sound counts as playing until stopped: soundscape loops aren't restarted every frame) */
int stubIsPlaying = 0;
int Audio_IsPlaying(u32 h) { if (!stubIsPlaying || !h) return 0; for (int i = 0; i < nstopped; i++) if (stopped[i] == h) return 0; return 1; }
int stubDsp = -1; f32 stubRoomSize, stubRoomOpen;
void Audio_SetDsp(int p) { stubDsp = p; }
void Audio_SetAutoRoom(f32 s, f32 o) { stubRoomSize = s; stubRoomOpen = o; }
void Audio_SetUnderwater(int on) { (void)on; }
void Audio_SetPos(u32 h, const f32 p[3]) { (void)h; (void)p; }
void Audio_Update(const f32 l[3], f32 y) { (void)l; (void)y; }

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
f32 Audio_GainAt(const f32 pos[3], f32 radius) { (void)pos; (void)radius; return 1.0f; }
void Audio_SetChannelVolume(u32 h, f32 v) { (void)h; (void)v; }
void Audio_SetOcclusion(AudioOccluded fn) { (void)fn; }
int stubCredits;
void Hud_ShowLogo(const char *t, f32 a, f32 b, f32 c) { (void)t; (void)a; (void)b; (void)c; stubCredits |= 1; }
void Hud_RollCredits(const char *n, const char *p) { (void)n; (void)p; stubCredits |= 2; }
void Hud_StopCredits(void) {}
