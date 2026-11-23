#include "disc.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ogc/cache.h>
#include <ogc/dvd.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <unistd.h>

#define DISC_MAGIC 0xC2339F3D
#define READ_CHUNK (512 * 1024)

static dvdcmdblk blk __attribute__((aligned(32)));
static u8 header[0x440] __attribute__((aligned(32)));
static u8 *fst;
static u32 fstEntries;
static const char *fstNames;

static void dvd_cb(s32 result, dvdcmdblk *block)
{
	(void)result;
	(void)block;
}

/* Wait for an async drive command, giving up after timeoutMs instead of hanging forever. */
static int wait_block(u32 timeoutMs)
{
	u64 start = gettime();
	for (;;) {
		s32 st = DVD_GetCmdBlockStatus(&blk);
		if (st == 0)
			return 0;
		if (st == -1 || st == 10)
			return -1;
		if (diff_msec(start, gettime()) > timeoutMs)
			return -2;
		usleep(1000);
	}
}

/* one drive command at a time: the sound loader thread reads lines while the game reads other files */
static mutex_t driveLock = LWP_MUTEX_NULL;

static int read_disc(void *buf, u32 len, u32 offset)
{
	/* lengths must be multiples of 32; callers allocate accordingly */
	len = (len + 31) & ~31u;
	DCInvalidateRange(buf, len);
	if (driveLock == LWP_MUTEX_NULL)
		LWP_MutexInit(&driveLock, false);
	LWP_MutexLock(driveLock);
	int r = -1;
	if (DVD_ReadAbsAsyncPrio(&blk, buf, len, offset, dvd_cb, 2))
		r = wait_block(5000);
	LWP_MutexUnlock(driveLock);
	return r;
}

static int header_ok(void)
{
	int r = read_disc(header, sizeof(header), 0);
	if (r != 0) {
		printf("  read failed (%d), block state %d, drive status %d\n", r, (int)DVD_GetCmdBlockStatus(&blk),
		       (int)DVD_GetDriveStatus());
		return 0;
	}
	if (*(u32 *)(header + 0x1C) != DISC_MAGIC) {
		printf("  read ok but not a GameCube disc (id %.6s)\n", header);
		return 0;
	}
	return 1;
}

int Disc_Init(void)
{
	/* the boot ROM / Dolphin copies the disc ID to 0x80000000 when it boots a disc */
	const char *bootId = (const char *)0x80000000;

	printf("boot id: %c%c%c%c%c%c\n", bootId[0] ? bootId[0] : '-', bootId[1] ? bootId[1] : '-',
	       bootId[2] ? bootId[2] : '-', bootId[3] ? bootId[3] : '-', bootId[4] ? bootId[4] : '-',
	       bootId[5] ? bootId[5] : '-');
	DVD_Init();
	/* Booted from a disc, the drive is already running: read it directly. Only if that
	 * fails, try a full mount (drive reset + disc ID), with a timeout. */
	printf("disc: reading header...\n");
	if (!header_ok()) {
		printf("disc: mounting...\n");
		int m = DVD_MountAsync(&blk, dvd_cb) ? wait_block(8000) : -9;
		if (m != 0)
			printf("  mount result %d, drive status %d\n", m, (int)DVD_GetDriveStatus());
		if (m != 0 || !header_ok()) {
			printf("disc: none\n");
			return -1;
		}
	}
	u32 fstOff = *(u32 *)(header + 0x424);
	u32 fstSize = *(u32 *)(header + 0x428);
	if (!fstSize)
		return -1;
	fst = memalign(32, (fstSize + 31) & ~31u);
	if (!fst || read_disc(fst, fstSize, fstOff) != 0) {
		printf("disc: file table read failed\n");
		return -1;
	}
	fstEntries = *(u32 *)(fst + 8); /* root entry's "next" = total entries */
	fstNames = (const char *)(fst + fstEntries * 12);
	printf("disc %.6s: %u files\n", header, (unsigned)fstEntries);
	return 0;
}

/* FST entry: [u8 isDir][u24 nameOffset][u32 fileOffset | parent][u32 fileLength | nextIndex] */
static int find(const char *path, u32 *offset, u32 *length)
{
	u32 dirEnd = fstEntries, i = 1;
	while (*path) {
		const char *slash = strchr(path, '/');
		size_t n = slash ? (size_t)(slash - path) : strlen(path);
		int found = 0;
		while (i < dirEnd) {
			const u8 *e = fst + i * 12;
			const char *name = fstNames + ((e[1] << 16) | (e[2] << 8) | e[3]);
			u32 w1 = *(const u32 *)(e + 4), w2 = *(const u32 *)(e + 8);
			if (strlen(name) == n && strncasecmp(name, path, n) == 0) {
				if (slash) {
					if (!e[0])
						return -1;
					dirEnd = w2; /* search inside this directory */
					i++;
				} else {
					if (e[0])
						return -1;
					*offset = w1;
					*length = w2;
				}
				found = 1;
				break;
			}
			i = e[0] ? w2 : i + 1; /* skip whole subdirectories */
		}
		if (!found)
			return -1;
		path += n + (slash ? 1 : 0);
		if (!slash)
			return 0;
	}
	return -1;
}

int Disc_ReadFile(const char *path, u8 **data, u32 *size)
{
	u32 off, len;
	if (!fst || find(path, &off, &len) != 0)
		return -1;
	u8 *buf = memalign(32, (len + 31) & ~31u);
	if (!buf)
		return -2;
	for (u32 pos = 0; pos < len; pos += READ_CHUNK) {
		u32 n = len - pos < READ_CHUNK ? len - pos : READ_CHUNK;
		if (read_disc(buf + pos, n, off + pos) != 0) {
			free(buf);
			return -3;
		}
	}
	*data = buf;
	*size = len;
	return 0;
}

int Disc_ReadRange(const char *path, u32 offset, u32 len, void *buf)
{
	/* (looked up every time: the sound loader thread and the game may read different files at once) */
	u32 cachedOff, cachedLen;
	if (!fst)
		return -1;
	if (find(path, &cachedOff, &cachedLen) != 0)
		return -1;
	if (offset + len > cachedLen + 31)
		return -2;
	u8 *p = buf;
	for (u32 pos = 0; pos < len; pos += READ_CHUNK) {
		u32 n = len - pos < READ_CHUNK ? len - pos : READ_CHUNK;
		if (read_disc(p + pos, n, cachedOff + offset + pos) != 0)
			return -3;
	}
	return 0;
}

int Disc_FileSize(const char *path, u32 *size)
{
	u32 off;
	if (!fst || find(path, &off, size) != 0)
		return -1;
	return 0;
}

/* the biggest file directly inside a directory (e.g. "hl2gc/maps"), to size the map arena */
u32 Disc_LargestFile(const char *dir)
{
	if (!fst)
		return 0;
	u32 dirEnd = fstEntries, i = 1;
	const char *path = dir;
	while (*path) {
		const char *slash = strchr(path, '/');
		size_t n = slash ? (size_t)(slash - path) : strlen(path);
		int found = 0;
		while (i < dirEnd) {
			const u8 *e = fst + i * 12;
			const char *name = fstNames + ((e[1] << 16) | (e[2] << 8) | e[3]);
			if (e[0] && strlen(name) == n && strncasecmp(name, path, n) == 0) {
				dirEnd = *(const u32 *)(e + 8);
				i++;
				found = 1;
				break;
			}
			i = e[0] ? *(const u32 *)(e + 8) : i + 1;
		}
		if (!found)
			return 0;
		path += n + (slash ? 1 : 0);
	}
	u32 best = 0;
	while (i < dirEnd) {
		const u8 *e = fst + i * 12;
		if (e[0]) {
			i = *(const u32 *)(e + 8);
			continue;
		}
		u32 len = *(const u32 *)(e + 8);
		if (len > best)
			best = len;
		i++;
	}
	return best;
}
