/*
 * hl2gc player movement. Behaviour follows Half-Life 2's walk movement as described by the
 * Source SDK's game/shared/gamemovement.cpp, reimplemented in C with HL2's default values.
 */
#ifndef HL2GC_PLAYER_H
#define HL2GC_PLAYER_H

#include "pack.h"

#define IN_JUMP (1 << 0)
#define IN_DUCK (1 << 1)
#define IN_SPEED (1 << 2) /* sprint */
#define IN_WALK (1 << 3)

#define TICK_INTERVAL 0.015f /* HL2's 66.67 Hz simulation rate */

typedef struct {
	f32 forwardmove, sidemove; /* -1..1 from the stick */
	u32 buttons;
	f32 pitch, yaw;            /* view angles, degrees */
} UserCmd;

typedef struct {
	f32 origin[3];   /* feet */
	f32 velocity[3];
	f32 pitch, yaw;
	s32 onGround;
	s32 groundEnt;   /* the entity stood on (riding a train or a lift), -1 the world */
	f32 groundNormal[3];
	s32 ducked;      /* using the crouch hull */
	f32 duckAmount;  /* 0 standing .. 1 crouched (view height blend) */
	u32 oldButtons;
	f32 surfaceFriction;
	s32 noclip;
	s32 waterLevel;   /* 0 dry, 1 feet, 2 waist, 3 eyes under (Source's WL_*) */
	s32 waterType;    /* CONTENTS_WATER / CONTENTS_SLIME at the feet */
	f32 waterJumpTime;
	f32 waterJumpVel[2];
	s32 frozen;       /* in a vehicle: no movement */
	s32 ladder;       /* climbing this useable ladder (-1 none) */
	f32 ladderT;      /* 0 bottom .. 1 top */
	f32 ladderCooldown;
	s32 forceDuck;    /* trigger_playermovement "auto-duck" (ForceButtons IN_DUCK): crouched while inside */
} Player;

/* HL2's useable ladders (func_useableladder, func_ladderendpoint pairs): where the player's feet go at the
 * bottom and the top, and the info_ladder_dismount spots to step off to */
void Player_SetLadders(const f32 (*ends)[2][3], int count, const f32 (*dismounts)[3], int numDismounts);

/* Returns 1 if a free spot was found at or near feet. */
int Player_Spawn(Player *pl, const f32 feet[3], f32 pitch, f32 yaw);
void Player_Tick(Player *pl, const UserCmd *cmd);
f32 Player_EyeHeight(const Player *pl);
void Player_Bounds(const Player *pl, f32 mins[3], f32 maxs[3]);

/* player_speedmod: top speeds scaled by this (1 = normal) */
void Player_SetSpeedScale(f32 s);

#endif
