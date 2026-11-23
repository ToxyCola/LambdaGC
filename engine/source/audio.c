#include "audio.h"

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <gccore.h>
#include <asndlib.h>
#include <ogc/cache.h>
#include <ogc/irq.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <unistd.h>

/* (tests) sounds that didn't get heard, by why: [0] no channel, [1] no cache room / read failed, [2] a
 * playing sound pushed out for a new one, [3] of those, a loop parked */
static u32 audioDrops[4];
#define NUM_CHANNELS 40          /* sounds at once: mixed in software into one stereo ASND voice (the PC mixes
                                  * dozens; one hardware voice each capped this at 15 and dropped the rest) */
#define MIX_VOICE 1
#define CACHE_SLOTS 64
#define CACHE_BYTES (1536 * 1024)
#define CHUNK 512                /* samples per mixed buffer: ~23 ms (a new sound is heard within two) */
#define NUM_BUFS 3
#define HEADER 12
#define STREAM_MIN (160 * 1024)  /* longer sounds stream from the disc */
#define BLOCK (16 * 1024)
#define RING (4 * BLOCK)
#define MAX_STREAMS 3
#define MIN_GAIN 0.008f
#define OCCLUDED_GAIN 0.6f       /* Source muffles sounds behind walls by a few dB */

static const s16 stepTable[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
	107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
	5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
	27086, 29794, 32767};
static const s8 indexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* recently played sounds stay in memory: scripts often repeat the same ambient sound */
typedef struct {
	u32 offset, size;
	u8 *data;
	u16 refs;
	u32 lastUse;
	volatile u8 loading;  /* 1: the loader thread is reading it, 2: the read failed */
} Blob;

static Blob cache[CACHE_SLOTS];
static u32 cacheBytes, useClock;

typedef struct {
	u32 handle;          /* 0 = free */
	s16 blob;            /* cache slot, -1 when streaming */
	const u8 *codes;     /* nibbles (cached sounds) */
	u8 *ring;            /* streaming: RING bytes of nibbles */
	u32 bankOffset;      /* streaming: where the nibbles start in vo.bin */
	u32 totalBytes;      /* bytes of nibbles */
	volatile u32 loadedTo; /* streaming: nibble bytes [.., loadedTo) are in the ring */
	volatile u8 restart; /* streaming loop reached the end: the main loop rewinds the stream */
	u32 samples, cursor;
	s32 pred, index;
	u8 loops;
	u32 loopStart;
	s16 loopPred;
	u8 loopIndex;
	s16 *pcm[1];         /* its decoded buffer, mixed straight away */
	volatile u8 live;    /* the mixer may read it (set last when it starts, cleared first when it stops) */
	volatile s32 gl, gr; /* its left / right gain for the mixer, /256 */
	u8 next;
	volatile u8 done;    /* every sample has been queued */
	volatile u32 doneAt; /* the frame it was */
	u8 positional;
	f32 pos[3], volume, level;
	f32 occl;            /* smoothed occlusion factor */
	u8 occlFrame;
	/* room reverb (Source's DSP): a feedback delay line per voice, fed by the dry signal; its tail keeps
	 * the voice alive for a moment after the sound ends */
	s16 *dline;
	u16 dpos;
	s32 lp, dryLp;       /* feedback damping / dry lowpass (occlusion, under water) state */
	s32 wet;             /* reverb send, /256 */
	s32 muffle;          /* dry lowpass coefficient /256: 256 = none */
	u32 tail;            /* samples of echo left after the sound's end */
	u8 held;             /* paused with the game (the pause menu); menu sounds are never held */
	u8 speech;           /* a line someone says: other sounds make way for it, not it for them */
	u8 pending;          /* waiting for its sound to come off the disc; starts in Audio_Update */
	u8 hdr[HEADER];      /* streaming: the sound's header (cached sounds keep theirs in the blob) */
	f32 baseVol;          /* the caller's volume (before the sound's own) */
	const PackSound *src; /* what it's playing (looping sounds are parked and brought back by it) */
} Channel;

/* Looping sounds out of earshot, or whose voice a nearer sound needed, are parked here instead of
 * dropped, and start again (under the same handle) once they can be heard and a voice is free: an
 * ambient_generic's hum used to be lost for good if the player was far away when it started or a
 * louder sound took its voice. Source keeps such sounds going silently. */
#define MAX_PARKED 32
typedef struct {
	u32 handle; /* 0 = free */
	const PackSound *s;
	f32 pos[3], volume, level;
} Parked;
static Parked parked[MAX_PARKED];

static Parked *parked_find(u32 handle)
{
	if (handle)
		for (int i = 0; i < MAX_PARKED; i++)
			if (parked[i].handle == handle)
				return &parked[i];
	return NULL;
}

static u32 park(u32 handle, const PackSound *s, const f32 pos[3], f32 volume, f32 level)
{
	for (int i = 0; i < MAX_PARKED; i++)
		if (!parked[i].handle) {
			parked[i].handle = handle;
			parked[i].s = s;
			memcpy(parked[i].pos, pos, 12);
			parked[i].volume = volume;
			parked[i].level = level;
			return handle;
		}
	return 0;
}

#define DLINE 4096           /* samples: up to 185 ms between echoes */
#define DMASK (DLINE - 1)

/* the room: delays (samples), feedback and damping (/256), how much of a sound goes into the reverb */
static volatile s32 roomDA = 700, roomDB = 1100, roomFb = 110, roomDamp = 150;
static volatile f32 roomWet = 0.3f, roomMixMin = 0.2f, roomMixMax = 0.7f;
static volatile u8 underwater;
static int dspPreset = 1;
static f32 autoSize = 300, autoOpen = 0;

static Channel chans[NUM_CHANNELS];
static AudioReader readBank;
static AudioOccluded occluded;
static u32 nextHandle = 1;
static f32 lpos[3], lyaw;
static f32 master = 1.0f;

void Audio_SetVolume(f32 v) { master = v < 0 ? 0 : (v > 1 ? 1 : v); }
void Audio_SetOcclusion(AudioOccluded fn) { occluded = fn; }

/* the next nibble byte, or -1 if a stream hasn't got it yet */
static inline int byte_at(const Channel *c, u32 b)
{
	if (!c->ring)
		return c->codes[b];
	if (b >= c->loadedTo)
		return -1;
	return c->ring[b % RING];
}

static void decode(Channel *c, s16 *out, u32 n)
{
	s32 pred = c->pred, index = c->index;
	u32 i = 0;
	while (i < n) {
		if (c->cursor >= c->samples) {
			if (!c->loops)
				break;
			if (c->ring) {
				c->restart = 1; /* the stream has to be refilled from the loop start */
				break;
			}
			c->cursor = c->loopStart;
			pred = c->loopPred;
			index = c->loopIndex;
		}
		u32 k = c->cursor;
		int byte = byte_at(c, k >> 1);
		if (byte < 0)
			break; /* stream underrun: silence until the disc catches up */
		u8 code = (k & 1) ? (byte & 15) : ((byte >> 4) & 15);
		s32 step = stepTable[index];
		s32 vp = step >> 3;
		if (code & 4)
			vp += step;
		if (code & 2)
			vp += step >> 1;
		if (code & 1)
			vp += step >> 2;
		pred += (code & 8) ? -vp : vp;
		if (pred > 32767)
			pred = 32767;
		else if (pred < -32768)
			pred = -32768;
		index += indexTable[code & 7];
		if (index < 0)
			index = 0;
		else if (index > 88)
			index = 88;
		out[i++] = (s16)pred;
		c->cursor++;
	}
	if (i < n)
		memset(out + i, 0, (n - i) * 2);
	c->pred = pred;
	c->index = index;
}

static u8 gamePaused;

static int finished(const Channel *c) { return !c->loops && c->cursor >= c->samples && !c->tail; }

static inline s16 clamp16(s32 v) { return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* the dry signal's lowpass (behind a wall, under water), then the room's echoes added */
static void process(Channel *c, s16 *buf, u32 n)
{
	s32 mu = underwater ? 40 : c->muffle;
	if (mu < 256) {
		s32 y = c->dryLp;
		for (u32 i = 0; i < n; i++) {
			y += ((buf[i] - y) * mu) >> 8;
			buf[i] = (s16)y;
		}
		c->dryLp = y;
	}
	if (!c->dline || c->wet <= 0)
		return;
	s32 dA = roomDA, dB = roomDB, fb = roomFb, damp = roomDamp, wet = c->wet;
	u16 w = c->dpos;
	s32 lp = c->lp;
	s16 *dl = c->dline;
	for (u32 i = 0; i < n; i++) {
		s32 a = dl[(w - dA) & DMASK], b = dl[(w - dB) & DMASK];
		s32 echo = (a * 3 + b * 2) / 5;
		lp += ((echo - lp) * damp) >> 8; /* walls soak up the highs */
		dl[w] = clamp16(buf[i] + ((lp * fb) >> 8));
		buf[i] = clamp16(buf[i] + ((lp * wet) >> 8));
		w = (u16)((w + 1) & DMASK);
	}
	c->dpos = w;
	c->lp = lp;
}

/* how long the echoes ring after a sound ends: until the feedback has died down to ~3% */
static u32 tail_for(s32 wet)
{
	if (wet <= 0 || roomFb <= 0)
		return 0;
	f32 fb = roomFb / 256.0f, k = logf(0.03f) / logf(fb > 0.05f ? fb : 0.05f);
	u32 t = (u32)(k * (roomDA > roomDB ? roomDA : roomDB));
	return t > AUDIO_RATE * 2 ? AUDIO_RATE * 2 : t;
}

/* fills the next buffer; returns its byte size (0 when the sound is over) */
static u32 fill(Channel *c, s16 **buf)
{
	if (finished(c))
		return 0;
	s16 *out = c->pcm[0];
	u32 n;
	if (!c->loops && c->cursor >= c->samples) {
		/* the sound is over: the room's echoes ring out */
		n = c->tail < CHUNK ? c->tail : CHUNK;
		c->tail -= n;
		memset(out, 0, n * 2);
	} else {
		u32 left = c->loops ? CHUNK : c->samples - c->cursor;
		n = left < CHUNK ? left : CHUNK;
		decode(c, out, n);
	}
	process(c, out, n);
	u32 bytes = n * 2;
	if (bytes & 31) { /* ASND wants 32-byte multiples: pad the tail with silence */
		u32 pad = 32 - (bytes & 31);
		memset((u8 *)out + bytes, 0, pad);
		bytes += pad;
	}
	DCFlushRange(out, bytes);
	*buf = out;
	return bytes;
}

static u32 frameNo;

/* the mixer: every live sound's next buffer, at its gains, summed into one stereo buffer for ASND */
static s16 *mixOut[NUM_BUFS];
static u8 mixNext;
static s32 mixAcc[CHUNK * 2];

static void mix_cb(s32 voice)
{
	if (voice != MIX_VOICE)
		return;
	memset(mixAcc, 0, sizeof(mixAcc));
	for (int i = 0; i < NUM_CHANNELS; i++) {
		Channel *c = &chans[i];
		if (!c->live || c->held || c->done)
			continue;
		s16 *buf;
		u32 bytes = fill(c, &buf);
		if (!bytes) {
			c->done = 1;
			c->doneAt = frameNo;
			continue;
		}
		u32 n = bytes / 2;
		if (n > CHUNK)
			n = CHUNK;
		s32 gl = c->gl, gr = c->gr;
		if (!gl && !gr)
			continue;
		for (u32 k = 0; k < n; k++) {
			mixAcc[k * 2] += (buf[k] * gl) >> 8;
			mixAcc[k * 2 + 1] += (buf[k] * gr) >> 8;
		}
	}
	s16 *out = mixOut[mixNext];
	mixNext = (u8)((mixNext + 1) % NUM_BUFS);
	for (u32 k = 0; k < CHUNK * 2; k++)
		out[k] = clamp16(mixAcc[k]);
	DCFlushRange(out, CHUNK * 4);
	ASND_AddVoice(MIX_VOICE, out, CHUNK * 4);
}

static u8 *ringPool[MAX_STREAMS];
static u8 ringUsed[MAX_STREAMS];

void Audio_Init(AudioReader reader)
{
	readBank = reader;
	memset(chans, 0, sizeof(chans));
	for (int i = 0; i < NUM_CHANNELS; i++) {
		chans[i].blob = -1;
		chans[i].pcm[0] = memalign(32, CHUNK * 2 + 32);
		chans[i].dline = memalign(32, DLINE * 2);
	}
	for (int i = 0; i < MAX_STREAMS; i++)
		ringPool[i] = memalign(32, RING); /* allocated once, so streaming never fragments the heap */
	ASND_Init();
	ASND_Pause(0);
	for (int b = 0; b < NUM_BUFS; b++) {
		mixOut[b] = memalign(32, CHUNK * 4);
		if (mixOut[b])
			memset(mixOut[b], 0, CHUNK * 4);
	}
	if (mixOut[0]) {
		DCFlushRange(mixOut[0], CHUNK * 4);
		mixNext = 1;
		ASND_SetVoice(MIX_VOICE, VOICE_STEREO_16BIT, AUDIO_RATE, 0, mixOut[0], CHUNK * 4, 255, 255, mix_cb);
	}
}

static u8 *ring_get(void)
{
	for (int i = 0; i < MAX_STREAMS; i++)
		if (ringPool[i] && !ringUsed[i]) {
			ringUsed[i] = 1;
			return ringPool[i];
		}
	return NULL;
}

static void ring_put(u8 *r)
{
	for (int i = 0; i < MAX_STREAMS; i++)
		if (ringPool[i] == r)
			ringUsed[i] = 0;
}

static void release(Channel *c)
{
	if (!c->handle)
		return;
	u32 level = IRQ_Disable();
	c->live = 0; /* (out of the mixer before anything it reads goes away) */
	c->handle = 0;
	c->held = 0;
	c->pending = 0;
	c->speech = 0;
	IRQ_Restore(level);
	if (c->blob >= 0 && cache[c->blob].refs)
		cache[c->blob].refs--;
	c->blob = -1;
	c->codes = NULL;
	ring_put(c->ring);
	c->ring = NULL;
}

static void evict(int i)
{
	free(cache[i].data);
	cacheBytes -= cache[i].size;
	memset(&cache[i], 0, sizeof(cache[i]));
}

/* ---- the loader: sounds not in the cache are read from the disc by a thread of their own, so the game
 * doesn't stop for the drive (a line used to freeze the frame for the whole seek and read, the game
 * clock lost the time, and the lines after it ran out of step) */
#define LOAD_QUEUE 32
static volatile s16 loadQ[LOAD_QUEUE];
static volatile u32 loadHead, loadTail;
static mutex_t ioLock = LWP_MUTEX_NULL;
static lwp_t loaderThread = LWP_THREAD_NULL;
static u8 loaderStack[16 * 1024] ATTRIBUTE_ALIGN(32);

static int read_locked(u32 offset, u32 len, void *buf)
{
	if (ioLock != LWP_MUTEX_NULL)
		LWP_MutexLock(ioLock);
	int r = readBank(offset, len, buf);
	if (ioLock != LWP_MUTEX_NULL)
		LWP_MutexUnlock(ioLock);
	return r;
}

static void *loader_main(void *arg)
{
	(void)arg;
	for (;;) {
		if (loadHead == loadTail) {
			usleep(2000);
			continue;
		}
		s16 slot = loadQ[loadTail % LOAD_QUEUE];
		Blob *b = &cache[slot];
		int r = read_locked(b->offset, b->size, b->data);
		b->loading = r == 0 ? 0 : 2;
		loadTail++;
	}
	return NULL;
}

static int loader_queue(s16 slot)
{
	if (loaderThread == LWP_THREAD_NULL || loadHead - loadTail >= LOAD_QUEUE)
		return -1;
	cache[slot].loading = 1;
	loadQ[loadHead % LOAD_QUEUE] = slot;
	loadHead++;
	return 0;
}

static void loader_drain(void)
{
	while (loaderThread != LWP_THREAD_NULL && loadHead != loadTail)
		usleep(1000);
}

/* the sound's ADPCM data, from the cache or read from the bank (async: the slot is marked loading) */
static s16 acquire_ex(const PackSound *s, int async);
static s16 acquire(const PackSound *s) { return acquire_ex(s, 1); }

static s16 acquire_ex(const PackSound *s, int async)
{
	useClock++;
	for (int i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].data && cache[i].offset == Sound_Offset(s)) {
			if (cache[i].loading == 2 && !cache[i].refs) {
				evict(i); /* its read failed: read it again rather than never playing this sound */
				break;
			}
			cache[i].refs++;
			cache[i].lastUse = useClock;
			return (s16)i;
		}
	u32 size = (Sound_Size(s) + 31) & ~31u;
	/* make room: drop the least recently used sounds nobody is playing */
	for (;;) {
		int freeSlot = -1, lru = -1;
		for (int i = 0; i < CACHE_SLOTS; i++) {
			if (!cache[i].data) {
				if (freeSlot < 0)
					freeSlot = i;
			} else if (!cache[i].refs && cache[i].loading != 1 &&
			           (lru < 0 || cache[i].lastUse < cache[lru].lastUse)) {
				lru = i;
			}
		}
		if (freeSlot >= 0 && cacheBytes + size <= CACHE_BYTES)
			break;
		if (lru < 0)
			break; /* everything is playing: allow going over budget */
		evict(lru);
	}
	int slot = -1;
	for (int i = 0; i < CACHE_SLOTS && slot < 0; i++)
		if (!cache[i].data)
			slot = i;
	if (slot < 0)
		return -1;
	u8 *data = memalign(32, size);
	if (!data)
		return -1;
	cache[slot].offset = Sound_Offset(s);
	cache[slot].size = size;
	cache[slot].data = data;
	cache[slot].refs = 1;
	cache[slot].lastUse = useClock;
	cache[slot].loading = 0;
	cacheBytes += size;
	if (async && loader_queue((s16)slot) == 0)
		return (s16)slot;
	if (read_locked(Sound_Offset(s), Sound_Size(s), data) != 0) {
		evict(slot);
		return -1;
	}
	return (s16)slot;
}

/* starts reading a sound into the cache ahead of time (a scene's lines when it starts) */
void Audio_Prefetch(const PackSound *s)
{
	if (!s || !readBank || Sound_Samples(s) == 0 || Sound_Size(s) > STREAM_MIN)
		return;
	for (int i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].data && cache[i].offset == Sound_Offset(s))
			return;
	s16 b = acquire_ex(s, 1);
	if (b >= 0 && cache[b].refs)
		cache[b].refs--; /* (nobody plays it yet) */
}

int Audio_Ready(const PackSound *s)
{
	if (!s || !readBank || Sound_Samples(s) == 0 || Sound_Size(s) > STREAM_MIN)
		return 1;
	for (int i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].data && cache[i].offset == Sound_Offset(s))
			return cache[i].loading != 1;
	return 0;
}

/* Source: gain = 1 / (distance * dist_mult), dist_mult from the sound level (60 dB at 36 units) */
static f32 level_gain(const f32 pos[3], f32 level)
{
	if (level <= 0)
		return 1.0f;
	f32 d[3] = {pos[0] - lpos[0], pos[1] - lpos[1], pos[2] - lpos[2]};
	f32 dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	f32 mult = powf(10.0f, (60.0f - level) / 20.0f) / 36.0f;
	f32 rel = dist * mult;
	f32 g = rel > 0.1f ? 1.0f / rel : 10.0f;
	return g > 1.0f ? 1.0f : g;
}

static f32 channel_gain(const Channel *c)
{
	f32 v = c->volume * master;
	if (c->positional)
		v *= level_gain(c->pos, c->level) * c->occl;
	return v > 1 ? 1 : v;
}

static void volumes(const Channel *c, s32 *l, s32 *r)
{
	f32 v = channel_gain(c);
	f32 pan = 0;
	if (c->positional && c->level > 0) {
		f32 d[3] = {c->pos[0] - lpos[0], c->pos[1] - lpos[1], c->pos[2] - lpos[2]};
		f32 dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dist > 1) {
			f32 yaw = lyaw * (f32)M_PI / 180.0f;
			/* right vector of the listener (Source: y is left) */
			f32 rx = sinf(yaw), ry = -cosf(yaw);
			pan = (d[0] * rx + d[1] * ry) / dist;
		}
	}
	if (v < MIN_GAIN)
		v = 0;
	f32 lv = v * (pan > 0 ? 1.0f - pan * 0.75f : 1.0f);
	f32 rv = v * (pan < 0 ? 1.0f + pan * 0.75f : 1.0f);
	*l = (s32)(lv * 255.0f);
	*r = (s32)(rv * 255.0f);
}

/* streaming: keep up to RING bytes of nibbles ahead of the cursor (main loop only) */
static void stream_refill(Channel *c)
{
	if (!c->ring)
		return;
	if (c->restart) {
		u32 level = IRQ_Disable();
		c->cursor = c->loopStart;
		c->pred = c->loopPred;
		c->index = c->loopIndex;
		c->loadedTo = c->loopStart >> 1;
		c->restart = 0;
		IRQ_Restore(level);
	}
	for (int guard = 0; guard < 4; guard++) {
		u32 have = c->loadedTo, cur = c->cursor >> 1;
		if (have >= c->totalBytes || have - cur > RING - BLOCK)
			break;
		u32 slot = have % RING;
		u32 n = BLOCK - (slot % BLOCK);
		if (n > c->totalBytes - have)
			n = c->totalBytes - have;
		if (slot + n > RING)
			n = RING - slot;
		/* read through an aligned bounce so the ring offset can be anything */
		static u8 bounce[BLOCK + 64] ATTRIBUTE_ALIGN(32);
		/* (if the loader has the drive, try next frame: the ring holds seconds) */
		if (ioLock != LWP_MUTEX_NULL && LWP_MutexTryLock(ioLock) != 0)
			break;
		int rr = readBank(c->bankOffset + have, n, bounce);
		if (ioLock != LWP_MUTEX_NULL)
			LWP_MutexUnlock(ioLock);
		if (rr != 0)
			break;
		memcpy(c->ring + slot, bounce, n);
		c->loadedTo = have + n;
	}
}

static Channel *pick_channel(f32 newGain, int speech)
{
	for (int i = 0; i < NUM_CHANNELS; i++)
		if (!chans[i].handle)
			return &chans[i];
	/* all busy: the quietest positional sound makes way; music and sounds heard everywhere stay. Speech
	 * (Source's CHAN_VOICE lines) outranks every effect: a line is never lost to footsteps or ambience,
	 * and effects never push a line out */
	Channel *best = NULL;
	f32 bestGain = 1e9f;
	for (int i = 0; i < NUM_CHANNELS; i++) {
		Channel *c = &chans[i];
		if (!c->positional)
			continue;
		if (c->speech && !speech)
			continue;
		f32 g = channel_gain(c) + (c->held ? 0.5f : 0.0f); /* (keep the paused game's sounds if possible) */
		if (speech && !c->speech)
			g -= 10.0f;
		if (g < bestGain) {
			bestGain = g;
			best = c;
		}
	}
	if (!best || bestGain > newGain)
		return NULL;
	audioDrops[2]++;
	if (best->src && Sound_Loops(best->src) && !best->speech) {
		audioDrops[3]++;
		park(best->handle, best->src, best->pos, best->baseVol, best->level);
	}
	release(best);
	return best;
}

/* how much of a sound goes into the room's reverb: like Source's dsp mix range, more for far sounds;
 * sounds heard everywhere (music, the suit, the menu, soundscape beds) stay dry */
static s32 channel_wet(const Channel *c)
{
	if (!c->positional || c->level <= 0 || roomWet <= 0)
		return 0;
	f32 d[3] = {c->pos[0] - lpos[0], c->pos[1] - lpos[1], c->pos[2] - lpos[2]};
	f32 dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	f32 k = dist / 1000.0f;
	k = k > 1 ? 1 : k;
	f32 mix = roomMixMin + (roomMixMax - roomMixMin) * k;
	return (s32)(roomWet * mix * 256.0f);
}

/* the room for a dsp preset (scripts/dsp_presets.txt's legacy room types 0-28): size S/M/L sets the
 * echo spacing and length; the material sets brightness (damping) and level */
static void room_for(int preset)
{
	static const struct {
		s16 dA, dB;
		u8 fb, damp;
		f32 wet;
	} t[] = {
		{0, 0, 0, 0, 0},              /* 0 off */
		{0, 0, 0, 0, 0},              /* 1 automatic (room_auto) */
		{420, 640, 140, 210, 0.55f},  /* 2-4 metallic */
		{700, 1050, 165, 210, 0.6f},
		{1100, 1650, 185, 200, 0.65f},
		{520, 780, 170, 160, 0.6f},   /* 5-7 tunnel */
		{900, 1350, 190, 150, 0.65f},
		{1400, 2100, 205, 140, 0.7f},
		{480, 720, 150, 130, 0.5f},   /* 8-10 chamber */
		{850, 1270, 170, 120, 0.55f},
		{1300, 1950, 190, 110, 0.6f},
		{450, 670, 140, 230, 0.55f},  /* 11-13 bright */
		{800, 1200, 165, 230, 0.6f},
		{1250, 1880, 185, 230, 0.65f},
		{300, 460, 60, 40, 0.3f},     /* 14-16 water */
		{300, 460, 60, 40, 0.3f},
		{300, 460, 60, 40, 0.3f},
		{500, 750, 140, 120, 0.5f},   /* 17-19 concrete */
		{900, 1350, 165, 110, 0.55f},
		{1350, 2000, 180, 100, 0.6f},
		{2200, 3300, 70, 90, 0.25f},  /* 20-22 outside: a distant slapback */
		{2600, 3900, 80, 90, 0.3f},
		{3000, 4000, 90, 90, 0.35f},
		{1200, 1800, 200, 100, 0.65f},/* 23-25 cavern */
		{1700, 2550, 210, 100, 0.7f},
		{2300, 3450, 220, 90, 0.75f},
		{600, 900, 200, 180, 0.6f},   /* 26-28 weird */
		{900, 1300, 210, 180, 0.65f},
		{1300, 2000, 220, 180, 0.7f},
	};
	if (preset == 1) {
		/* automatic, like Source's DAS: from how big the space around the listener is and how much of
		 * it is open sky */
		if (autoOpen > 0.5f) {
			roomDA = 2400, roomDB = 3600, roomFb = 70, roomDamp = 90, roomWet = 0.25f;
		} else {
			f32 sz = autoSize < 64 ? 64 : autoSize > 1500 ? 1500 : autoSize;
			f32 k = (sz - 64) / (1500 - 64);
			roomDA = (s32)(380 + k * 1300);
			roomDB = (s32)(roomDA * 1.5f);
			roomFb = (s32)(120 + k * 90);
			roomDamp = 140;
			roomWet = 0.4f + k * 0.25f;
		}
	} else if (preset >= 0 && preset < (int)(sizeof(t) / sizeof(t[0]))) {
		roomDA = t[preset].dA, roomDB = t[preset].dB, roomFb = t[preset].fb, roomDamp = t[preset].damp;
		roomWet = t[preset].wet;
	} else {
		roomWet = 0;
	}
	if (roomDA >= DLINE)
		roomDA = DLINE - 1;
	if (roomDB >= DLINE)
		roomDB = DLINE - 1;
	if (roomDA < 1 || roomDB < 1)
		roomWet = 0;
}

void Audio_SetDsp(int preset)
{
	dspPreset = preset;
	room_for(preset);
}

void Audio_SetAutoRoom(f32 size, f32 openness)
{
	autoSize += (size - autoSize) * 0.5f;
	autoOpen += (openness - autoOpen) * 0.5f;
	if (dspPreset == 1)
		room_for(1);
}

void Audio_SetUnderwater(int on) { underwater = (u8)(on != 0); }

static int start_channel(Channel *c);

static u32 play_as(const PackSound *s, const f32 *pos, f32 volume, f32 soundlevel, int speech, u32 handle);
static u32 play(const PackSound *s, const f32 *pos, f32 volume, f32 soundlevel, int speech)
{
	return play_as(s, pos, volume, soundlevel, speech, 0);
}

static u32 new_handle(void)
{
	u32 h = nextHandle++;
	if (!nextHandle)
		nextHandle = 1;
	return h;
}

/* handle: 0 for a new sound; a parked loop coming back keeps the handle its owner knows it by */
static u32 play_as(const PackSound *s, const f32 *pos, f32 volume, f32 soundlevel, int speech, u32 handle)
{
	if (!s || !readBank || Sound_Samples(s) == 0)
		return 0;
	if (ioLock == LWP_MUTEX_NULL) { /* first sound: the loader thread starts */
		LWP_MutexInit(&ioLock, false);
		if (LWP_CreateThread(&loaderThread, loader_main, NULL, loaderStack, sizeof(loaderStack), 70) < 0)
			loaderThread = LWP_THREAD_NULL;
	}
	f32 level = soundlevel < 0 ? (f32)Sound_Level(s) : soundlevel;
	f32 vol = volume * (s->sizeVol >> 24 ? Sound_Volume(s) : 1.0f);
	f32 newGain = vol * (pos ? level_gain(pos, level) : 1.0f);
	int loop = pos && level > 0 && !speech && Sound_Loops(s);
	if (pos && level > 0 && newGain < MIN_GAIN) /* too far away to hear: a loop waits until it isn't */
		return loop ? park(handle ? handle : new_handle(), s, pos, volume, level) : 0;
	Channel *c = pick_channel(pos ? newGain : 2.0f, speech);
	if (!c) {
		audioDrops[0]++;
		return loop ? park(handle ? handle : new_handle(), s, pos, volume, level) : 0;
	}
	c->src = s;
	c->baseVol = volume;
	c->speech = (u8)(speech != 0);
	c->pending = 0;
	u32 size = Sound_Size(s);
	int stream = size > STREAM_MIN;
	if (stream) {
		int streams = 0;
		for (int i = 0; i < NUM_CHANNELS; i++)
			streams += chans[i].handle && chans[i].ring;
		if (streams >= MAX_STREAMS)
			stream = 0;
	}
	static u8 hdr[32] ATTRIBUTE_ALIGN(32);
	const u8 *h;
	if (stream) {
		c->ring = ring_get();
		if (!c->ring || read_locked(Sound_Offset(s), 32, hdr) != 0) {
			ring_put(c->ring);
			c->ring = NULL;
			return 0;
		}
		h = hdr;
		c->blob = -1;
		c->codes = NULL;
		c->bankOffset = Sound_Offset(s) + HEADER;
		c->loadedTo = 0;
		c->restart = 0;
	} else {
		s16 blob = acquire(s);
		if (blob < 0) {
			audioDrops[1]++;
			return 0;
		}
		c->blob = blob;
		h = cache[blob].data;
		c->codes = h + HEADER;
	}
	c->totalBytes = size > HEADER ? size - HEADER : 0;
	c->samples = Sound_Samples(s);
	c->positional = pos != NULL;
	if (pos)
		memcpy(c->pos, pos, sizeof(c->pos));
	c->volume = vol;
	c->level = level;
	c->handle = handle ? handle : new_handle();
	memcpy(c->hdr, h, HEADER);
	if (!stream && cache[c->blob].loading) {
		c->pending = 1; /* Audio_Update starts it once the loader has it */
		return c->handle;
	}
	if (!start_channel(c))
		return 0;
	return c->handle;
}

u32 Audio_Play(const PackSound *s, const f32 *pos, f32 volume, f32 soundlevel)
{
	return play(s, pos, volume, soundlevel, 0);
}

u32 Audio_PlaySpeech(const PackSound *s, const f32 *pos, f32 volume, f32 soundlevel)
{
	return play(s, pos, volume, soundlevel, 1);
}

/* the sound's data is there: read its header, prime the buffers and start the voice */
static int start_channel(Channel *c)
{
	const u8 *h = c->blob >= 0 ? cache[c->blob].data : c->hdr;
	c->pending = 0;
	c->cursor = 0;
	c->pred = (s16)((h[0] << 8) | h[1]);
	c->index = h[2] > 88 ? 88 : h[2];
	c->loops = h[3] & 1;
	c->loopStart = (u32)h[4] << 24 | (u32)h[5] << 16 | (u32)h[6] << 8 | h[7];
	c->loopPred = (s16)((h[8] << 8) | h[9]);
	c->loopIndex = h[10] > 88 ? 88 : h[10];
	if (c->loopStart >= c->samples)
		c->loops = 0;
	c->next = 0;
	c->done = 0;
	c->occl = 1.0f;
	c->muffle = 256;
	c->dryLp = 0;
	c->lp = 0;
	c->wet = channel_wet(c);
	c->tail = tail_for(c->wet);
	if (c->dline && c->wet > 0)
		memset(c->dline, 0, DLINE * 2);
	c->occlFrame = (u8)(c - chans);
	if (c->ring) {
		stream_refill(c);
		stream_refill(c);
	}
	if (c->positional && occluded && c->level > 0 && occluded(lpos, c->pos)) {
		c->occl = OCCLUDED_GAIN;
		c->muffle = 90;
	}

	s32 l, r;
	volumes(c, &l, &r);
	c->gl = l;
	c->gr = r;
	c->live = 1; /* the mixer takes it from its next buffer */
	return 1;
}

static Channel *find(u32 handle)
{
	if (!handle)
		return NULL;
	for (int i = 0; i < NUM_CHANNELS; i++)
		if (chans[i].handle == handle)
			return &chans[i];
	return NULL;
}

#ifdef HOST_TEST
void *audioStopLog[4096];
int audioStopN;
#endif
void Audio_Stop(u32 handle)
{
	Channel *c = find(handle);
#ifdef HOST_TEST
	if (c && !c->done && c->live && audioStopN < 4096 && c->cursor + AUDIO_RATE / 4 < c->samples)
		audioStopLog[audioStopN++] = __builtin_return_address(0); /* (cut off with more than 0.25 s to go) */
#endif
	if (c)
		release(c);
	Parked *q = parked_find(handle);
	if (q)
		q->handle = 0;
}

/* the pause menu: every sound playing now (dialogue, effects, ambience) stops where it is and carries
 * on when the game resumes, like Source pausing its sound with the game; sounds started while paused
 * (the menu's) play normally */
void Audio_Pause(int on)
{
	on = on != 0;
	if (on == gamePaused)
		return;
	gamePaused = (u8)on;
	for (int i = 0; i < NUM_CHANNELS; i++) {
		Channel *c = &chans[i];
		if (!c->handle)
			continue;
		if (on)
			c->held = 1; /* (the mixer skips it, its place kept) */
		else if (c->held)
			c->held = 0;
	}
}

int Audio_Paused(void) { return gamePaused; }

void Audio_StopAll(void)
{
	gamePaused = 0;
	for (int i = 0; i < NUM_CHANNELS; i++)
		release(&chans[i]);
	memset(parked, 0, sizeof(parked));
}

/* between levels: drop every cached sound, so the next map's memory isn't split up by them */
void Audio_FlushCache(void)
{
	Audio_StopAll();
	loader_drain();
	for (int i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].data)
			evict(i);
}

int Audio_IsPlaying(u32 handle) { return find(handle) != NULL || parked_find(handle) != NULL; }

void Audio_SetChannelVolume(u32 handle, f32 volume)
{
	Channel *c = find(handle);
	if (c) {
		c->volume = volume;
		c->baseVol = volume;
	}
	Parked *q = parked_find(handle);
	if (q)
		q->volume = volume;
}

f32 Audio_GainAt(const f32 pos[3], f32 soundlevel)
{
	if (!pos)
		return master;
	f32 g = level_gain(pos, soundlevel) * master;
	if (occluded && soundlevel > 0 && g > MIN_GAIN && occluded(lpos, pos))
		g *= OCCLUDED_GAIN;
	return g;
}

void Audio_SetPos(u32 handle, const f32 pos[3])
{
	Channel *c = find(handle);
	if (c && c->positional)
		memcpy(c->pos, pos, sizeof(c->pos));
	Parked *q = parked_find(handle);
	if (q)
		memcpy(q->pos, pos, sizeof(q->pos));
}

void Audio_Update(const f32 listener[3], f32 yaw)
{
	memcpy(lpos, listener, sizeof(lpos));
	lyaw = yaw;
	frameNo++;
	if (!gamePaused && (frameNo & 7) == 0) {
		/* parked loops: back as soon as they can be heard (one a pass: starting one may read the disc) */
		for (int i = 0; i < MAX_PARKED; i++) {
			Parked *q = &parked[(i + (frameNo >> 3)) % MAX_PARKED];
			if (!q->handle)
				continue;
			f32 g = q->volume * (q->s->sizeVol >> 24 ? Sound_Volume(q->s) : 1.0f) * level_gain(q->pos, q->level);
			if (g < MIN_GAIN * 1.5f)
				continue;
			Parked cp = *q;
			q->handle = 0;
			play_as(cp.s, cp.pos, cp.volume, cp.level, 0, cp.handle); /* (parks it again if no voice is free) */
			break;
		}
	}
	for (int i = 0; i < NUM_CHANNELS; i++) {
		Channel *c = &chans[i];
		if (!c->handle)
			continue;
		if (c->held)
			continue; /* paused with the game: left exactly as it is */
		if (c->pending) {
			u8 ld = c->blob >= 0 ? cache[c->blob].loading : 2;
			if (ld == 2) {
				s16 bl = c->blob;
				release(c);
				if (bl >= 0 && cache[bl].data && !cache[bl].refs)
					evict(bl); /* (so the next time it's played it's read again) */
			} else if (ld == 0 && !start_channel(c)) {
				/* (start_channel released it) */
			}
			continue;
		}
		/* finished: everything queued and the mixer has played it. (libasnd never marks a voice with a
		 * callback unused when it runs dry: it reads "working" forever. Waiting for that kept every
		 * finished voice taken, so after 15 non-positional sounds (menu clicks, music, the suit, the
		 * soundscape) nothing more could play, in the menu or the game. Now: no buffer queued behind the
		 * one playing, and longer than a buffer lasts since the last was queued.) */
		if (c->done) { /* (the mixer has had the last of it) */
			release(c);
			continue;
		}
		stream_refill(c);
		/* walls in the way muffle it: re-checked a few times a second, eased so it doesn't click */
		if (c->positional && occluded && c->level > 0 && ((frameNo + c->occlFrame) & 7) == 0) {
			f32 want = occluded(lpos, c->pos) ? OCCLUDED_GAIN : 1.0f;
			c->occl += (want - c->occl) * 0.5f;
			/* behind a wall it's muffled too (Source lowpasses occluded sounds), not just quieter */
			c->muffle = (s32)(90 + (256 - 90) * (c->occl - OCCLUDED_GAIN) / (1.0f - OCCLUDED_GAIN));
			if (!c->done && !(!c->loops && c->cursor >= c->samples))
				c->wet = channel_wet(c);
		}
		s32 l, r;
		volumes(c, &l, &r);
		c->gl = l;
		c->gr = r;
	}
}

/* (tests) sounds the mixer is playing now, sounds held by a pause; a sound's position */
const u32 *Audio_DebugDrops(void) { return audioDrops; }

int Audio_DebugCounts(int *held)
{
	int n = 0, h = 0;
	for (int i = 0; i < NUM_CHANNELS; i++) {
		if (!chans[i].handle)
			continue;
		if (chans[i].held)
			h++;
		else if (chans[i].live && !chans[i].done)
			n++;
	}
	if (held)
		*held = h;
	return n;
}

u32 Audio_DebugCursor(u32 handle)
{
	Channel *c = find(handle);
	return c ? c->cursor : 0;
}
