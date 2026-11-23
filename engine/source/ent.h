/*
 * hl2gc entities: Source's input/output system ("map scripts") and the entity classes the
 * campaign's maps rely on (logic, triggers, doors, buttons, brushes, props, teleports).
 *
 * NPCs (npc.c) act out scripted sequences and choreographed scenes (scene.c); ambient_generic plays
 * its sound through audio.c.
 */
#ifndef HL2GC_ENT_H
#define HL2GC_ENT_H

#include "pack.h"
#include "player.h"

enum {
	EC_OTHER = 0,
	EC_PLAYER,
	EC_LOGIC_AUTO,
	EC_LOGIC_RELAY,
	EC_LOGIC_TIMER,
	EC_MATH_COUNTER,
	EC_LOGIC_BRANCH,
	EC_LOGIC_COMPARE,
	EC_TRIGGER_ONCE,
	EC_TRIGGER_MULTIPLE,
	EC_TRIGGER_TELEPORT,
	EC_TRIGGER_PUSH,
	EC_TRIGGER_CHANGELEVEL,
	EC_POINT_TELEPORT,
	EC_FUNC_DOOR,
	EC_FUNC_DOOR_ROTATING,
	EC_FUNC_MOVELINEAR,
	EC_FUNC_BUTTON,
	EC_FUNC_BRUSH,
	EC_FUNC_WALL,
	EC_FUNC_ILLUSIONARY,
	EC_FUNC_BREAKABLE,
	EC_PROP,
	EC_PROP_DOOR,
	EC_SCENE,
	EC_SCRIPTED_SEQUENCE,
	EC_SCRIPT_CONDITIONS,
	EC_NPC,
	EC_AMBIENT,
	EC_FOLLOW_GOAL,
	EC_ACTBUSY,               /* ai_goal_actbusy */
	EC_ENV_GLOBAL,
	EC_ACTBUSY_QUEUE,         /* ai_goal_actbusy_queue */
	EC_TEMPLATE_MAKER,
	EC_POINT_CAMERA,
	EC_FUNC_MONITOR,
	EC_GAME_TEXT,
	EC_ENV_MESSAGE,
	EC_ENV_CREDITS,
	EC_HUDHINT,
	EC_MICROPHONE,
	EC_TRACKTRAIN,
	EC_PATH,
	EC_VIEWCONTROL,
	EC_FADE,
	EC_SHAKE,
	EC_ZOOM,
	EC_POINT_TEMPLATE,
	EC_WEAPON_STRIP,    /* trigger_weapon_strip / trigger_weapon_dissolve / player_weaponstrip */
	EC_HURT,            /* trigger_hurt */
	EC_SCRIPT_INTRO,
	EC_VEHICLE,         /* prop_vehicle_airboat / prop_vehicle_jeep / prop_vehicle_prisoner_pod */
	EC_ROTATING,        /* func_rotating */
	EC_SPRITE,          /* env_sprite / env_sprite_oriented / env_glow / point_spotlight */
	EC_LIGHTGLOW,       /* env_lightglow */
	EC_LIGHT,           /* light / light_spot with a name: switches its light style */
	EC_DYNLIGHT,        /* light_dynamic: a moving light for models (light.c) */
	EC_MOMENTARY,       /* momentary_rot_button: a valve wheel turned by holding use */
	EC_LOADSAVED,       /* player_loadsaved: fade out, then back to the last save (the level's start here) */
	EC_TRIGGER_LOOK,    /* trigger_look: fires when the player inside has looked at its target long enough */
	EC_TRIGGER_PLAYERMOVE, /* trigger_playermovement: crouches the player while inside (auto-duck) */
};

#define EF_DISABLED 1 /* not drawn, not solid, not touchable */
#define EF_KILLED 2
#define EF_SOLID 4    /* the player collides with it */
#define EF_DRAW 8     /* has a brush model or studio model to draw */
#define EF_FIRED 16   /* a once-only env_message has shown */
#define EF_TEMPLATE 32 /* an NPC maker's template (SF_NPC_TEMPLATE): never in the world, never what a name finds */

typedef struct Entity {
	u16 cls, flags;
	u32 firstKV, numKV;
	const char *classname, *name;
	u32 spawnflags;
	f32 origin[3], angles[3];
	f32 R[3][3];      /* rotation from angles (columns: forward, left, up) */
	s32 bmodel, mdl;  /* brush model "*N" / studio model index, -1 if none */
	u8 tint[4];

	/* movers: doors, buttons, movelinear */
	u8 moveState, locked, enabled, touching, blocked;
	u8 latch;         /* math_counter: bit 0 at its max, bit 1 at its min (OnHitMax/Min fire on reaching it) */
	f32 ajar; /* prop doors spawned ajar: how far open (0..1) */
	u8 spawnOpen; /* prop doors: spawned open (the "Starts Open" flag or spawnpos 1/2) */
	f32 frac, speed, wait, waitUntil, moveLen;
	f32 pos1[3], pos2[3], ang1[3], ang2[3];

	/* logic */
	f32 value, minv, maxv, compare;
	f32 nextThink, interval;
	u32 fired;
	u8 armed;

	/* NPCs, scenes, scripts, sounds */
	s32 npcModel;     /* PackNpcModel index, -1 if not an NPC */
	s32 scene;        /* PackScene index (logic_choreographed_scene), -1 none */
	s32 sound;        /* PackSound index (ambient_generic), -1 none */
	s32 actor;        /* scripted_sequence: NPC entity it drives; template maker: template entity */
	u8 phase;         /* scripted_sequence state */
	f32 phaseUntil;
	u32 voice;        /* playing sound handle */
	s16 doorSnd[3];   /* doors: move, stop, locked (PackSound index, -1 none) */
	s32 pathNode;     /* trains / cameras: path_track or path_corner heading to */
	s32 lastNode;     /* trains: the path_track it passed last (m_ppath) */
	f32 trainSpeed, maxSpeed;

	/* parenting (parentname / SetParent): moves with its parent, keeping its offset */
	s32 parent;
	s32 toucher;      /* triggers: who touched it last (the activator of OnEndTouch) */
	f32 localOrigin[3], localR[3][3];
	f32 localMove[3];
	char parentAttach[32];    /* SetParentAttachment: follows this attachment of its NPC parent */
	f32 scriptCycle;          /* scripted_sequence: the NPC's cycle last tick (animation events) */
	s32 nextCine;             /* scripted_sequence: the script queued after this one (m_hNextCine), -1 none */
	s16 cineDelay;            /* scripted_sequence: m_iDelay, the same-named scripts not yet at their marks */
	u8 cineWait;              /* scripted_sequence: waiting for BeginSequence (m_bWaitForBeginSequence) */
	u8 cineReady;             /* scripted_sequence: at its mark and enabled (TASK_ENABLE_SCRIPT done) */
	u8 cineStarted;           /* scripted_sequence: the action has begun (m_startTime != 0) */
	u8 hinged;                /* a func_physbox on a vertical phys_hinge (turnstiles): turns when pushed */
	f32 hingeVel, hingePivot[3];         /* a parented sliding door: its open-closed travel in the parent's frame */

	/* sprites: render color/alpha, scale, texture (pack index, -1 none), rendermode */
	u8 color[4], rendermode;
	s16 tex, tex2; /* tex2: point_spotlight's beam */
	f32 beamLen, beamWidth;
	f32 scale, glowSize[2], glowDist[3]; /* env_lightglow: size h/v; min, max, outer max distance */
	f32 seen; /* glow fade 0..1 (renderer) */

	/* moving things' sounds (trains, func_movelinear, func_rotating, buttons): start, move loop, stop */
	s16 esnd[3];
	f32 fadeMax;      /* fademaxdist: not drawn beyond this (0 = no limit) */
	u8 sndMoving;
	u32 sndLoop;
	u32 sndStart;  /* the moving sound (noise1, MoveSound): stopped when it stops, like Source */
} Entity;

typedef struct {
	void (*changeLevel)(const char *map, const char *landmark);
	/* trigger_weapon_strip etc.: dissolve = keep the gravity gun and supercharge it */
	void (*stripWeapons)(int dissolve);
	/* trigger_hurt and explosions hurting the player */
	void (*hurtPlayer)(f32 damage);
	/* the same with a damage type (DMG_*) and where it came from, used instead when set */
	void (*hurtPlayerFrom)(f32 damage, u32 type, const f32 *from);
	/* a radiation trigger_hurt this far from the player (Geiger counter) */
	void (*geiger)(f32 range);
} EntCallbacks;

/* Parses the pack's entities, sets up every class and fires logic_auto. */
int Ent_Spawn(const Pack *p, Player *player, int newGame, const EntCallbacks *cb);
void Ent_Free(void);
/* Advances one simulation tick: movers, timers, trigger touches, delayed outputs. */
void Ent_Tick(f32 dt);
/* The player pressed "use" looking along fwd from eye. */
void Ent_Use(const f32 eye[3], const f32 fwd[3]);
void Ent_WorldBounds(s32 idx, f32 mn[3], f32 mx[3]);
int Ent_HingePush(s32 idx, const f32 pos[3], f32 vx, f32 vy);
Entity *Ent_List(u32 *count);
/* Rebuilds the list of entities the player collides with (for collide.c). */
void Ent_UpdateCollision(void);

void Ent_AngleMatrix(const f32 angles[3], f32 R[3][3]);

/* for npc.c / scene.c */
const char *Ent_KV(s32 ent, const char *key);
void Ent_FireOutput(s32 ent, const char *output, s32 activator);
/* the same, passing value as the parameter of the outputs that have none (OutValue and the like) */
void Ent_FireOutputValue(s32 ent, const char *output, s32 activator, const char *value);
/* sends an input straight to an entity (tests, debugging) */
void Ent_Input(s32 ent, const char *in, const char *param, s32 activator);
/* Source's global states (env_global; kept across level changes): -1 unknown, 0 off, 1 on, 2 dead */
int Ent_GlobalState(const char *name);
void Ent_ResetGlobals(void); /* new game */
/* damage the player from somewhere (NPC attacks) */
void Ent_HurtPlayer(f32 damage, u32 type, const f32 from[3]);
/* 1 on the title screen's background map: NPCs ignore the player and nothing hurts him */
void Ent_SetNoTarget(int on);
int Ent_NoTarget(void);
int Ent_PlayerVisible(const f32 from[3], f32 *dist);
/* An actor named in a scene or script: NPC targetname first, then any targetname, then classname. */
void Ent_PrepareCarry(const char *landmark, const f32 from[3]); /* NPCs through a level change */
void Ent_FinishCarry(const f32 to[3]);
s32 Ent_FindActor(const char *name);
/* the next path_track / path_corner along (backward: the previous); -1 at the end or a disabled one */
s32 Ent_PathNext(s32 node, int backward);
/* use held on something that takes continuous use (a momentary_rot_button), once a tick */
void Ent_UseHeld(s32 ent, s32 activator);
/* a player_loadsaved has asked for the game to go back to its last save */
int Ent_ReloadRequested(void);
/* An NPC at pos walking along dir bumped into something: open a door there if it can. Returns 1 if one opens. */
int Ent_NpcOpenDoor(s32 npcEnt, const f32 pos[3], const f32 dir[3]);
f32 Ent_Now(void);
const Pack *Ent_Pack(void); /* the map */
/* a new entity copied from src (its keys, model, flags) at origin/angles, named name; npcModel >= 0 makes
 * it that NPC. -1 if there's no room. (npc_maker, template makers, env_entity_maker) */
s32 Ent_Clone(s32 src, const char *classname, const f32 origin[3], const f32 angles[3], const char *name, s32 npcModel);
/* env_microphone -> env_speaker relays for a sound made at src: fills up to max speaker positions
 * (nearest the player first). Returns how many. */
int Ent_SpeakerRelays(const f32 src[3], f32 (*out)[3], int max);
/* origin of an entity (any index) */
const f32 *Ent_Origin(s32 ent);

/* Damage from the player's weapons (or explosions): NPCs die, breakables and crates break, props
 * get knocked. dir: direction of the hit (may be NULL), at: point hit (may be NULL). */
void Ent_Damage(s32 ent, f32 damage, const f32 dir[3], const f32 at[3]);
int Ent_IsProp(s32 ent); /* a live prop or func_breakable */
/* Entities hurt by an explosion (radius damage, falling off with distance, blocked by walls). */
void Ent_RadiusDamage(const f32 at[3], f32 damage, f32 radius);
/* world-space bounds of an entity's model as placed; story characters that ignore damage */
void Ent_Bounds(s32 ent, f32 mins[3], f32 maxs[3]);
int Ent_Protected(s32 ent);
/* the NPC is in an act-busy goal's hands (ai_goal_actbusy) */
int Ent_NpcActBusy(s32 npcEnt);
/* the "use" target the player is looking at (USE_RANGE), any class; -1 none */
s32 Ent_UseTarget(const f32 eye[3], const f32 fwd[3], f32 range);

/* Cutscene camera (point_viewcontrol) if one is on: eye position and angles. Player input is frozen. */
int Ent_ViewOverride(f32 origin[3], f32 angles[3]);
/* script_intro (the G-Man intro): a second view from a camera entity, combined with the normal view by
 * up to two blend passes (Source's intro_screenspaceeffect modes 0-9), plus its own fade. */
typedef struct {
	int hasCamera;
	f32 camOrigin[3], camAngles[3];
	f32 fov;          /* field of view for the camera view, 0 = the player's */
	int passes;
	int mode[2];
	f32 alpha[2];
	f32 fade[4];      /* rgb 0..255, alpha 0..255 */
} IntroView;
int Ent_IntroView(IntroView *v);
/* how far trains, lifts and doors carried the player in the last Ent_Tick */
void Ent_PlayerCarry(f32 out[3]);

/* Screen fade (env_fade): rgba for this frame, alpha 0 = none */
u32 Ent_ScreenFade(void);
void Ent_PlayerView(f32 eye[3], f32 fwd[3]);
void Ent_FlashScreen(u8 r, u8 g, u8 b, f32 alpha, f32 fadeTime); /* alpha 0..1 */
/* Screen shake (env_shake) amplitude at the player, in units */
f32 Ent_ShakeAmount(void);
void Ent_ShakeAdd(f32 amp, f32 duration);
/* env_zoom: horizontal field of view for this frame (0 = default) */
f32 Ent_ZoomFov(void);
/* The point_camera the monitors near the player show, if any: position, angles and fov. */
int Ent_ActiveCamera(f32 origin[3], f32 angles[3], f32 *fov);

#endif
