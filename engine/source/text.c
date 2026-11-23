#include "text.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <gccore.h>

typedef struct {
	u16 x, y;
	u8 w, h;
	s8 xoff, yoff;
	u8 adv, pad;
} Glyph;

typedef struct {
	GXTexObj tex;
	const u8 *glyphs; /* 10-byte records (not naturally aligned in the file) */
	u16 n, line, w, h;
	int ok;
} Font;

static Font fonts[NUM_FONTS];

static Glyph glyph(const Font *f, u8 c)
{
	Glyph g;
	memset(&g, 0, sizeof(g));
	if (c < 32 || (u32)(c - 32) >= f->n)
		return g;
	const u8 *p = f->glyphs + (c - 32) * 10;
	g.x = (u16)(p[0] << 8 | p[1]);
	g.y = (u16)(p[2] << 8 | p[3]);
	g.w = p[4];
	g.h = p[5];
	g.xoff = (s8)p[6];
	g.yoff = (s8)p[7];
	g.adv = p[8];
	return g;
}

int Text_LoadFont(int id, u8 *data, u32 size)
{
	if (id < 0 || id >= NUM_FONTS || !data || size < 44 || memcmp(data, "HFN2", 4) != 0)
		return -1;
	Font *f = &fonts[id];
	f->w = (u16)(data[4] << 8 | data[5]);
	f->h = (u16)(data[6] << 8 | data[7]);
	f->line = (u16)(data[8] << 8 | data[9]);
	f->n = (u16)(data[10] << 8 | data[11]);
	f->glyphs = data + 12;
	u32 texOff = (12 + f->n * 10 + 31) & ~31u;
	if (texOff + (u32)f->w * f->h / 2 > size)
		return -1;
	DCFlushRange(data, size);
	GX_InitTexObj(&f->tex, data + texOff, f->w, f->h, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&f->tex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	f->ok = 1;
	return 0;
}

int Text_HasFont(int id) { return id >= 0 && id < NUM_FONTS && fonts[id].ok; }

int Text_Covers(int id, const char *s)
{
	if (!Text_HasFont(id))
		return 0;
	for (; *s; s++)
		if (*s != ' ' && !glyph(&fonts[id], (u8)*s).w)
			return 0;
	return 1;
}

static const Font *font_or_fallback(int id)
{
	if (Text_HasFont(id))
		return &fonts[id];
	for (int i = 0; i < NUM_FONTS; i++)
		if (fonts[i].ok)
			return &fonts[i];
	return NULL;
}

f32 Text_Width(int id, const char *s, int n, f32 scale)
{
	const Font *f = font_or_fallback(id);
	if (!f)
		return 0;
	f32 w = 0;
	for (int i = 0; (n < 0 || i < n) && s[i]; i++)
		w += glyph(f, (u8)s[i]).adv;
	return w * scale;
}

f32 Text_LineHeight(int id, f32 scale)
{
	const Font *f = font_or_fallback(id);
	return f ? f->line * scale : 16 * scale;
}

void Text_Begin(u16 width, u16 height)
{
	Mtx44 ortho;
	Mtx ident;
	guOrtho(ortho, 0, (f32)height, 0, (f32)width, 0, 1);
	GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
	memset(ident, 0, sizeof(ident)); /* plain C, like render.c's mtx_concat (no libogc paired-single helpers) */
	ident[0][0] = ident[1][1] = ident[2][2] = 1;
	GX_LoadPosMtxImm(ident, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetCullMode(GX_CULL_NONE);
}

void Text_End(f32 proj[4][4])
{
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
}

static void quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 s0, f32 t0, f32 s1, f32 t1, u32 rgba)
{
	GX_Position2f32(x0, y0);
	GX_Color1u32(rgba);
	GX_TexCoord2f32(s0, t0);
	GX_Position2f32(x1, y0);
	GX_Color1u32(rgba);
	GX_TexCoord2f32(s1, t0);
	GX_Position2f32(x1, y1);
	GX_Color1u32(rgba);
	GX_TexCoord2f32(s1, t1);
	GX_Position2f32(x0, y1);
	GX_Color1u32(rgba);
	GX_TexCoord2f32(s0, t1);
}

void Text_Rect(f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba)
{
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_Begin(GX_QUADS, GX_VTXFMT4, 4);
	quad(x0, y0, x1, y1, 0, 0, 0, 0, rgba);
	GX_End();
}

/* a rectangle whose colour runs from left to right (the Xbox menu's highlight and legend bars) */
void Text_GradientH(f32 x0, f32 y0, f32 x1, f32 y1, u32 left, u32 right)
{
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_Begin(GX_QUADS, GX_VTXFMT4, 4);
	GX_Position2f32(x0, y0);
	GX_Color1u32(left);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(x1, y0);
	GX_Color1u32(right);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(x1, y1);
	GX_Color1u32(right);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(x0, y1);
	GX_Color1u32(left);
	GX_TexCoord2f32(0, 0);
	GX_End();
}

void Text_Image(void *tex, f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba)
{
	GX_LoadTexObj((GXTexObj *)tex, GX_TEXMAP0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_Begin(GX_QUADS, GX_VTXFMT4, 4);
	quad(x0, y0, x1, y1, 0, 0, 1, 1, rgba);
	GX_End();
}

void Text_ImageUV(void *tex, f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba)
{
	GX_LoadTexObj((GXTexObj *)tex, GX_TEXMAP0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_Begin(GX_QUADS, GX_VTXFMT4, 4);
	quad(x0, y0, x1, y1, u0, v0, u1, v1, rgba);
	GX_End();
}

void Text_Draw(int id, f32 x, f32 y, const char *s, int n, u32 rgba, f32 scale)
{
	const Font *f = font_or_fallback(id);
	if (!f)
		return;
	if (n < 0)
		n = (int)strlen(s);
	int quads = 0;
	for (int i = 0; i < n; i++)
		if (glyph(f, (u8)s[i]).w)
			quads++;
	if (!quads)
		return;
	GX_LoadTexObj((GXTexObj *)&f->tex, GX_TEXMAP0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	f32 iw = 1.0f / f->w, ih = 1.0f / f->h;
	GX_Begin(GX_QUADS, GX_VTXFMT4, (u16)(quads * 4));
	for (int i = 0; i < n; i++) {
		Glyph g = glyph(f, (u8)s[i]);
		if (g.w) {
			f32 gx = x + g.xoff * scale, gy = y + g.yoff * scale;
			quad(gx, gy, gx + g.w * scale, gy + g.h * scale, g.x * iw, g.y * ih, (g.x + g.w) * iw, (g.y + g.h) * ih,
			     rgba);
		}
		x += g.adv * scale;
	}
	GX_End();
}

/* ------------------------------------------------------------------ HUD messages and hints */

#define CHANNELS 6
#define MSG_CHARS 256

typedef struct {
	char text[MSG_CHARS];
	f32 x, y, t, fadeIn, hold, fadeOut, fxTime;
	u32 rgba, rgba2;
	u8 active, effect;
} Message;

static Message msgs[CHANNELS];
static char hint[MSG_CHARS];
static f32 hintAlpha;
static u8 hintOn;

void Hud_MessageFx(int ch, const char *text, f32 x, f32 y, u32 rgba, u32 rgba2, int effect, f32 fadeIn, f32 fxTime,
                   f32 hold, f32 fadeOut)
{
	if (ch < 0 || ch >= CHANNELS)
		ch = 0;
	Message *m = &msgs[ch];
	Text_ConsoleKeys(text ? text : "", m->text, sizeof(m->text));
	m->x = x;
	m->y = y;
	m->rgba = rgba;
	m->rgba2 = rgba2;
	m->effect = (u8)effect;
	m->fxTime = fxTime;
	m->fadeIn = fadeIn;
	m->hold = hold > 0 ? hold : 2;
	m->fadeOut = fadeOut;
	m->t = 0;
	m->active = 1;
}

void Hud_Message(int ch, const char *text, f32 x, f32 y, u32 rgba, f32 fadeIn, f32 hold, f32 fadeOut)
{
	Hud_MessageFx(ch, text, x, y, rgba, rgba, 0, fadeIn, 0, hold, fadeOut);
}

/* how long a message is on screen: effect 2 writes it out a character every fadeIn (Source's
 * CHudMessage), then holds and fades */
static f32 msg_life(const Message *m)
{
	if (m->effect == 2)
		return m->fadeIn * (f32)strlen(m->text) + m->hold + m->fadeOut;
	return m->fadeIn + m->hold + m->fadeOut;
}

/* PC key-binding tokens in the game's hint strings ("%+use% OPEN DOOR") -> the GameCube button bound to
 * that action now ("[X] OPEN DOOR"), like the console versions show the controller button. The menu
 * tells us the layout (Text_SetBindName) whenever it changes. */
static const char *bindTok[] = {"+jump", "+speed", "+use", "+reload", "+duck", "+attack", "+attack2", "impulse 100"};
static char bindNow[8][16] = {"A", "B", "X", "Y", "Z", "R", "L", "D-pad up"};

void Text_SetBindName(int action, const char *name)
{
	if (action >= 0 && action < 8 && name)
		snprintf(bindNow[action], sizeof(bindNow[action]), "%s", name);
}

static const char *bind_for(const char *tok)
{
	for (int i = 0; i < 8; i++)
		if (!strcasecmp(tok, bindTok[i]))
			return bindNow[i];
	if (!strcasecmp(tok, "+forward")) return "Stick up";
	if (!strcasecmp(tok, "+back")) return "Stick down";
	if (!strcasecmp(tok, "+moveleft")) return "Stick left";
	if (!strcasecmp(tok, "+moveright")) return "Stick right";
	if (!strcasecmp(tok, "xmove")) return "Control stick";
	if (!strcasecmp(tok, "xlook")) return "C-stick";
	if (!strcasecmp(tok, "+walk") || !strcasecmp(tok, "+zoom")) return NULL; /* (no button for these) */
	if (!strcasecmp(tok, "invnext")) return "D-pad right";
	if (!strcasecmp(tok, "invprev")) return "D-pad left";
	if (!strcasecmp(tok, "lastinv")) return "D-pad down";
	return NULL;
}

void Text_ConsoleKeys(const char *in, char *out, u32 n)
{
	u32 o = 0;
	for (const char *p = in; *p && o + 1 < n;) {
		const char *e = p[0] == '%' ? strchr(p + 1, '%') : NULL;
		if (e && e - p > 1 && e - p < 24) {
			char tok[24];
			memcpy(tok, p + 1, (size_t)(e - p - 1));
			tok[e - p - 1] = 0;
			const char *b = bind_for(tok);
			if (b) {
				int w = snprintf(out + o, n - o, "[%s]", b);
				o += w > 0 ? (u32)w : 0;
				if (o >= n)
					o = n - 1;
			}
			p = e + 1;
			if (!b)
				while (*p == ' ')
					p++;
			continue;
		}
		out[o++] = *p++;
	}
	out[o] = 0;
}

void Hud_Hint(const char *text, int show)
{
	if (text)
		Text_ConsoleKeys(text, hint, sizeof(hint));
	hintOn = (u8)show;
}

void Hud_Clear(void)
{
	memset(msgs, 0, sizeof(msgs));
	hintOn = 0;
	hintAlpha = 0;
	Hud_StopCredits();
}

/* ---- env_credits (CHudCredits): the HALF-LIFE 2 logo, and the intro's names during the train ride */
static struct {
	char logo[32];
	f32 t, fi, hold, fo;
	u8 on;
} logo;
#define MAX_CREDIT_NAMES 96
static struct {
	char names[MAX_CREDIT_NAMES][24];
	int n;
	f32 t, fi, fo, hold, next, pause, x, y;
	u8 on;
} roll;

void Hud_ShowLogo(const char *text, f32 fadeIn, f32 hold, f32 fadeOut)
{
	snprintf(logo.logo, sizeof(logo.logo), "%s", text ? text : "");
	logo.t = 0;
	logo.fi = fadeIn;
	logo.hold = hold;
	logo.fo = fadeOut;
	logo.on = 1;
}

void Hud_RollCredits(const char *names, const char *params)
{
	roll.n = 0;
	for (const char *p = names; p && *p && roll.n < MAX_CREDIT_NAMES;) {
		const char *e = strchr(p, '\n');
		int len = e ? (int)(e - p) : (int)strlen(p);
		snprintf(roll.names[roll.n++], sizeof(roll.names[0]), "%.*s", len, p);
		p = e ? e + 1 : p + len;
	}
	f32 lt;
	roll.fi = 2.5f, roll.fo = 1, roll.hold = 1, roll.next = 1, roll.pause = 1, roll.x = 96, roll.y = 360;
	if (params)
		sscanf(params, "%f %f %f %f %f %f %f %f", &roll.fi, &roll.fo, &roll.hold, &roll.next, &roll.pause, &lt,
		       &roll.x, &roll.y);
	roll.t = 0;
	roll.on = roll.n > 0;
}

void Hud_StopCredits(void)
{
	logo.on = roll.on = 0;
}

/* names come in waves of three down the screen, a "nextfadetime" apart, a pause between waves */
#define CREDIT_WAVE 3
static f32 name_start(int i)
{
	return i * roll.next + (f32)(i / CREDIT_WAVE) * (roll.pause + roll.fi + roll.hold + roll.fo - CREDIT_WAVE * roll.next);
}

static void credits_tick(f32 dt)
{
	if (logo.on) {
		logo.t += dt;
		if (logo.t > logo.fi + logo.hold + logo.fo)
			logo.on = 0;
	}
	if (roll.on) {
		roll.t += dt;
		if (roll.t > name_start(roll.n - 1) + roll.fi + roll.hold + roll.fo)
			roll.on = 0;
	}
}

static f32 fade3(f32 t, f32 fi, f32 hold, f32 fo)
{
	if (t < 0)
		return 0;
	if (t < fi)
		return fi > 0 ? t / fi : 1;
	t -= fi;
	if (t < hold)
		return 1;
	t -= hold;
	return t < fo && fo > 0 ? 1 - t / fo : 0;
}

static void credits_draw(u16 sw, u16 sh)
{
	if (logo.on) {
		f32 a = fade3(logo.t, logo.fi, logo.hold, logo.fo);
		/* CHudCredits draws it in ClientTitleFont, 32 units tall on a 480-line screen (clientscheme.res);
		 * our title font is built bigger, so scale it down to that, and never wider than the screen */
		f32 lh1 = Text_LineHeight(FONT_TITLE, 1);
		f32 sc = lh1 > 0 ? 32.0f * sh / 480.0f / lh1 : 1.0f;
		f32 w1 = Text_Width(FONT_TITLE, logo.logo, (int)strlen(logo.logo), 1);
		if (w1 * sc > sw * 0.9f && w1 > 0)
			sc = sw * 0.9f / w1;
		f32 w = w1 * sc, lh = lh1 * sc;
		Text_Draw(FONT_TITLE, (sw - w) * 0.5f, (sh - lh) * 0.5f, logo.logo, (int)strlen(logo.logo),
		          0xFFFFFF00u | (u32)(a * 200), sc);
	}
	if (roll.on) {
		f32 lh = Text_LineHeight(FONT_HUD, 1) * 1.2f;
		for (int i = 0; i < roll.n; i++) {
			f32 a = fade3(roll.t - name_start(i), roll.fi, roll.hold, roll.fo);
			if (a <= 0)
				continue;
			f32 x = roll.x / 640.0f * sw, y = roll.y / 480.0f * sh + (i % CREDIT_WAVE) * lh;
			const char *n = roll.names[i];
			Text_Draw(FONT_HUD, x + 1, y + 1, n, (int)strlen(n), (u32)(a * 140), 1);
			Text_Draw(FONT_HUD, x, y, n, (int)strlen(n), 0xFFFFFF00u | (u32)(a * 180), 1);
		}
	}
}

void Hud_Tick(f32 dt)
{
	credits_tick(dt);
	for (int i = 0; i < CHANNELS; i++)
		if (msgs[i].active) {
			msgs[i].t += dt;
			if (msgs[i].t > msg_life(&msgs[i]))
				msgs[i].active = 0;
		}
	hintAlpha += (hintOn ? dt : -dt) * 3;
	if (hintAlpha < 0)
		hintAlpha = 0;
	if (hintAlpha > 1)
		hintAlpha = 1;
}

/* multi-line text, lines split on '\n' */
static void draw_block(int font, const char *s, f32 x, f32 y, u16 sw, u16 sh, u32 rgba)
{
	int lines = 1;
	for (const char *p = s; *p; p++)
		if (*p == '\n')
			lines++;
	f32 lh = Text_LineHeight(font, 1);
	f32 top = y < 0 ? (sh - lh * lines) * 0.5f : y * sh;
	const char *p = s;
	for (int l = 0; l < lines; l++) {
		const char *e = strchr(p, '\n');
		int n = e ? (int)(e - p) : (int)strlen(p);
		f32 w = Text_Width(font, p, n, 1);
		f32 left = x < 0 ? (sw - w) * 0.5f : x * sw;
		/* a soft shadow keeps text readable over bright scenes */
		Text_Draw(font, left + 2, top + l * lh + 2, p, n, rgba & 0x000000FF ? (rgba & 0xFF) * 3 / 4 : 0, 1);
		Text_Draw(font, left, top + l * lh, p, n, rgba, 1);
		p = e ? e + 1 : p + n;
	}
}

static u32 lerp_rgb(u32 a, u32 b, f32 k)
{
	u32 o = 0;
	for (int sh = 8; sh < 32; sh += 8) {
		f32 x = ((a >> sh) & 255) * (1 - k) + ((b >> sh) & 255) * k;
		o |= (u32)(x < 0 ? 0 : x > 255 ? 255 : x) << sh;
	}
	return o;
}

/* effect 2 ("write out", the chapter titles): each character appears fadeIn after the one before in
 * the highlight colour and turns to the final colour over fxTime; the whole message fades at the end */
static void draw_writeout(const Message *m, u16 sw, u16 sh)
{
	const char *s = m->text;
	int len = (int)strlen(s), lines = 1;
	for (const char *p = s; *p; p++)
		if (*p == '\n')
			lines++;
	f32 fadeAt = m->fadeIn * len + m->hold, a = 1;
	if (m->t > fadeAt && m->fadeOut > 0)
		a = 1 - (m->t - fadeAt) / m->fadeOut;
	if (a <= 0)
		return;
	f32 lh = Text_LineHeight(FONT_HUD, 1);
	f32 top = m->y < 0 ? (sh - lh * lines) * 0.5f : m->y * sh;
	const char *p = s;
	int idx = 0;
	for (int l = 0; l < lines; l++) {
		const char *e = strchr(p, '\n');
		int n = e ? (int)(e - p) : (int)strlen(p);
		f32 x = m->x < 0 ? (sw - Text_Width(FONT_HUD, p, n, 1)) * 0.5f : m->x * sw;
		for (int c = 0; c < n; c++, idx++) {
			f32 since = m->t - m->fadeIn * (f32)(idx + 1);
			f32 w = Text_Width(FONT_HUD, p + c, 1, 1);
			if (since >= 0) {
				f32 k = m->fxTime > 0 && since < m->fxTime ? since / m->fxTime : 1;
				u32 col = lerp_rgb(m->rgba2, m->rgba, k) | (u32)(a * 255);
				Text_Draw(FONT_HUD, x + 2, top + l * lh + 2, p + c, 1, (u32)(a * 160), 1);
				Text_Draw(FONT_HUD, x, top + l * lh, p + c, 1, col, 1);
			}
			x += w;
		}
		idx++; /* the newline takes its turn too */
		p = e ? e + 1 : p + n;
	}
}

void Hud_Draw(u16 sw, u16 sh)
{
	credits_draw(sw, sh);
	for (int i = 0; i < CHANNELS; i++) {
		Message *m = &msgs[i];
		if (!m->active)
			continue;
		if (m->effect == 2) {
			draw_writeout(m, sw, sh);
			continue;
		}
		f32 a = 1;
		if (m->t < m->fadeIn && m->fadeIn > 0)
			a = m->t / m->fadeIn;
		else if (m->t > m->fadeIn + m->hold && m->fadeOut > 0)
			a = 1 - (m->t - m->fadeIn - m->hold) / m->fadeOut;
		if (a < 0)
			a = 0;
		u32 rgba = (m->rgba & 0xFFFFFF00u) | (u32)(a * 255);
		draw_block(FONT_HUD, m->text, m->x, m->y, sw, sh, rgba);
	}
	if (hintAlpha > 0 && hint[0]) {
		f32 w = Text_Width(FONT_HUD, hint, -1, 0.8f) + 32, h = Text_LineHeight(FONT_HUD, 0.8f) + 16;
		f32 x0 = (sw - w) * 0.5f, y0 = sh * 0.72f;
		Text_Rect(x0, y0, x0 + w, y0 + h, (u32)(hintAlpha * 120));
		Text_Draw(FONT_HUD, x0 + 16, y0 + 8, hint, -1, 0xFFD040u << 8 | (u32)(hintAlpha * 255), 0.8f);
	}
}
