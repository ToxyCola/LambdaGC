/* hl2gc closed captions: subtitle lines for dialogue, drawn over the 3D view. */
#ifndef HL2GC_CAPTION_H
#define HL2GC_CAPTION_H

#include <gctypes.h>

/* fontData: hl2gc/font.bin (32-byte aligned, kept by the caller). Returns 0 on success. */
int Caption_Init(u8 *fontData, u32 size);
/* Shows a line (Latin-1) for the given number of seconds. */
void Caption_Add(const char *text, f32 seconds);
void Caption_Clear(void);
void Caption_SetEnabled(int on);
int Caption_Enabled(void);
void Caption_Tick(f32 dt);
/* Draws the captions (inside Text_Begin/Text_End). */
void Caption_Draw(u16 width, u16 height, f32 proj[4][4]);

#endif
