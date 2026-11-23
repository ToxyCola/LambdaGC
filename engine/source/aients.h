/*
 * hl2gc: the AI and logic entities the map scripts drive, after Source's
 *   npc_maker / npc_template_maker / npc_antlion_template_maker (monstermaker.cpp, npc_antlion_template_maker.cpp)
 *   point_template / env_entity_maker (point_template.cpp, env_entity_maker.cpp)
 *   aiscripted_schedule (scripted.cpp CAI_ScriptedSchedule)
 *   ai_goal_assault / assault_rallypoint / assault_assaultpoint (ai_behavior_assault.cpp)
 *   ai_goal_lead (ai_behavior_lead.cpp), npc_bullseye (npc_bullseye.cpp), npc_enemyfinder
 *   logic_case, math_remap, logic_measure_movement (logic*.cpp)
 *   trigger_remove, trigger_physics_trap, trigger_vphysics_motion, trigger_waterydeath, trigger_wind,
 *   player_speedmod, and the ones with nothing for the GameCube to do (autosave, achievements, hint groups...)
 */
#ifndef HL2GC_AIENTS_H
#define HL2GC_AIENTS_H

#include "pack.h"

void AiEnts_Spawn(void);
void AiEnts_Free(void);
int AiEnts_Input(s32 ent, const char *in, const char *param, s32 activator);
void AiEnts_Tick(f32 dt);
/* the enemy a script gave this NPC (aiscripted_schedule "enemy is goal"), -1 none */
s32 AiEnts_ForcedEnemy(s32 npcEnt);
/* player_speedmod: how fast the player may move (1 = normal) */
f32 AiEnts_PlayerSpeed(void);

/* tests */
int AiEnts_MakerChildren(s32 maker, int *live);

#endif
