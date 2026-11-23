#include "relations.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ent.h"
#include "reltable.h"

/* each NPC class's Classify() */
static const struct {
	const char *cn;
	unsigned char c;
} classOf[] = {
	{"npc_alyx", CLASS_PLAYER_ALLY_VITAL},     {"npc_barney", CLASS_PLAYER_ALLY_VITAL},
	{"npc_kleiner", CLASS_PLAYER_ALLY_VITAL},  {"npc_eli", CLASS_PLAYER_ALLY_VITAL},
	{"npc_mossman", CLASS_PLAYER_ALLY_VITAL},  {"npc_monk", CLASS_PLAYER_ALLY_VITAL},
	{"npc_dog", CLASS_PLAYER_ALLY_VITAL},      {"npc_magnusson", CLASS_PLAYER_ALLY_VITAL},
	{"npc_fisherman", CLASS_PLAYER_ALLY_VITAL},{"npc_vortigaunt", CLASS_VORTIGAUNT},
	{"npc_combine_s", CLASS_COMBINE},          {"npc_combine", CLASS_COMBINE},
	{"npc_metropolice", CLASS_METROPOLICE},    {"npc_manhack", CLASS_MANHACK},
	{"npc_cscanner", CLASS_SCANNER},           {"npc_clawscanner", CLASS_SCANNER},
	{"npc_rollermine", CLASS_COMBINE},         {"npc_turret_floor", CLASS_COMBINE},
	{"npc_turret_ceiling", CLASS_COMBINE},     {"npc_turret_ground", CLASS_COMBINE},
	{"npc_combine_camera", CLASS_MILITARY},    {"npc_helicopter", CLASS_COMBINE_GUNSHIP},
	{"npc_combinegunship", CLASS_COMBINE_GUNSHIP}, {"npc_combinedropship", CLASS_COMBINE_GUNSHIP},
	{"npc_strider", CLASS_COMBINE_GUNSHIP},    {"npc_hunter", CLASS_COMBINE_HUNTER},
	{"npc_sniper", CLASS_PROTOSNIPER},         {"npc_stalker", CLASS_STALKER},
	{"npc_antlion", CLASS_ANTLION},            {"npc_antlionguard", CLASS_ANTLION},
	{"npc_headcrab", CLASS_HEADCRAB},          {"npc_headcrab_fast", CLASS_HEADCRAB},
	{"npc_headcrab_black", CLASS_HEADCRAB},    {"npc_headcrab_poison", CLASS_HEADCRAB},
	{"npc_zombie", CLASS_ZOMBIE},              {"npc_zombie_torso", CLASS_ZOMBIE},
	{"npc_fastzombie", CLASS_ZOMBIE},          {"npc_fastzombie_torso", CLASS_ZOMBIE},
	{"npc_poisonzombie", CLASS_ZOMBIE},        {"npc_zombine", CLASS_ZOMBIE},
	{"npc_barnacle", CLASS_BARNACLE},          {"npc_crow", CLASS_EARTH_FAUNA},
	{"npc_pigeon", CLASS_EARTH_FAUNA},         {"npc_seagull", CLASS_EARTH_FAUNA},
	{"npc_ichthyosaur", CLASS_ANTLION},        {"npc_bullseye", CLASS_BULLSEYE},
	{"npc_apcdriver", CLASS_COMBINE},          {"npc_vehicledriver", CLASS_METROPOLICE},
	{"npc_cranedriver", CLASS_NONE},           {"npc_launcher", CLASS_NONE},
	{"combine_mine", CLASS_NONE},
};

int Rel_Class(s32 ent)
{
	if (ent == 0)
		return CLASS_PLAYER;
	Entity *e = Ent_List(NULL) + ent;
	const char *cn = e->classname;
	if (!strcmp(cn, "npc_citizen")) {
		/* CNPC_Citizen::Classify: passive before Gordon's a fugitive (or told to be), an ally after */
		if (Ent_GlobalState("gordon_precriminal") == 1 || Ent_GlobalState("citizens_passive") == 1)
			return CLASS_CITIZEN_PASSIVE;
		return CLASS_PLAYER_ALLY;
	}
	for (unsigned i = 0; i < sizeof(classOf) / sizeof(classOf[0]); i++)
		if (!strcmp(cn, classOf[i].cn))
			return classOf[i].c;
	return CLASS_NONE;
}

/* ---- overrides: (subject entity) -> (target entity, or every entity of a class) */
#define MAX_OVERRIDES 512
typedef struct {
	s32 subject, target; /* target -1: by class */
	const char *targetClass;
	s32 owner;           /* the ai_relationship that set it (RevertRelationship), -1 an NPC input */
	unsigned char disp, prev;
} Override;
static Override ovr[MAX_OVERRIDES];
static int numOvr;

void Rel_Free(void) { numOvr = 0; }

static int disposition_default(s32 a, s32 b)
{
	int d = defaultRel[Rel_Class(a)][Rel_Class(b)];
	return d == D_ER ? REL_NEUTRAL : d;
}

int Rel_Disposition(s32 a, s32 b)
{
	const char *bc = b == 0 ? "player" : Ent_List(NULL)[b].classname;
	for (int i = numOvr - 1; i >= 0; i--) {
		const Override *o = &ovr[i];
		if (o->subject != a)
			continue;
		if (o->target == b || (o->target < 0 && o->targetClass && !strcasecmp(o->targetClass, bc)))
			return o->disp;
	}
	return disposition_default(a, b);
}

static void add(s32 subject, s32 target, const char *targetClass, int disp, s32 owner)
{
	/* replace one already there for the same pair */
	for (int i = 0; i < numOvr; i++)
		if (ovr[i].subject == subject && ovr[i].target == target &&
		    (target >= 0 || (ovr[i].targetClass && targetClass && !strcasecmp(ovr[i].targetClass, targetClass)))) {
			ovr[i].disp = (unsigned char)disp;
			ovr[i].owner = owner;
			return;
		}
	if (numOvr >= MAX_OVERRIDES)
		return;
	Override *o = &ovr[numOvr++];
	o->subject = subject;
	o->target = target;
	o->targetClass = targetClass;
	o->owner = owner;
	o->disp = (unsigned char)disp;
}

static int parse_disp(const char *s)
{
	if (!s)
		return REL_NEUTRAL;
	if (!strncasecmp(s, "D_HT", 4))
		return REL_HATE;
	if (!strncasecmp(s, "D_FR", 4))
		return REL_FEAR;
	if (!strncasecmp(s, "D_LI", 4))
		return REL_LIKE;
	if (!strncasecmp(s, "D_NU", 4))
		return REL_NEUTRAL;
	int v = atoi(s);
	return v >= REL_HATE && v <= REL_NEUTRAL ? v : REL_NEUTRAL;
}

static int matches(s32 i, const char *spec)
{
	if (!spec || !*spec)
		return 0;
	if (i == 0)
		return !strcasecmp(spec, "!player") || !strcasecmp(spec, "player");
	Entity *e = Ent_List(NULL) + i;
	if (e->flags & EF_KILLED)
		return 0;
	if (!strcasecmp(e->classname, spec))
		return 1;
	if (!e->name)
		return 0;
	size_t n = strlen(spec);
	if (n && spec[n - 1] == '*')
		return !strncasecmp(e->name, spec, n - 1);
	return !strcasecmp(e->name, spec);
}

/* a classname rather than a name: it applies to every one of them, now and later */
static int is_class_spec(const char *spec)
{
	if (!spec || !*spec || spec[0] == '!')
		return 0;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++)
		if (ents[i].name && !strcasecmp(ents[i].name, spec))
			return 0;
	return !strncasecmp(spec, "npc_", 4) || !strcasecmp(spec, "player");
}

static void apply_rel(s32 owner, int revert)
{
	const char *subj = Ent_KV(owner, "subject"), *targ = Ent_KV(owner, "target");
	int disp = parse_disp(Ent_KV(owner, "disposition"));
	const char *rs = Ent_KV(owner, "radius");
	f32 radius = rs ? (f32)atof(rs) : 0;
	const char *rc = Ent_KV(owner, "reciprocal");
	int recip = rc && atoi(rc);
	u32 n;
	Entity *ents = Ent_List(&n);
	const f32 *o = ents[owner].origin;
	int targClass = is_class_spec(targ), subjClass = is_class_spec(subj);
	for (u32 s = 0; s < n; s++) {
		if (!matches((s32)s, subj))
			continue;
		if (radius > 0) {
			const f32 *p = ents[s].origin;
			f32 d[3] = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
			if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > radius * radius)
				continue;
		}
		if (targClass) {
			if (revert) {
				for (int i = 0; i < numOvr; i++)
					if (ovr[i].owner == owner && ovr[i].subject == (s32)s)
						ovr[i] = ovr[--numOvr], i--;
			} else {
				add((s32)s, -1, !strcasecmp(targ, "player") ? "player" : targ, disp, owner);
			}
		}
		for (u32 t = 0; t < n && !targClass; t++) {
			if (t == s || !matches((s32)t, targ))
				continue;
			if (revert) {
				for (int i = 0; i < numOvr; i++)
					if (ovr[i].owner == owner && ((ovr[i].subject == (s32)s && ovr[i].target == (s32)t) ||
					                              (recip && ovr[i].subject == (s32)t && ovr[i].target == (s32)s)))
						ovr[i] = ovr[--numOvr], i--;
				continue;
			}
			add((s32)s, (s32)t, NULL, disp, owner);
			if (recip)
				add((s32)t, (s32)s, NULL, disp, owner);
		}
	}
	/* a reciprocal class relationship: each of the class back toward the subjects */
	if (recip && targClass && !revert && !subjClass)
		for (u32 t = 1; t < n; t++)
			if (matches((s32)t, targ))
				for (u32 s = 0; s < n; s++)
					if (matches((s32)s, subj))
						add((s32)t, (s32)s, NULL, disp, owner);
}

void Rel_Spawn(void)
{
	numOvr = 0;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		if (strcmp(ents[i].classname, "ai_relationship"))
			continue;
		const char *sa = Ent_KV((s32)i, "startactive");
		if (sa && atoi(sa))
			apply_rel((s32)i, 0);
	}
	/* npc_maker's / an NPC's own "Relationship" key ("target disposition rank") */
	for (u32 i = 1; i < n; i++) {
		const char *r = Ent_KV((s32)i, "relationship");
		if (r && *r && ents[i].npcModel >= 0)
			Rel_SetRelationship((s32)i, r);
	}
}

int Rel_Input(s32 ent, const char *in)
{
	if (strcmp(Ent_List(NULL)[ent].classname, "ai_relationship"))
		return 0;
	if (!strcasecmp(in, "applyrelationship"))
		apply_rel(ent, 0);
	else if (!strcasecmp(in, "revertrelationship") || !strcasecmp(in, "reverttodefaultrelationship"))
		apply_rel(ent, 1);
	else
		return 0;
	return 1;
}

void Rel_SetRelationship(s32 subject, const char *param)
{
	char targ[64], disp[16];
	if (!param || sscanf(param, "%63s %15s", targ, disp) != 2)
		return;
	int d = parse_disp(disp);
	if (is_class_spec(targ)) {
		/* (the string must outlive this call: classnames are the pack's strings or the literal) */
		u32 n;
		Entity *ents = Ent_List(&n);
		const char *keep = !strcasecmp(targ, "player") ? "player" : NULL;
		for (u32 i = 1; i < n && !keep; i++)
			if (!strcasecmp(ents[i].classname, targ))
				keep = ents[i].classname;
		if (keep)
			add(subject, -1, keep, d, -1);
		return;
	}
	u32 n;
	Ent_List(&n);
	for (u32 t = 0; t < n; t++)
		if ((s32)t != subject && matches((s32)t, targ))
			add(subject, (s32)t, NULL, d, -1);
}
