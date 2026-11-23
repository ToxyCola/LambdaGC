/*
 * hl2gc vehicles: the airboat (prop_vehicle_airboat), the buggy (prop_vehicle_jeep) and the Citadel's
 * prisoner pods (prop_vehicle_prisoner_pod).
 *
 * Driving is an arcade version of Source's vehicle physics with HL2's numbers: the buggy rides on four
 * wheels that follow the ground (35 mph, 50 with the turbo, which recharges for 15 s); the airboat
 * planes on water and slides on land, steering with its fan at any speed. Both push physics objects
 * aside and run NPCs over. You get in and out with use; the vehicle you leave a level in comes along
 * to the next one, like on PC.
 *
 * Controls when driving: stick = throttle/steer, C-stick = look, B = turbo, A = handbrake, X = get out.
 */
#ifndef HL2GC_VEHICLE_H
#define HL2GC_VEHICLE_H

#include "ent.h"
#include "pack.h"
#include "player.h"

void Veh_LevelStart(const Pack *p, Player *player);
/* before a level change: remember the vehicle being driven, to bring it along */
void Veh_PrepareCarry(const f32 landmarkFrom[3]);
/* after the new level spawned (landmark position in the new level) */
void Veh_FinishCarry(const f32 landmarkTo[3]);
/* inputs to a vehicle entity (Lock, Unlock, EnterVehicle, ExitVehicle, ...) */
void Veh_Input(Entity *e, const char *in, const char *param);
/* the player used this vehicle: get in. Returns 1 if they did. */
int Veh_TryEnter(s32 ent);
/* once per frame while driving: stick -1..1, look offsets in degrees, buttons below */
#define VB_TURBO 1
#define VB_HANDBRAKE 2
#define VB_EXIT 4
#define VB_FIRE 8
/* the mounted gun is on (and its heat, 0..1, for the HUD) */
int Veh_GunState(f32 *heat);
void Veh_Controls(f32 throttle, f32 steer, f32 lookYaw, f32 lookPitch, u32 buttons);
void Veh_Tick(f32 dt);
/* the entity being driven or ridden, -1 none */
s32 Veh_Driving(void);
/* the driver's eye and view angles; returns 0 when not in a vehicle */
int Veh_View(f32 eye[3], f32 *pitch, f32 *yaw, f32 *roll);

#endif
