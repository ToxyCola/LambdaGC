/*
 * hl2gc weapons, items and the player's health and suit, as in Half-Life 2.
 *
 * Models, animations and sounds come from hl2gc/weapons.hgm (built from the install's viewmodels,
 * world models and item models), which stays loaded across levels. The inventory carries over
 * level changes like on PC; a new game starts empty, and the maps hand out the suit and weapons
 * through their own scripts (item_suit, weapon_* pickups, point_template "newgame" spawners).
 *
 * Controls: R = fire (throws what you're carrying), L = secondary fire, Y = reload,
 *           D-pad left/right = previous/next weapon, D-pad down = last weapon.
 */
#ifndef HL2GC_WEAPONS_H
#define HL2GC_WEAPONS_H

#include "pack.h"
#include "player.h"
#include "collide.h"
#ifndef HOST_TEST
#include <gccore.h>
#include "world.h"
#endif

enum { WFX_EXPLOSION, WFX_IMPACT, WFX_SPARK, WFX_MUZZLE };
/* effects for the map's effect entities (envents.c) */
#define EXPL_NOFIREBALL 1
#define EXPL_NOSMOKE 2
#define EXPL_NOSOUND 4
#define EXPL_NOSPARKS 8
void Weap_Sparks(const f32 at[3], const f32 *dir, int magnitude, int trail);
void Weap_Dust(const f32 at[3], const f32 *dir, f32 scale, u32 rgb, f32 speed, int count);
void Weap_Splash(const f32 at[3], f32 scale);
void Weap_Explosion(const f32 at[3], f32 scale, int flags);
void Weap_Muzzle(const f32 at[3], f32 scale);
/* a bullet hitting something: the surface's decal and its dust / sparks / blood (tr from the shot) */

void Weap_BulletImpact(const Trace *tr, const f32 dir[3]);
void Weap_ClearImpacts(void);
f32 Weap_PlayerHealth(void);
/* items in the air: thrown by a physexplosion, dropped by a dying NPC (falls, bounces, then can be picked up) */
void Weap_ItemImpulse(s32 ent, const f32 vel[3]);
int Weap_DropItem(const char *cls, const f32 pos[3], const f32 vel[3]);
int Weap_ImpactCounts(int *numDecals);

/* named sounds in weapons.hgm's WSND table (tools/weapons.py EXTRA_SOUNDS, same order) */
enum { X_PC_PICKUP, X_PC_DROP, X_PC_LAUNCH, X_PC_DRY, X_PC_HEAVY, X_CROWBAR_SWING, X_CROWBAR_WORLD, X_EXPLODE,
       X_ROCKET, X_PICKUP_WEAPON, X_DENY, X_FLASHLIGHT, X_BREAK_WOOD, X_BREAK_GLASS, X_BREAK_METAL,
       X_BREAK_CONCRETE, X_SPLASH, X_DROWN, X_AIRBOAT_START, X_AIRBOAT_IDLE, X_AIRBOAT_THROTTLE, X_AIRBOAT_STOP,
       X_AIRBOAT_IMPACT, X_JEEP_START, X_JEEP_IDLE, X_JEEP_THROTTLE, X_JEEP_STOP, X_JEEP_IMPACT, X_JEEP_TURBO,
       X_GEIGER_HIGH, X_GEIGER_LOW, X_WATER_STEP_L, X_WATER_STEP_R, X_WADE_STEP_L, X_WADE_STEP_R, X_SWIM,
       X_NPC_PISTOL, X_NPC_SMG1, X_NPC_AR2, X_NPC_SHOTGUN, X_NPC_STUNSTICK, X_ZOMBIE_HIT, X_HEADCRAB_BITE,
       X_ANTLION_HIT, X_JEEP_CANNON, X_JEEP_CANNON_CHARGED, X_AIRBOAT_GUN, NUM_X };
/* plays one of them (pos NULL = heard everywhere); returns the audio handle */
u32 Weap_PlaySound(int which, const f32 *pos, f32 vol);
void Weap_VehicleShot(const f32 from[3], const f32 dir[3], f32 damage);

#define WB_FIRE 1
#define WB_ALT 2
#define WB_RELOAD 4
#define WB_NEXT 8
#define WB_PREV 16
#define WB_LAST 32
#define WB_USE 64

int Weap_Load(u8 *data, u32 size);
int Weap_Loaded(void);
/* new game: no suit, no weapons, full health */
void Weap_NewGame(void);
/* after a map's entities spawn / before they're freed */
void Weap_LevelStart(const Pack *map, Player *pl);
void Weap_LevelEnd(void);
/* after dying: the inventory the level started with, full health (main reloads the map) */
void Weap_Respawn(void);
/* once per frame: buttons held and newly pressed (WB_*), and the view */
void Weap_Input(u32 held, u32 down, const f32 eye[3], f32 pitch, f32 yaw, int frozen);
/* once per simulation tick */
void Weap_Tick(f32 dt);
/* the tables, already found in a loaded pack (Weap_Load uses it; the host tests call it directly) */
int Weap_Setup(const Pack *p, const void *weapRecs, u32 numWeapRecs, const void *itemRecs, u32 numItemRecs,
               const s16 *wsnd, u32 numWsnd, const s16 *ammx, u32 numAmmx);
#ifndef HOST_TEST
/* items lying in the world, thrown grenades, rockets, effects (inside the 3D pass) */
void Weap_DrawWorld(Mtx view, const Camera *cam);
/* the gun in your hands; uses its own field of view, then restores proj */
void Weap_DrawViewmodel(Mtx view, const Camera *cam, f32 proj[4][4], u16 width, u16 height);
/* HUD: health, suit, ammo, crosshair, weapon selection; between Text_Begin and Text_End */
void Weap_DrawHud(u16 width, u16 height);
/* red flash when hurt, rgba (alpha 0 = none) */
#endif
u32 Weap_DamageFlash(void);

void Weap_Effect(int fx, const f32 at[3]);
/* a prop/crate broke (item_item_crate drops its items) */
void Weap_Broke(s32 ent, const f32 at[3]);
void Weap_Strip(int dissolve);
/* the sound of something breaking: -1 wood, -2 glass, -3 metal, -4 concrete */
void Weap_BreakSound(int material, const f32 at[3]);
/* surface impact sounds (scripts/surfaceproperties): kind 0 hard impact, 1 soft, 2 bullet, 3 scrape;
 * surf: a surface id (trace's surface, Weap_EntSurface), -1 = default */
void Weap_SurfaceSound(int surf, int kind, const f32 at[3], f32 vol);
int Weap_EntSurface(s32 ent);
void Weap_Hurt(f32 damage);
/* Source damage types (DMG_*) the suit reacts to */
#define DMG_CRUSH 1
#define DMG_BULLET 2
#define DMG_SLASH 4
#define DMG_BURN 8
#define DMG_FALL 32
#define DMG_BLAST 64
#define DMG_CLUB 128
#define DMG_SHOCK 256
#define DMG_SONIC 512
#define DMG_DROWN 16384
#define DMG_PARALYZE 32768
#define DMG_NERVEGAS 65536
#define DMG_POISON 131072
#define DMG_RADIATION 262144
#define DMG_ACID 1048576
#define DMG_SLOWBURN 2097152
/* damage with a type (DMG_*) and where it came from (NULL = nowhere in particular): the HUD's
 * damage direction indicator and the suit's voice use them */
void Weap_HurtFrom(f32 damage, u32 type, const f32 *from);
/* a radiation trigger_hurt this far away: the Geiger counter clicks (CBasePlayer::m_flgeigerRange) */
void Weap_GeigerRange(f32 range);
/* weapons.hgm's HSEN chunk: the suit's sentences */
void Weap_SetSentences(const u8 *c, u32 size);
/* weapons.hgm's STEP chunk: footstep sounds per surface */
void Weap_SetSteps(const u8 *c, u32 size);
/* seconds since the player died, 0 = alive */
f32 Weap_DeadTime(void);
int Weap_HasSuit(void);
f32 Weap_DebugHealth(void);
int Weap_DebugAux(void);

/* HEV suit: sprint needs the suit and aux power (call each frame; returns whether to sprint),
 * the flashlight (D-pad up) drains it too, and so does breathing under water */
int Weap_SprintAllowed(int wantSprint, int moving);
void Weap_ToggleFlashlight(void);
int Weap_FlashlightOn(void);

/* cheats */
void Weap_GiveAll(void);
typedef struct {
	int noclip, god, infiniteAmmo;
} Cheats;
extern Cheats gCheats;

#endif
