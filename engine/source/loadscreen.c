#include "loadscreen.h"

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>

#include "text.h"

/* ------------------------------------------------------------------ loading images */

static GXTexObj tex[2]; /* 0 = console/loading (in-game glow), 1 = console/startup_loading (box) */
static u16 texW[2], texH[2];
static int have[2];

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

int Load_Init(u8 *data, u32 size)
{
	if (!data || size < 32 || memcmp(data, "HGLD", 4) != 0)
		return -1;
	u16 n = be16(data + 4);
	for (u16 i = 0; i < n && i < 2; i++) {
		const u8 *t = data + 8 + i * 8;
		texW[i] = be16(t);
		texH[i] = be16(t + 2);
		u32 off = be32(t + 4);
		if (off + (u32)texW[i] * texH[i] * 2 > size)
			continue;
		GX_InitTexObj(&tex[i], data + off, texW[i], texH[i], GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
		have[i] = 1;
	}
	DCFlushRange(data, size);
	return 0;
}

/* Source draws both at the bottom right of the screen */
static void corner(int i, u16 w, u16 h, f32 *x0, f32 *y0)
{
	*x0 = w - texW[i] - 32;
	*y0 = h - texH[i] - 32;
}

static void draw_image(int i, u16 w, u16 h)
{
	if (!have[i])
		return;
	f32 x0, y0;
	corner(i, w, h, &x0, &y0);
	Text_Image(&tex[i], x0, y0, x0 + texW[i], y0 + texH[i], 0xFFFFFFFF);
}

static void copy_out(void *xfb, int clear)
{
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	GX_CopyDisp(xfb, clear ? GX_TRUE : GX_FALSE);
	GX_DrawDone();
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

void Load_DrawOverlay(int kind, u16 w, u16 h)
{
	if (kind == LOAD_LEVEL) {
		draw_image(0, w, h);
	} else if (kind == LOAD_STARTUP) {
		draw_image(1, w, h);
	} else if (kind == LOAD_NEWGAME) {
		/* the startup box, with a progress bar under it (filled by Load_Progress) */
		draw_image(1, w, h);
		f32 x0, y0;
		corner(1, w, h, &x0, &y0);
		Text_Rect(x0 + 8, y0 + texH[1] + 6, x0 + texW[1] - 8, y0 + texH[1] + 12, 0x000000C0);
	}
}

static void *outXfb[2];
static int outIdx, curKind;
static u16 outW, outH;
static f32 shownFrac;
static f32 savedProj[4][4];

void Load_Begin(int kind, void *xfbA, void *xfbB, int currentlyShown, u16 w, u16 h, f32 proj[4][4])
{
	curKind = kind;
	outXfb[0] = xfbA;
	outXfb[1] = xfbB;
	outIdx = currentlyShown ^ 1;
	outW = w;
	outH = h;
	shownFrac = -1;
	memcpy(savedProj, proj, sizeof(savedProj));
}

void Load_Progress(f32 frac)
{
	if (curKind != LOAD_NEWGAME)
		return;
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	if (frac - shownFrac < 0.02f && frac < 1)
		return;
	shownFrac = frac;
	/* only the bar is drawn: the frame underneath is still in the EFB (copied without clearing) */
	Text_Begin(outW, outH);
	f32 x0, y0;
	corner(1, outW, outH, &x0, &y0);
	f32 bx0 = x0 + 8, bx1 = x0 + texW[1] - 8, by = y0 + texH[1] + 6;
	Text_Rect(bx0, by, bx1, by + 6, 0x101010FF);
	Text_Rect(bx0, by, bx0 + (bx1 - bx0) * frac, by + 6, 0xE6A01EFF);
	Text_End(savedProj);
	copy_out(outXfb[outIdx], 0);
	outIdx ^= 1;
}

/* ------------------------------------------------------------------ startup movie */

static u32 rd32(const u8 *p) { return be32(p); }

int Load_PlayMovie(LoadReader read, void *xfbA, void *xfbB, u16 w, u16 h, f32 proj[4][4])
{
	static u8 hdr[64] ATTRIBUTE_ALIGN(32);
	if (read(0, 64, hdr) != 0 || memcmp(hdr, "HGV1", 4) != 0)
		return -1;
	u16 vw = be16(hdr + 4), vh = be16(hdr + 6);
	u32 frames = rd32(hdr + 8);
	f32 fps;
	memcpy(&fps, hdr + 12, 4);
	u32 frameBytes = rd32(hdr + 16), first = rd32(hdr + 20);
	u32 audioOff = rd32(hdr + 24), audioBytes = rd32(hdr + 28), rate = rd32(hdr + 32), chans = rd32(hdr + 36);
	if (!frames || fps <= 0 || fps > 120 || !frameBytes)
		return -1;
	u8 *buf[2] = {memalign(32, frameBytes), memalign(32, frameBytes)};
	u8 *pcm = audioBytes ? memalign(32, (audioBytes + 31) & ~31u) : NULL;
	if (!buf[0] || !buf[1]) {
		free(buf[0]);
		free(buf[1]);
		free(pcm);
		return -1;
	}
	if (pcm && read(audioOff, (audioBytes + 31) & ~31u, pcm) != 0) {
		free(pcm);
		pcm = NULL;
	}
	if (pcm) {
		DCFlushRange(pcm, audioBytes);
		/* voice 0 is free: the game's sounds use 1..15 */
		ASND_SetVoice(0, chans == 2 ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT, (s32)rate, 0, pcm, (s32)audioBytes, 255, 255,
		              NULL);
	}
	u64 start = gettime();
	s32 shown = -1;
	int b = 0, skipped = 0;
	void *xfb[2] = {xfbA, xfbB};
	int xi = 0;
	for (;;) {
		f32 t = (f32)diff_usec(start, gettime()) / 1000000.0f;
		s32 f = (s32)(t * fps);
		if (f >= (s32)frames)
			break;
		PAD_ScanPads();
		if (PAD_ButtonsDown(0) & (PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_START)) {
			skipped = 1; /* Source skips the startup movie on a key press */
			break;
		}
		if (f != shown) {
			if (read(first + (u32)f * frameBytes, frameBytes, buf[b]) != 0)
				break;
			DCFlushRange(buf[b], frameBytes);
			GXTexObj obj;
			GX_InitTexObj(&obj, buf[b], vw, vh, GX_TF_CMPR, GX_CLAMP, GX_CLAMP, GX_FALSE);
			GX_InitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
			GX_InvalidateTexAll();
			Text_Begin(w, h);
			Text_Rect(0, 0, w, h, 0x000000FF);
			/* 4:3 movie, 4:3 screen: fill it */
			Text_Image(&obj, 0, 0, w, h, 0xFFFFFFFF);
			Text_End(proj);
			copy_out(xfb[xi], 1);
			xi ^= 1;
			b ^= 1;
			shown = f;
		} else {
			VIDEO_WaitVSync();
		}
	}
	if (pcm) {
		ASND_StopVoice(0);
		/* wait for the DSP to let go of the buffer before freeing it */
		VIDEO_WaitVSync();
		VIDEO_WaitVSync();
	}
	free(pcm);
	free(buf[0]);
	free(buf[1]);
	/* a black frame, so the last movie frame doesn't linger */
	Text_Begin(w, h);
	Text_Rect(0, 0, w, h, 0x000000FF);
	Text_End(proj);
	copy_out(xfb[xi], 1);
	return skipped;
}
