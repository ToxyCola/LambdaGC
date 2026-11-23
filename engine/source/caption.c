#include "caption.h"

#include <string.h>

#include "text.h"

#define MAX_LINES 6
#define LINE_CHARS 96
#define BOX_W 560.0f

typedef struct {
	char text[LINE_CHARS];
	f32 timeLeft;
} Line;

static Line lines[MAX_LINES];
static int numLines;
static int enabled = 0; /* off by default, like PC */

int Caption_Init(u8 *fontData, u32 size) { return Text_LoadFont(FONT_CAPTION, fontData, size); }

void Caption_SetEnabled(int on)
{
	enabled = on;
	if (!on)
		numLines = 0;
}

int Caption_Enabled(void) { return enabled; }

static void push_line(const char *s, int n, f32 seconds)
{
	if (numLines == MAX_LINES) {
		memmove(&lines[0], &lines[1], sizeof(Line) * (MAX_LINES - 1));
		numLines--;
	}
	Line *l = &lines[numLines++];
	if (n >= LINE_CHARS)
		n = LINE_CHARS - 1;
	memcpy(l->text, s, (size_t)n);
	l->text[n] = 0;
	l->timeLeft = seconds;
}

void Caption_Add(const char *text, f32 seconds)
{
	if (!enabled || !text || !*text || !Text_HasFont(FONT_CAPTION))
		return;
	if (seconds < 1.5f)
		seconds = 1.5f;
	const char *p = text;
	while (*p) {
		while (*p == ' ')
			p++;
		int len = 0, lastSpace = -1;
		while (p[len] && len < LINE_CHARS - 1) {
			if (p[len] == ' ')
				lastSpace = len;
			if (Text_Width(FONT_CAPTION, p, len + 1, 1) > BOX_W - 24)
				break;
			len++;
		}
		if (p[len] && lastSpace > 0)
			len = lastSpace;
		if (len == 0)
			len = 1;
		push_line(p, len, seconds);
		p += len;
	}
}

void Caption_Clear(void) { numLines = 0; }

void Caption_Tick(f32 dt)
{
	for (int i = 0; i < numLines;) {
		lines[i].timeLeft -= dt;
		if (lines[i].timeLeft <= 0) {
			memmove(&lines[i], &lines[i + 1], sizeof(Line) * (size_t)(numLines - i - 1));
			numLines--;
		} else {
			i++;
		}
	}
}

/* call between Text_Begin and Text_End */
void Caption_Draw(u16 width, u16 height, f32 proj[4][4])
{
	(void)proj;
	if (numLines == 0)
		return;
	f32 lh = Text_LineHeight(FONT_CAPTION, 1) + 2;
	f32 boxH = lh * numLines + 12;
	f32 x0 = (width - BOX_W) * 0.5f, y1 = height - 40.0f, y0 = y1 - boxH;
	Text_Rect(x0, y0, x0 + BOX_W, y1, 0x00000090);
	for (int li = 0; li < numLines; li++) {
		const char *s = lines[li].text;
		u32 alpha = lines[li].timeLeft < 0.5f ? (u32)(lines[li].timeLeft * 2 * 255) : 255;
		f32 x = (width - Text_Width(FONT_CAPTION, s, -1, 1)) * 0.5f;
		Text_Draw(FONT_CAPTION, x, y0 + 6 + lh * li, s, -1, 0xF0F0F000u | alpha, 1);
	}
}
