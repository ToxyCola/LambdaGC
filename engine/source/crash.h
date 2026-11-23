#ifndef CRASH_H
#define CRASH_H
#include <gctypes.h>

/* Crash handler: a CPU exception (bad pointer, bad instruction...) or a hang (the main loop stops for
 * HANG_SECONDS, or a load takes far too long) stops the audio and draws a report straight into the
 * frame buffers: what happened, where (PC, LR, the faulting address, a backtrace to look up in
 * engine/hl2gc.map), the map, and the last stage of the frame the game reached. */
void Crash_Init(void *xfb0, void *xfb1, u16 fbWidth, u16 xfbHeight);
/* what the game is doing now (a short static string: "render world", "entities"...) */
void Crash_Stage(const char *stage);
/* the map being played (copied) */
void Crash_Map(const char *map);
/* once a frame: the game is alive */
void Crash_Beat(void);
/* stop watching for hangs (an error screen waiting for START) */
void Crash_Disarm(void);
/* during long work (loading a map) the hang limit is longer */
void Crash_Loading(int on);
/* an engine error with a message (shown like a crash; doesn't return) */
void Crash_Fatal(const char *what, const char *detail);

extern const char *volatile crashStage;
#define CRASH_STAGE(s) (crashStage = (s))
#endif
