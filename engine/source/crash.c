#include "crash.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/audio.h>
#include <ogc/lwp.h>
#include <tuxedo/ppc/exception.h>
#include <asndlib.h>

#include "crashfont.h"

const char *volatile crashStage = "starting";
static char mapName[48] = "-";
static u32 *fbs[2];
static u16 fbW, fbH;
static volatile u32 beats;
static volatile u8 loading;
static volatile u8 shown;
static volatile u8 armed; /* from the game loop's first frame (not the startup movie or an error screen) */

/* ---- drawing straight into the YUY2 frame buffers (no GX: it may be what crashed) */
#define Y_OF(rgb) ((u8)(16 + (65 * (((rgb) >> 16) & 255) + 129 * (((rgb) >> 8) & 255) + 25 * ((rgb) & 255)) / 256))
static u32 bg = 0x10801080u; /* black */

static void put_char(int col, int row, char ch, u32 rgb)
{
	if (ch < 32 || ch > 126)
		ch = '?';
	const unsigned char *g = crashFont[ch - 32];
	u8 yf = Y_OF(rgb), yb = 16;
	s32 r = (rgb >> 16) & 255, gg = (rgb >> 8) & 255, b = rgb & 255;
	u8 cb = (u8)(128 + (-38 * r - 74 * gg + 112 * b) / 256), cr = (u8)(128 + (112 * r - 94 * gg - 18 * b) / 256);
	int x0 = 16 + col * 8, y0 = 24 + row * 16;
	if (x0 + 8 > fbW || y0 + 16 > fbH)
		return;
	for (int k = 0; k < 2; k++) {
		if (!fbs[k])
			continue;
		for (int y = 0; y < 16; y++) {
			u32 *line = fbs[k] + ((y0 + y) * fbW + x0) / 2;
			for (int p = 0; p < 4; p++) {
				int on1 = (g[y] >> (7 - p * 2)) & 1, on2 = (g[y] >> (6 - p * 2)) & 1;
				u8 cbp = (on1 || on2) ? cb : 128, crp = (on1 || on2) ? cr : 128;
				line[p] = (u32)(on1 ? yf : yb) << 24 | (u32)cbp << 16 | (u32)(on2 ? yf : yb) << 8 | crp;
			}
		}
	}
}

static int row;
static void line_out(u32 rgb, const char *s)
{
	int cols = (fbW - 32) / 8;
	for (int c = 0; s[c] && c < cols; c++)
		put_char(c, row, s[c], rgb);
	row++;
}

static void clear(void)
{
	for (int k = 0; k < 2; k++)
		if (fbs[k])
			for (u32 i = 0; i < (u32)fbW * fbH / 2; i++)
				fbs[k][i] = bg;
	row = 0;
}

static int valid(u32 a) { return a >= 0x80000000u && a < 0x81800000u && !(a & 3); }

static void report(const char *title, const char *what, u32 pc, u32 lr, u32 sp, u32 dar, int haveRegs)
{
	shown = 1;
	AUDIO_StopDMA(); /* (else the last buffer loops: the stutter) */
	clear();
	char buf[128];
	line_out(0xFFDC00, "HL2GC has stopped");
	row++;
	line_out(0xFFFFFF, title);
	line_out(0xC0C0C0, what);
	row++;
	snprintf(buf, sizeof(buf), "Map:   %s", mapName);
	line_out(0xFFFFFF, buf);
	snprintf(buf, sizeof(buf), "Stage: %s", crashStage ? crashStage : "-");
	line_out(0xFFFFFF, buf);
	if (haveRegs) {
		row++;
		snprintf(buf, sizeof(buf), "PC %08X  LR %08X  SP %08X  addr %08X", (unsigned)pc, (unsigned)lr, (unsigned)sp, (unsigned)dar);
		line_out(0xFFFFFF, buf);
		/* the callers: each frame's back chain, and the return address saved in the frame above */
		char bt[128] = "Calls:";
		int n = 0;
		u32 p = sp;
		for (int i = 0; i < 12 && valid(p); i++) {
			u32 up = *(u32 *)p;
			if (!valid(up) || up <= p)
				break;
			u32 ret = *(u32 *)(up + 4);
			if (valid(ret)) {
				char t[12];
				snprintf(t, sizeof(t), " %08X", (unsigned)ret);
				if (strlen(bt) + strlen(t) >= 70) {
					line_out(0xFFFFFF, bt);
					strcpy(bt, "      ");
				}
				strcat(bt, t);
				n++;
			}
			p = up;
		}
		if (n)
			line_out(0xFFFFFF, bt);
	}
	row++;
	line_out(0xC0C0C0, "Send a photo of this screen. The addresses can be looked up");
	line_out(0xC0C0C0, "in engine/hl2gc.map from the same build.");
	line_out(0xC0C0C0, "Turn the console off to restart.");
}

static const char *exception_name(unsigned id)
{
	switch (id) {
	case PPC_EXCPT_MCHK: return "Machine check (bad memory access on the bus)";
	case PPC_EXCPT_DSI: return "Data access error (a bad pointer was read or written)";
	case PPC_EXCPT_ISI: return "Instruction access error (jumped to a bad address)";
	case PPC_EXCPT_ALIGN: return "Alignment error (misaligned access)";
	case PPC_EXCPT_UNDEF: return "Program error (illegal instruction or trap)";
	case PPC_EXCPT_FPU: return "Floating point unavailable";
	default: return "CPU exception";
	}
}

static void panic(unsigned exid, PPCContext *ctx)
{
	u32 dar = 0;
	if (exid == PPC_EXCPT_DSI || exid == PPC_EXCPT_ALIGN)
		__asm__ volatile("mfspr %0, 19" : "=r"(dar)); /* DAR: the address it tried to use */
	char title[64];
	snprintf(title, sizeof(title), "Crash: exception %u", exid);
	report(title, exception_name(exid), ctx->pc, ctx->lr, ctx->gpr[1], dar, 1);
	for (;;)
		;
}

/* ---- the watchdog: a thread above the game's that notices the main loop has stopped */
static lwp_t dogThread = LWP_THREAD_NULL;
static u8 dogStack[8 * 1024] ATTRIBUTE_ALIGN(32);

static void *dog_main(void *arg)
{
	(void)arg;
	u32 last = beats, still = 0;
	for (;;) {
		usleep(250 * 1000);
		if (shown || !armed)
			continue;
		if (beats != last) {
			last = beats;
			still = 0;
			continue;
		}
		still++;
		u32 limit = loading ? 4 * 120 : 4 * 15; /* quarter seconds: 2 min loading, 15 s playing */
		if (still >= limit) {
			char what[96];
			snprintf(what, sizeof(what), "The game stopped responding for %u seconds (%s).", (unsigned)(still / 4),
			         loading ? "while loading" : "while playing");
			report("Hang", what, 0, 0, 0, 0, 0);
		}
	}
	return NULL;
}

void Crash_Init(void *xfb0, void *xfb1, u16 width, u16 height)
{
	fbs[0] = xfb0;
	fbs[1] = xfb1;
	fbW = width;
	fbH = height;
	PPCExcptCurPanicFn = panic;
	if (dogThread == LWP_THREAD_NULL)
		LWP_CreateThread(&dogThread, dog_main, NULL, dogStack, sizeof(dogStack), 120);
}

void Crash_Stage(const char *stage) { crashStage = stage; }

void Crash_Map(const char *map)
{
	snprintf(mapName, sizeof(mapName), "%s", map ? map : "-");
}

void Crash_Beat(void)
{
	beats++;
	armed = 1;
}

void Crash_Disarm(void) { armed = 0; }

void Crash_Loading(int on)
{
	loading = (u8)(on != 0);
	beats++;
}

void Crash_Fatal(const char *what, const char *detail)
{
	report(what, detail ? detail : "", 0, 0, 0, 0, 0);
	for (;;)
		usleep(100000);
}
