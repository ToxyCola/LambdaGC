/*
 * hl2gc: the PC version's startup and loading screens.
 *  - the startup movie (media/valve.bik, converted to hl2gc/intro.bin), skippable with A, B or START
 *  - console/startup_loading: the "Loading..." box while the game starts and while a new game loads
 *    (here with a progress bar under it)
 *  - console/loading: the glowing "Loading..." the engine draws over the last frame on a level change
 */
#ifndef HL2GC_LOADSCREEN_H
#define HL2GC_LOADSCREEN_H

#include <gctypes.h>

enum { LOAD_STARTUP, LOAD_NEWGAME, LOAD_LEVEL };

/* loading.bin from the converter (kept in memory) */
int Load_Init(u8 *data, u32 size);
/* draws the overlay for a kind of load; call between Text_Begin and Text_End on the last frame */
void Load_DrawOverlay(int kind, u16 w, u16 h);
/* after that frame was copied out without clearing the EFB: progress updates redraw just the bar */
void Load_Begin(int kind, void *xfbA, void *xfbB, int currentlyShown, u16 w, u16 h, f32 proj[4][4]);
void Load_Progress(f32 frac);

/* reads len bytes at offset of intro.bin into buf (32-byte aligned); 0 = ok */
typedef int (*LoadReader)(u32 offset, u32 len, void *buf);
/* plays the startup movie; returns -1 if there isn't one, 1 if skipped, 0 if it played through */
int Load_PlayMovie(LoadReader read, void *xfbA, void *xfbB, u16 w, u16 h, f32 proj[4][4]);

#endif
