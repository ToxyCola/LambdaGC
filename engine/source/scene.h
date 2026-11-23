/*
 * hl2gc choreographed scenes (logic_choreographed_scene): the compiled .vcd timelines from the
 * player's scenes.image, played against the map's NPCs - speech with captions, sequences, moving
 * to marks, facing, trigger outputs, pause sections and loops.
 */
#ifndef HL2GC_SCENE_H
#define HL2GC_SCENE_H

#include "pack.h"
#include "player.h"

void Scene_Init(const Pack *p, Player *pl);
void Scene_Free(void);
void Scene_Start(s32 sceneEnt, u32 sceneIndex, s32 activator);
void Scene_Cancel(s32 sceneEnt);
void Scene_Pause(s32 sceneEnt);
void Scene_Resume(s32 sceneEnt);
int Scene_IsPlaying(s32 sceneEnt);
void Scene_Tick(f32 dt);
/* an NPC says a line on its own (response system): positional, captioned, lip-synced */
struct Npc;
void Scene_NpcSay(struct Npc *n, s16 sound);
/* in a scene or still talking */
int Scene_NpcBusy(const struct Npc *n);

#endif
