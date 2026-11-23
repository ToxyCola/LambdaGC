/* hl2gc: reading files from a GameCube disc (or Dolphin's virtual disc from an extracted folder). */
#ifndef HL2GC_DISC_H
#define HL2GC_DISC_H

#include <gctypes.h>

/* Mounts the disc and loads its file table. Returns 0 if a readable disc is present. */
int Disc_Init(void);
/* Reads a whole file (path relative to the disc's files/ folder) into a new 32-byte aligned buffer.
 * Returns -1 not found, -2 out of memory, -3 read error. */
int Disc_ReadFile(const char *path, u8 **data, u32 *size);
/* Reads part of a file into buf (32-byte aligned, room for len rounded up to 32). offset must be a
 * multiple of 4. */
int Disc_ReadRange(const char *path, u32 offset, u32 len, void *buf);
int Disc_FileSize(const char *path, u32 *size);
/* size of the biggest file directly inside dir ("hl2gc/maps"), 0 if none */
u32 Disc_LargestFile(const char *dir);

#endif
