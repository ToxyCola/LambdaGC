/*
 * hl2gc 2D text and HUD: font atlases built from the install's fonts (font*.bin), on-screen
 * messages (game_text, env_message / titles.txt), HUD hints (env_hudhint), and the primitives the
 * menu and captions draw with.
 */
#ifndef HL2GC_TEXT_H
#define HL2GC_TEXT_H

#include <gctypes.h>

enum { FONT_CAPTION, FONT_HUD, FONT_MENU, FONT_TITLE, FONT_ICONS, FONT_CROSS, NUM_FONTS }; /* FONT_CROSS: hl2crosshairs.ttf (quick info) */ /* FONT_ICONS: halflife2.ttf HUD icons */

int Text_LoadFont(int font, u8 *data, u32 size);
int Text_HasFont(int font);
/* every non-space character of s has a glyph in this font */
int Text_Covers(int font, const char *s);
f32 Text_Width(int font, const char *s, int n, f32 scale);
f32 Text_LineHeight(int font, f32 scale);
/* 2D drawing happens between Begin and End (End restores the 3D projection). */
void Text_Begin(u16 width, u16 height);
void Text_End(f32 proj[4][4]);
void Text_Draw(int font, f32 x, f32 y, const char *s, int n, u32 rgba, f32 scale);
void Text_Rect(f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba);
void Text_GradientH(f32 x0, f32 y0, f32 x1, f32 y1, u32 left, u32 right);
/* tex: a GXTexObj* */
void Text_Image(void *tex, f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba);
/* part of a texture (u, v in 0..1) */
void Text_ImageUV(void *tex, f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba);

/* x, y in 0..1 of the screen, -1 = centred (Source's game_text convention) */
void Hud_Message(int channel, const char *text, f32 x, f32 y, u32 rgba, f32 fadeIn, f32 hold, f32 fadeOut);
/* with titles.txt's effect: 2 writes the text out a character at a time in rgba2, settling to rgba */
/* env_credits: the game's logo, and the intro's names (credits.txt) */
void Hud_ShowLogo(const char *text, f32 fadeIn, f32 hold, f32 fadeOut);
void Hud_RollCredits(const char *names, const char *params);
void Hud_StopCredits(void);
void Hud_MessageFx(int channel, const char *text, f32 x, f32 y, u32 rgba, u32 rgba2, int effect, f32 fadeIn,
                   f32 fxTime, f32 hold, f32 fadeOut);
/* game hint strings: "%+use%" etc. become the GameCube button for that action */
void Text_SetBindName(int action, const char *name);
void Text_ConsoleKeys(const char *in, char *out, u32 n);
void Hud_Hint(const char *text, int show);
void Hud_Clear(void);
void Hud_Tick(f32 dt);
void Hud_Draw(u16 width, u16 height);

#endif
