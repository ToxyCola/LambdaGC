/*
 * Host test for audio.c with a stand-in ASND mixer: voices consume their buffers in "real time" as the
 * test advances the clock, and call their callbacks for the next buffer like the DSP does.
 * Checks: sounds play to the end and free their voice; a pause holds game sounds (dialogue) where they
 * are while menu sounds still play; after the pause everything resumes and new sounds play.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <asndlib.h>
#include "audio.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>

/* ---- stand-in threads and mutexes (pthreads) for the sound loader */
static pthread_mutex_t mtx[8];
static int nmtx;
s32 LWP_MutexInit(mutex_t *m, bool rec) { (void)rec; pthread_mutex_init(&mtx[nmtx], NULL); *m = (mutex_t)(++nmtx); return 0; }
s32 LWP_MutexLock(mutex_t m) { return pthread_mutex_lock(&mtx[m - 1]); }
s32 LWP_MutexTryLock(mutex_t m) { return pthread_mutex_trylock(&mtx[m - 1]) ? 1 : 0; }
s32 LWP_MutexUnlock(mutex_t m) { return pthread_mutex_unlock(&mtx[m - 1]); }
s32 LWP_CreateThread(lwp_t *t, void *(*entry)(void *), void *arg, void *stack, u32 size, u8 prio)
{
	(void)stack; (void)size; (void)prio;
	pthread_t th;
	if (pthread_create(&th, NULL, entry, arg)) return -1;
	pthread_detach(th);
	*t = 1;
	return 0;
}
static int slowReads; /* ms each read takes, like a disc seek */

/* ---- stand-in ASND */
typedef struct {
	int used, paused;
	s32 queued; /* samples left in the current + next buffer */
	ASNDVoiceCallback cb;
	s32 nextPending;
	long played; /* samples mixed */
	int vl, vr;
	int frameBytes; /* 2 mono, 4 stereo 16-bit */
} V;
static V voice[16];
static int globalPause;
void ASND_Init(void) {}
void ASND_Pause(s32 p) { globalPause = p; }
s32 ASND_Is_Paused(void) { return globalPause; }
s32 ASND_SetVoice(s32 v, s32 fmt, s32 pitch, s32 delay, void *snd, s32 size, s32 l, s32 r, ASNDVoiceCallback cb)
{
	(void)pitch; (void)delay; (void)snd;
	voice[v].frameBytes = fmt == VOICE_STEREO_16BIT ? 4 : 2;
	voice[v].used = 1; voice[v].paused = 0; voice[v].queued = size / voice[v].frameBytes; voice[v].cb = cb; voice[v].nextPending = 0;
	voice[v].played = 0; voice[v].vl = l; voice[v].vr = r;
	if (cb) cb(v); /* asks for the second buffer straight away */
	return SND_OK;
}
s32 ASND_AddVoice(s32 v, void *snd, s32 size) { (void)snd; if (voice[v].nextPending) return SND_INVALID; voice[v].nextPending = size / (voice[v].frameBytes ? voice[v].frameBytes : 2); return SND_OK; }
s32 ASND_StopVoice(s32 v) { voice[v].used = 0; return SND_OK; }
s32 ASND_PauseVoice(s32 v, s32 p) { voice[v].paused = p; return SND_OK; }
/* as libasnd does it: a voice with a callback is never marked unused when it runs dry (only one
 * without a callback is), so it reads as working until it's stopped */
s32 ASND_StatusVoice(s32 v)
{
	if (!voice[v].used) return SND_UNUSED;
	if (voice[v].paused) return SND_WAITING;
	return SND_WORKING;
}
s32 ASND_TestVoiceBufferReady(s32 v) { return !(voice[v].queued > 0 && voice[v].nextPending); }
static int voice_dry(int v) { return voice[v].queued <= 0 && !voice[v].nextPending; }
s32 ASND_ChangeVolumeVoice(s32 v, s32 l, s32 r) { voice[v].vl = l; voice[v].vr = r; return SND_OK; }
/* advance the mixer by n samples */
static void mix(s32 n)
{
	if (globalPause) return;
	for (int v = 1; v < 16; v++) {
		V *x = &voice[v];
		if (!x->used || x->paused) continue;
		s32 left = n;
		while (left > 0) {
			if (x->queued <= 0) {
				if (!x->nextPending) break;
				x->queued = x->nextPending; x->nextPending = 0;
				if (x->cb) x->cb(v);
			}
			s32 k = left < x->queued ? left : x->queued;
			x->queued -= k; x->played += k; left -= k;
		}
	}
}

/* ---- a bank of fake sounds: 12-byte header + nibbles */
static u8 bank[1 << 20];
static int reader(u32 off, u32 len, void *buf) { if (slowReads) usleep(slowReads * 1000); memcpy(buf, bank + off, len); return 0; }
static double secs_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static PackSound make(u32 off, u32 samples)
{
	memset(bank + off, 0, 12);
	memset(bank + off + 12, 0x11, samples / 2);
	PackSound s = {off, (255u << 24) | (12 + samples / 2), samples};
	return s;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void run(f32 secs) { f32 z[3] = {0, 0, 0}; for (int i = 0; i < (int)(secs * 60); i++) { usleep(1000); mix(AUDIO_RATE / 60); Audio_Update(z, 0); } }
/* (the mixer plays every sound through one ASND voice: count sounds, not voices) */
static int playing_voices(void) { return Audio_DebugCounts(NULL); }
static int held_voices(void) { int h, n = Audio_DebugCounts(&h); return n + h; }

int main(void)
{
	Audio_Init(reader);
	PackSound line = make(0, AUDIO_RATE * 4);          /* a 4 s line of dialogue */
	PackSound blip = make(200000, AUDIO_RATE / 4);     /* a menu blip */
	PackSound step = make(300000, AUDIO_RATE / 2);
	f32 at[3] = {100, 0, 0};
	u32 h = Audio_Play(&line, at, 1, 75);
	CHECK(h, "line starts");
	run(1);
	CHECK(Audio_IsPlaying(h), "line playing after 1 s");
	/* pause (the menu): the line holds, menu sounds play */
	Audio_Pause(1);
	run(0.1f);
	int vl = 1;
	long before = (long)Audio_DebugCursor(h);
	u32 m = Audio_Play(&blip, NULL, 1, 0);
	CHECK(m, "menu sound plays while paused");
	run(5);
	CHECK(vl > 0 && (long)Audio_DebugCursor(h) == before, "dialogue held during the pause (%ld -> %ld)", before, (long)Audio_DebugCursor(h));
	CHECK(Audio_IsPlaying(h), "line still there after a 5 s pause");
	CHECK(!Audio_IsPlaying(m), "menu sound finished");
	/* unpause: the line carries on where it was and ends; new sounds play */
	Audio_Pause(0);
	run(1);
	CHECK(Audio_IsPlaying(h), "line resumes");
	run(4);
	if (Audio_IsPlaying(h)) { int hd; int n = Audio_DebugCounts(&hd); printf("line cursor %u of %u, playing %d held %d, voice1 used %d queued %d pend %d\n", Audio_DebugCursor(h), AUDIO_RATE * 4, n, hd, voice[1].used, voice[1].queued, voice[1].nextPending); }
	CHECK(!Audio_IsPlaying(h), "line ends");
	u32 s2 = Audio_Play(&step, at, 1, 75);
	CHECK(s2, "a new sound after the pause starts");
	run(0.1f);
	CHECK(playing_voices() >= 1, "and is heard");
	/* many sounds at once, then all free again */
	for (int k = 0; k < 30; k++) Audio_Play(&step, at, 1, 75);
	run(3);
	CHECK(playing_voices() == 0, "%d voices stuck after everything ended", playing_voices());
	/* everything queued is heard: 35 sounds at once all play (15 hardware voices used to drop the rest) */
	{
		u32 hs[35];
		int got = 0;
		for (int k = 0; k < 35; k++) hs[k] = Audio_Play(&step, at, 1, 75);
		run(0.1f);
		for (int k = 0; k < 35; k++) got += hs[k] && Audio_IsPlaying(hs[k]);
		CHECK(got == 35 && playing_voices() == 35, "35 sounds at once: %d playing", playing_voices());
		run(3);
	}
	u32 s3 = Audio_Play(&step, at, 1, 75);
	CHECK(s3 && playing_voices() == 1, "a sound plays after a burst");
	/* the loader: a sound not in the cache comes off a slow "disc" without holding up the game */
	run(3);
	Audio_FlushCache();
	slowReads = 150;
	PackSound fresh = make(400000, AUDIO_RATE);
	double t0 = secs_now();
	u32 f = Audio_Play(&fresh, at, 1, 75);
	double took = secs_now() - t0;
	CHECK(f && took < 0.02, "an uncached sound returns at once (%.0f ms)", took * 1000);
	CHECK(playing_voices() == 0, "and isn't heard before it's read");
	for (int k = 0; k < 30 && playing_voices() == 0; k++) { usleep(10000); run(1.0f / 60); }
	CHECK(playing_voices() == 1 && Audio_IsPlaying(f), "then it plays");
	/* a prefetched line is ready by the time it's said */
	PackSound pre = make(500000, AUDIO_RATE);
	Audio_Prefetch(&pre);
	usleep(300000);
	t0 = secs_now();
	u32 p2 = Audio_PlaySpeech(&pre, at, 1, 75);
	run(1.0f / 60);
	CHECK(p2 && secs_now() - t0 < 0.02 && playing_voices() >= 2, "a prefetched line starts straight away");
	slowReads = 0;
	run(3);
	/* speech outranks effects: every voice busy with loud effects, a quiet line still gets one, and
	 * effects can't take a line's voice */
	PackSound loop = make(600000, AUDIO_RATE * 8);
	f32 near[3] = {10, 0, 0}, far[3] = {1500, 0, 0};
	for (int k = 0; k < 40; k++) Audio_Play(&loop, near, 1, 75);
	run(0.1f);
	u32 sp = Audio_PlaySpeech(&line, far, 1, 75);
	run(0.1f);
	CHECK(sp && Audio_IsPlaying(sp), "a line gets a voice when effects have them all");
	for (int k = 0; k < 40; k++) Audio_Play(&step, near, 1, 75);
	run(0.1f);
	CHECK(Audio_IsPlaying(sp), "effects don't take the line's voice");
	/* looping sounds aren't lost: one started out of earshot is heard once the listener gets there, and
	 * one whose voice a nearer sound took comes back when a voice frees up */
	Audio_StopAll();
	run(0.2f);
	PackSound hum = make(700000, AUDIO_RATE * 2);
	bank[700000 + 3] = 1; /* loops from sample 0 */
	hum.offset |= 1;
	f32 away[3] = {60000, 0, 0};
	u32 hh = Audio_Play(&hum, away, 1, 75);
	run(0.2f);
	CHECK(hh && Audio_IsPlaying(hh) && playing_voices() == 0, "a far loop is kept, silent (%u, %d voices)", hh, playing_voices());
	for (int i = 0; i < 30; i++) { usleep(1000); mix(AUDIO_RATE / 60); Audio_Update(away, 0); }
	CHECK(playing_voices() == 1 && Audio_IsPlaying(hh), "and is heard when the listener arrives (%d voices)", playing_voices());
	Audio_StopAll();
	run(0.2f);
	f32 mid[3] = {300, 0, 0};
	u32 h2 = Audio_Play(&hum, mid, 1, 75);
	run(0.3f);
	for (int k = 0; k < 40; k++) Audio_Play(&step, near, 1, 75); /* half-second effects take every voice */
	run(0.1f);
	CHECK(Audio_IsPlaying(h2), "a loop pushed out by nearer sounds is kept");
	run(1.5f);
	CHECK(Audio_IsPlaying(h2) && playing_voices() == 1, "and plays again when they end (%d voices)", playing_voices());
	Audio_Stop(h2);
	run(0.3f);
	CHECK(!Audio_IsPlaying(h2) && playing_voices() == 0, "and stops when told");
	/* the menu: 100 short non-positional clicks one after another; every one plays, and the voices are
	 * given back (they used to stay taken once played, so after 15 the menu went silent) */
	Audio_StopAll();
	run(0.3f);
	PackSound click = make(800000, AUDIO_RATE / 10);
	int heard = 0;
	for (int k = 0; k < 100; k++) {
		u32 ch = Audio_Play(&click, NULL, 1, 0);
		run(0.06f); /* (the first time it's read from the disc) */
		heard += ch && playing_voices() >= 1;
		run(0.3f);
	}
	CHECK(heard == 100, "menu clicks heard: %d of 100", heard);
	CHECK(held_voices() == 0, "%d voices still taken after the clicks ended", held_voices());
	printf("%s\n", fails ? "FAILED" : "audio: all checks passed");
	return fails != 0;
}
