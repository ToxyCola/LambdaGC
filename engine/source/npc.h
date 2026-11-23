/*
 * hl2gc NPCs: characters placed by the map (and spawned by npc_template_maker), animated with
 * their baked skeletal sequences, walking straight-line paths with collision, following the
 * player, and acting out scripted sequences and choreographed scenes.
 *
 * There's no combat AI yet: hostile NPCs stand and idle.
 */
#ifndef HL2GC_NPC_H
#define HL2GC_NPC_H

#include "ent.h"
#include "pack.h"
#include "player.h"

#define NPC_MAX_BONES 128
#define NPC_MAX_FLEXCTL 64

#define NPC_MAX_GESTURES 4
#define NPC_MAX_PATH 48
/* a gesture layer (CBaseAnimatingOverlay): sequence at a cycle, weight, and time for realtime sub-layers */
typedef struct {
	s16 seq;
	f32 cycle, weight, time;
} AnimLayer;

/* what goes on top of the main sequence: gesture layers and a look-at head turn (degrees, body-relative) */
typedef struct {
	const AnimLayer *layers;
	int numLayers;
	f32 lookYaw, lookPitch, lookWeight;
	u8 aimOn;               /* cameras, turrets: their aim_yaw / aim_pitch (degrees) */
	f32 aimYaw, aimPitch;
} AnimExtra;

typedef struct Npc {
	s32 ent;                  /* entity index */
	u16 model;
	const PackSkel *skel;
	f32 yaw, idealYaw;        /* degrees */

	/* animation: current sequence, cross-faded from the previous one */
	s16 seq, prevSeq;
	f32 t, prevT, blend;
	u8 locked;                /* scripted/scene sequence: locomotion doesn't change it */
	u8 hold;                  /* non-looping sequence holds its last frame */
	f32 seqDuration;

	/* movement */
	u8 moving, running, onGround;
	u8 inAScript;     /* StartScripting..StopScripting (CAI_BaseNPC::m_bInAScript): deaf, not +USE-able */
	s32 groundEnt;            /* entity stood on (a train, a lift); <= 0 the world */
	f32 goal[3], stopDist;
	f32 fallSpeed;
	f32 stuckTime;
	f32 avoidUntil;   /* steering round something in the way until then (local navigation) */
	s8 avoidSide;     /* +1 left, -1 right */
	s32 followEnt;            /* -1 = not following */
	f32 followDist;
	s32 faceEnt;              /* keep turning toward this entity while idle (-1 none) */

	/* scripted_sequence state */
	s32 script;               /* entity index of the scripted_sequence, -1 none */
	u8 scriptPhase;

	u32 voice;                /* playing speech */
	f32 speakUntil, speakStart;
	s16 speakSound;           /* PackSound index of the line (lip sync), -1 none */
	f32 flexCtl[NPC_MAX_FLEXCTL]; /* face controllers set by scenes this tick */
	f32 health;
	u8 dead;
	u8 prop;                  /* a multi-bone prop drawn posed/animated: no AI, no movement */
	s16 defaultSeq;           /* props: DefaultAnim, returned to after SetAnimation; -1 none */

	/* scene gestures, set by the scene each tick (seq -1 = free) */
	AnimLayer gest[NPC_MAX_GESTURES];
	/* look-at: the scene's target (or the player when idle and close), smoothed */
	s32 lookEnt;              /* -1 none */
	/* route through the map's node graph to goal (node indices), -1 terminated by pathLen */
	u16 path[NPC_MAX_PATH];
	u8 pathLen, pathPos, replanned;
	/* stepped aside for the player: where to go back to, and when */
	u8 aside;
	/* combat (hostile NPCs): weapon, attack timing, where the player was last seen */
	u8 weapon, burst, chasing;
	f32 nextAttack, lastSeen, seenPos[3], rechase;
	u8 aimOn;                 /* cameras, turrets: aim by aim_yaw / aim_pitch */
	f32 aimYaw, aimPitch;
	s32 enemy;                /* what it's fighting: 0 the player, an NPC or npc_bullseye entity, -1 nothing */
	f32 enemyDist;
	f32 alertUntil;
	f32 moveFrac;
	f32 progBest, progT;      /* closest it has got to its current waypoint, and how long since it got closer */
	u8 progPos;             /* how much of its stride it actually covered lately (0 blocked .. 1 free) */           /* it heard combat or was hurt: it knows to look all around until then */
	f32 asideHome[3], asideReturn;
	u8 sceneLook;             /* the target comes from a scene's LOOKAT event */
	f32 senseNext, lookSenseNext; /* when it next checks whether it sees the player (combat, look-at) */
	u8 senseSeen, lookSenseSeen;
	f32 deathPush[3];
	/* the weapon in its hand (additionalequipment, GiveWeapon): its class, drawn bone-merged */
	char held[28];
	s16 heldItem;             /* weapons.hgm item for it, -2 not looked up yet, -1 none */
	/* fighting: the activities for its weapon (-1 none), the fire gesture's progress, until when it's
	 * in combat stance */
	s16 actAngry, actFire, actGest, actRunAim, actWalkAim;
	u8 actsReady, fireBase;
	f32 fireT, fireDur, combatUntil, muzzleUntil;         /* the hit that killed it (world units/s): how its ragdoll starts moving */
	u8 train;                 /* npc_citizen riding a func_tracktrain: 1 on it, 2 waiting to get up, 3 getting up */
	f32 trainWait;
	f32 lookYaw, lookPitch, lookW;
	/* animation sound events (footsteps, AE_CL_PLAYSOUND): the sequence and cycle last tick */
	u32 sndSeq;
	f32 sndCycle;
} Npc;

int Npc_Spawn(const Pack *p, Player *player);
void Npc_Free(void);
void Npc_Tick(f32 dt);
void Npc_BonePose(const Npc *n, f32 (*out)[3][4]); /* (tests) */
/* index-based access (renderer) */
Npc *Npc_List(u32 *count);
/* the brain for an NPC entity made while playing (Ent_Clone) */
Npc *Npc_Add(s32 ent);
Npc *Npc_OfEntity(s32 entIndex);
/* Plays a named sequence (or ACT_ name). Returns its duration in seconds, or -1 if the model lacks it. */
f32 Npc_PlaySequence(Npc *n, const char *name, int loop);
/* props: plays an animation (SetAnimation) and returns to the default one after; asDefault sets it as the default */
void Npc_PropAnimation(Npc *n, const char *name, int asDefault);
/* Back to idle/walk/run chosen by movement. */
void Npc_ResetSequence(Npc *n);
/* Plays its death animation and stops thinking. Returns 0 if it has none (remove it instead). */
int Npc_Kill(Npc *n);
/* the weapon in its hand: GiveWeapon / additionalequipment ("" none) */
void Npc_SetWeapon(Npc *n, const char *cls);
/* fighting right now (combat stance) / its weapon just fired (muzzle flash) */
int Npc_InCombat(const Npc *n);
int Npc_MuzzleFlash(const Npc *n);
void Npc_MoveTo(Npc *n, const f32 pos[3], int run, f32 stopDist);
void Npc_Stop(Npc *n);
void Npc_FacePoint(Npc *n, const f32 pos[3]);
void Npc_Teleport(Npc *n, const f32 pos[3], f32 yaw);
/* CSoundEnt combat sounds (gunfire, explosions): NPCs within the radius turn to it; being hurt too */
f32 Npc_TalkingUntil(void); /* someone talking until (npc clock), 0 none */
void Npc_HearCombat(const f32 pos[3], f32 radius);
void Npc_Alert(Npc *n, f32 seconds);
void Npc_Follow(Npc *n, s32 ent, f32 dist);
/* Skinning matrices (model space) for the current pose: count = skel->numBones. */
void Npc_Pose(const Npc *n, f32 (*out)[3][4]);
/* skinning matrices for any skeleton in any pack (weapons use their own pack) */
void Anim_Pose(const Pack *pk, const PackSkel *sk, s16 seq, f32 t, int hold, s16 prevSeq, f32 prevT, f32 blend,
               f32 (*out)[3][4]);
void Anim_PoseEx(const Pack *pk, const PackSkel *sk, s16 seq, f32 t, int hold, s16 prevSeq, f32 prevT, f32 blend,
                 const AnimExtra *ex, f32 (*out)[3][4]);
/* skeleton-local sequence by name (or -1) */
s16 Npc_FindSeq(const Npc *n, const char *name);
/* sets/clears a scene gesture slot; returns the slot or -1 */
int Npc_SetGesture(Npc *n, int slot, s16 seq, f32 cycle, f32 weight, f32 time);
void Npc_ClearGestures(Npc *n);
/* the current sequence's cycle (0..1) and its global index in the pack's sequence table */
f32 Npc_Cycle(const Npc *n, u32 *globalSeq);
f32 Anim_SeqDuration(const Pack *pk, const PackSkel *sk, s16 seq);
s16 Anim_FindSeq(const Pack *pk, const PackSkel *sk, const char *name);
/* World position and rotation of a model attachment ("eyes", "mouth", "cameraeye"...). 0 if missing. */
int Npc_Attachment(const Npc *n, const char *name, f32 pos[3], f32 R[3][3]);
/* World position of an entity (the player is entity 0). */
void Npc_EntPos(s32 ent, f32 out[3]);
/* the point to look at on an entity: eyes for the player and NPCs, the origin for anything else */
void Npc_EyePos(s32 ent, f32 out[3]);

#endif
