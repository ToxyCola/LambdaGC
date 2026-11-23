/*
 * hl2gc audio: voice lines, music and world sounds from hl2gc/vo.bin (IMA ADPCM, 22050 Hz mono),
 * decoded on the fly into small buffers and mixed by libogc's ASND.
 *
 * Positional sounds fall off with distance the way Source's do (by sound level in dB: gain =
 * reference distance / distance), are muffled when a wall is in the way, and pan left/right.
 * WAVs with a cue point loop like on PC. Long sounds (music) stream from the disc instead of being
 * loaded whole. When every voice is busy, the quietest positional sound makes way.
 */
#ifndef HL2GC_AUDIO_H
#define HL2GC_AUDIO_H

#include <gctypes.h>

#include "pack.h"

#define AUDIO_RATE 22050
#define SNDLVL_TALKING 80.0f
/* soundlevel argument: use the level the sound's soundscript gives it */
#define SNDLVL_DEFAULT (-1.0f)

/* Reads len bytes at offset of vo.bin into buf (32-byte aligned). Returns 0 on success. */
typedef int (*AudioReader)(u32 offset, u32 len, void *buf);
/* Returns 1 if the line between a and b is blocked by the world (sounds behind walls are muffled). */
typedef int (*AudioOccluded)(const f32 a[3], const f32 b[3]);

void Audio_Init(AudioReader reader);
void Audio_SetOcclusion(AudioOccluded fn);
/* Starts a sound; pos may be NULL (heard everywhere). soundlevel in dB (0 = everywhere,
 * SNDLVL_DEFAULT = the sound's own). Returns a handle > 0, or 0. */
u32 Audio_Play(const PackSound *s, const f32 *pos, f32 volume, f32 soundlevel);
/* the same for a line someone says: other sounds give their voice up for it (Source's CHAN_VOICE) */
u32 Audio_PlaySpeech(const PackSound *s, const f32 *pos, f32 volume, f32 soundlevel);
/* reads a sound into the cache in the background, ready for when it plays */
void Audio_Prefetch(const PackSound *s);
/* 1 when the sound would start right away: in the cache and read (or long enough to stream) */
int Audio_Ready(const PackSound *s);
void Audio_Stop(u32 handle);
void Audio_StopAll(void);
/* pause menu: holds every sound playing now until Audio_Pause(0); sounds started meanwhile play */
void Audio_Pause(int on);
int Audio_Paused(void);
/* stops everything and frees the sound cache (between levels) */
void Audio_FlushCache(void);
/* master volume 0..1 (options menu) */
void Audio_SetVolume(f32 v);
int Audio_IsPlaying(u32 handle);
void Audio_SetChannelVolume(u32 handle, f32 volume);
/* how loud a sound at pos with this soundlevel would be for the listener right now (0..1) */
f32 Audio_GainAt(const f32 pos[3], f32 soundlevel);
/* Moves a playing sound (e.g. with the NPC speaking it). */
void Audio_SetPos(u32 handle, const f32 pos[3]);
/* Once per frame: listener position and yaw (degrees); frees finished sounds, streams, updates volume/pan. */
void Audio_Update(const f32 listener[3], f32 yaw);
/* the room's reverb, like Source's DSP: a dsp_presets.txt room type (soundscapes' "dsp"; 1 = automatic) */
void Audio_SetDsp(int preset);
/* for the automatic room: the space around the listener (average distance to walls, units) and how open
 * to the sky it is (0..1); call a few times a second */
void Audio_SetAutoRoom(f32 size, f32 openness);
/* under water: everything is muffled */
void Audio_SetUnderwater(int on);
/* Seconds of audio in a sound. */
static inline f32 Audio_Duration(const PackSound *s) { return (f32)Sound_Samples(s) / (f32)AUDIO_RATE; }

int Audio_DebugCounts(int *held); /* (tests) */
const u32 *Audio_DebugDrops(void); /* (tests) no channel, no cache, pushed out, of those loops parked */
u32 Audio_DebugCursor(u32 handle);

#endif
