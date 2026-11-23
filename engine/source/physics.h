/*
 * hl2gc physics: prop_physics objects as rigid boxes (the model's bounds) with gravity, tumbling,
 * bounce and friction against the world, doors and other props. The player pushes them by walking
 * into them, and picks light ones up with use, like on PC (mass up to 35 kg).
 *
 * Contacts come from tracing the box's corners and face centres through the collision model each
 * tick and applying impulses at the hit points - simple, but it gives Source-like tumbling and
 * resting without Havok.
 */
#ifndef HL2GC_PHYSICS_H
#define HL2GC_PHYSICS_H

#include "pack.h"
#include "player.h"

int Phys_Init(const Pack *p, Player *pl);
void Phys_Free(void);
void Phys_Tick(f32 dt);
/* use on an entity: picks it up if it's a light physics prop. Returns 1 if it took it. */
int Phys_TryPickup(s32 ent);
int Phys_Holding(void);
void Phys_Drop(int throwIt);
/* the player's view, while holding something */
void Phys_SetView(const f32 eye[3], const f32 fwd[3]);
/* the player's intended horizontal velocity from the controls, so walking into a prop shoves it */
void Phys_SetWish(f32 vx, f32 vy);
void Phys_GetWish(f32 out[2]);
void Phys_Wake(s32 ent);
void Phys_SetMotion(s32 ent, int enabled);

/* gravity gun and weapons */
int Phys_IsBody(s32 ent);
int Phys_AwakeCount(void); /* (tests) */
int Phys_IsAwake(s32 ent);   /* (tests) */
int Phys_BodyEnts(s32 *out, int max); /* every simulated object's entity */
f32 Phys_Mass(s32 ent);
int Phys_HeldEnt(void);
int Phys_GrabFar(s32 ent, f32 maxMass);         /* hold it in front of the gun */
void Phys_Pull(s32 ent, const f32 eye[3], f32 strength);
void Phys_Launch(f32 impulse);                   /* punt what's held along the view */
void Phys_Impulse(s32 ent, const f32 point[3], const f32 imp[3]);
void Phys_Explosion(const f32 at[3], f32 radius, f32 force);
void Phys_Teleport(s32 ent, const f32 pos[3]);
const f32 *Phys_Com(s32 ent);
void Phys_SetAngles(s32 ent, const f32 angles[3]); /* turned, its origin kept */
/* a prop_physics / func_physbox made while playing (Ent_Clone) */
int Phys_AddBody(s32 ent);
/* constraints (phys_spring, phys_lengthconstraint, phys_ballsocket, phys_constraint): inputs to one
 * (TurnOn, TurnOff, Break; 2 = takes a value: Phys_ConstraintValue) */
int Phys_ConstraintInput(s32 ent, const char *in);
void Phys_ConstraintValue(s32 ent, const char *in, f32 v);
int Phys_ConstraintCount(void);
int Phys_ConstraintProbe(int i, f32 a[3], f32 b[3], f32 *len, int *on); /* (tests) type + 1, 0 if none */

#endif
