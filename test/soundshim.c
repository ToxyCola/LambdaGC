/* Host stand-ins for libogc threads and ASND (from audiotest.c), for linking the real audio.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <asndlib.h>
#include <pthread.h>
#include <unistd.h>
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
void shim_mix(s32 n)
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

