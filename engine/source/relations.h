/*
 * hl2gc: who fights whom. Source's AI relationships: every NPC has a class (Classify()), the default
 * table between classes (hl2_gamerules.cpp InitDefaultAIRelationships), and overrides set while playing by
 * ai_relationship entities and NPCs' SetRelationship input (ai_relationship.cpp, CBaseCombatCharacter).
 */
#ifndef HL2GC_RELATIONS_H
#define HL2GC_RELATIONS_H

#include "pack.h"

enum { REL_ER, REL_HATE, REL_FEAR, REL_LIKE, REL_NEUTRAL };

void Rel_Spawn(void); /* after Ent_Spawn: the ai_relationships that start active */
void Rel_Free(void);
/* how entity a feels about entity b (0 = the player) */
int Rel_Disposition(s32 a, s32 b);
/* the ai_relationship's inputs (1 if handled) */
int Rel_Input(s32 ent, const char *in);
/* an NPC's SetRelationship input: "target disposition rank" (target a name, a classname or !player) */
void Rel_SetRelationship(s32 subject, const char *param);
/* entity a's Source class (CLASS_*, reltable.h) */
int Rel_Class(s32 ent);

#endif
