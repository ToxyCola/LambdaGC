#include "aients.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collide.h"
#include "ent.h"
#include "npc.h"
#include "physics.h"
#include "player.h"
#include "relations.h"
#include "weapons.h"

static f32 frand(void) { return (f32)(rand() & 0xFFFF) / 65535.0f; }
static Entity *E(s32 i) { return Ent_List(NULL) + i; }
static const char *kv(s32 ent, const char *k)
{
	const char *v = Ent_KV(ent, k);
	return v ? v : "";
}
static f32 kvf(s32 ent, const char *k, f32 def)
{
	const char *v = Ent_KV(ent, k);
	return v && *v ? (f32)atof(v) : def;
}
static int kvi(s32 ent, const char *k, int def)
{
	const char *v = Ent_KV(ent, k);
	return v && *v ? atoi(v) : def;
}
static f32 dist2(const f32 *a, const f32 *b)
{
	f32 d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
	return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}
static int alive_npc(s32 ent)
{
	if (ent <= 0)
		return 0;
	Entity *e = E(ent);
	if (e->flags & (EF_KILLED | EF_DISABLED))
		return 0;
	Npc *n = Npc_OfEntity(ent);
	return n && !n->dead;
}

/* names, wildcards, classnames and the ! names (Source's FindEntityGeneric) */
static int matches(s32 i, const char *spec, s32 activator)
{
	if (!spec || !*spec)
		return 0;
	if (spec[0] == '!') {
		if (!strcasecmp(spec, "!player"))
			return i == 0;
		if (!strcasecmp(spec, "!activator"))
			return i == activator;
		return 0;
	}
	Entity *e = E(i);
	if (e->flags & EF_KILLED)
		return 0;
	if (e->name) {
		size_t n = strlen(spec);
		if (n && spec[n - 1] == '*' ? !strncasecmp(e->name, spec, n - 1) : !strcasecmp(e->name, spec))
			return 1;
	}
	return e->classname && !strcasecmp(e->classname, spec);
}

static s32 find_first(const char *spec, s32 activator)
{
	u32 n;
	Ent_List(&n);
	for (u32 i = 0; i < n; i++)
		if (matches((s32)i, spec, activator))
			return (s32)i;
	return -1;
}

/* a point on the floor under p (and the hull fits there) */
static int ground_at(f32 p[3])
{
	f32 up[3] = {p[0], p[1], p[2] + 32}, dn[3] = {p[0], p[1], p[2] - 512};
	f32 mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 64};
	Trace tr;
	CM_BoxTrace(&tr, up, dn, mn, mx, MASK_PLAYERSOLID);
	if (tr.startsolid || tr.fraction >= 1)
		return 0;
	memcpy(p, tr.endpos, 12);
	return 1;
}

static int player_sees(const f32 p[3])
{
	f32 d;
	const f32 *eye = E(0)->origin;
	(void)eye;
	f32 at[3] = {p[0], p[1], p[2] + 32};
	return Ent_PlayerVisible(at, &d);
}

/* ================================================================== makers */
#define MAX_CHILDREN 32
typedef struct {
	s32 ent;
	u8 kind; /* MK_* */
	u8 on;
	int made, maxMade, maxLive;
	f32 freq, next;
	s32 template_;  /* the entity it copies */
	const char *tname; /* its name: the copies take it (the template itself gives it up, so scripts find them) */
	s32 kids[MAX_CHILDREN];
	int numKids;
	u8 allDeadFired;
	/* antlion maker: its pool */
	int pool, poolMax, poolRegen;
	f32 poolRegenTime, poolNext;
} Maker;
enum { MK_NPC = 1, MK_TEMPLATE, MK_ANTLION };
static Maker *makers;
static int numMakers;

static Maker *maker_of(s32 ent)
{
	for (int i = 0; i < numMakers; i++)
		if (makers[i].ent == ent)
			return &makers[i];
	return NULL;
}

static int live_kids(Maker *m)
{
	int n = 0;
	for (int i = 0; i < m->numKids; i++)
		if (alive_npc(m->kids[i]))
			n++;
	return n;
}

/* CBaseNPCMaker::CanMakeNPC: not over the live limit, and (hidden makers) not in the player's view */
static int can_make(Maker *m, const f32 at[3])
{
	if (m->maxLive > 0 && live_kids(m) >= m->maxLive)
		return 0;
	if ((E(m->ent)->spawnflags & 128) && player_sees(at)) /* SF_NPCMAKER_HIDEFROMPLAYER */
		return 0;
	/* someone standing on the spot */
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 0; i < n; i++) {
		if (i && (!alive_npc((s32)i) || Npc_OfEntity((s32)i)->prop))
			continue; /* (an animated prop, a keypad on the wall, isn't in the way) */
		const f32 *o = ents[i].origin;
		if (fabsf(o[0] - at[0]) < 34 && fabsf(o[1] - at[1]) < 34 && fabsf(o[2] - at[2]) < 72)
			return 0;
	}
	return 1;
}

static int depleted(Maker *m)
{
	return !(E(m->ent)->spawnflags & 32) && m->made >= m->maxMade; /* SF_NPCMAKER_INF_CHILD */
}

static s32 make_one(Maker *m, const f32 at[3], f32 yaw, s32 activator)
{
	(void)activator;
	if (depleted(m))
		return -1;
	Entity *me = E(m->ent);
	f32 pos[3] = {at[0], at[1], at[2]};
	f32 ang[3] = {0, yaw, 0};
	s32 src = m->kind == MK_NPC ? m->ent : m->template_;
	if (src <= 0)
		return -1;
	const char *cls = m->kind == MK_NPC ? kv(m->ent, "npctype") : NULL;
	const char *nm = kv(m->ent, "npctargetname");
	if (!*nm && m->kind != MK_NPC)
		nm = m->tname ? m->tname : E(src)->name ? E(src)->name : "";
	s32 model = m->kind == MK_NPC ? kvi(m->ent, "_npc", -1) : E(src)->npcModel;
	if (model < 0)
		return -1;
	/* (a classname string that outlives this: the pack's own) */
	s32 c = Ent_Clone(src, cls && *cls ? cls : NULL, pos, ang, nm, model);
	if (c < 0)
		return -1;
	if (cls && *cls) {
		/* the pack keeps the NPCType string: find it among the entities' classnames, or use the maker's
		 * key (a pack string too) */
		E(c)->classname = Ent_KV(m->ent, "npctype");
	}
	Npc *n = Npc_OfEntity(c);
	if (n) {
		Npc_Teleport(n, pos, yaw);
		n->onGround = 0;
		const char *eq = kv(m->ent, "additionalequipment");
		if (*eq && strcmp(eq, "0") && strncasecmp(eq, "none", 4))
			Npc_SetWeapon(n, eq);
	}
	const char *rel = kv(m->ent, "relationship");
	if (*rel)
		Rel_SetRelationship(c, rel);
	m->made++;
	if (m->numKids < MAX_CHILDREN)
		m->kids[m->numKids++] = c;
	else {
		/* forget a dead one to make room */
		for (int i = 0; i < m->numKids; i++)
			if (!alive_npc(m->kids[i])) {
				m->kids[i] = c;
				break;
			}
	}
	m->allDeadFired = 0;
	Ent_FireOutput(m->ent, "onspawnnpc", c);
	if (depleted(m)) {
		Ent_FireOutput(m->ent, "onallspawned", c);
		if (!(me->spawnflags & 32))
			m->on = 0;
	}
	return c;
}

/* where a template maker puts its next one: its own spot, a random spot in its radius, or the nearest
 * info_npc_spawn_destination of its group the player can't see (CTemplateNPCMaker::MakeNPC*) */
static int template_spot(Maker *m, int radius, f32 out[3], f32 *yaw)
{
	Entity *me = E(m->ent);
	memcpy(out, me->origin, 12);
	*yaw = me->angles[1];
	const char *grp = kv(m->ent, "destinationgroup");
	if (*grp && !radius) {
		u32 n;
		Entity *ents = Ent_List(&n);
		s32 best = -1;
		f32 bestD = 1e18f;
		int byDist = kvi(m->ent, "criteriondistance", 0); /* 1: closest to the player, 2: farthest */
		int vis = kvi(m->ent, "criterionvisibility", 2);  /* 0: must be seen, 1: not seen, 2: don't care */
		f32 minD = kvf(m->ent, "minspawndistance", 0);
		for (u32 i = 1; i < n; i++) {
			if (strcmp(ents[i].classname, "info_npc_spawn_destination") || !ents[i].name || strcasecmp(ents[i].name, grp))
				continue;
			f32 d = dist2(ents[i].origin, ents[0].origin);
			if (minD > 0 && d < minD * minD)
				continue;
			if (vis != 2 && player_sees(ents[i].origin) != (vis == 0))
				continue;
			f32 score = byDist == 1 ? d : byDist == 2 ? -d : frand();
			if (score < bestD) {
				bestD = score;
				best = (s32)i;
			}
		}
		if (best < 0)
			return 0;
		memcpy(out, ents[best].origin, 12);
		*yaw = ents[best].angles[1];
		Ent_FireOutput(best, "onspawnnpc", m->ent);
		return 1;
	}
	if (radius || (me->spawnflags & 256)) { /* SF_NPCMAKER_ALWAYSUSERADIUS */
		f32 r = kvf(m->ent, "radius", 256);
		for (int t = 0; t < 8; t++) {
			f32 a = frand() * 6.2831853f, d = frand() * r;
			f32 p[3] = {me->origin[0] + cosf(a) * d, me->origin[1] + sinf(a) * d, me->origin[2]};
			if (ground_at(p) && can_make(m, p)) {
				memcpy(out, p, 12);
				*yaw = frand() * 360;
				return 1;
			}
		}
		return 0;
	}
	return 1;
}

static void maker_spawn(Maker *m, s32 activator, int radius)
{
	f32 at[3], yaw;
	if (m->kind == MK_ANTLION) {
		/* CAntlionTemplateMaker: they dig up somewhere in the spawn radius, out of the pool */
		if (m->pool <= 0 && m->poolMax > 0)
			return;
		f32 r = kvf(m->ent, "spawnradius", 256);
		Entity *me = E(m->ent);
		for (int t = 0; t < 8; t++) {
			f32 a = frand() * 6.2831853f, d = frand() * r;
			f32 p[3] = {me->origin[0] + cosf(a) * d, me->origin[1] + sinf(a) * d, me->origin[2]};
			if (ground_at(p) && can_make(m, p)) {
				if (make_one(m, p, frand() * 360, activator) >= 0) {
					if (m->poolMax > 0)
						m->pool--;
					f32 up[3] = {0, 0, 1};
					Weap_Dust(p, up, 2, 0x70604800u, 60, 6); /* the dirt it comes up through */
				}
				return;
			}
		}
		return;
	}
	if (m->kind == MK_TEMPLATE) {
		if (!template_spot(m, radius, at, &yaw))
			return;
	} else {
		memcpy(at, E(m->ent)->origin, 12);
		yaw = E(m->ent)->angles[1];
	}
	if (!can_make(m, at))
		return;
	make_one(m, at, yaw, activator);
}

static void makers_spawn(void)
{
	u32 n;
	Entity *ents = Ent_List(&n);
	int count = 0;
	for (u32 i = 1; i < n; i++)
		if (!strcmp(ents[i].classname, "npc_maker") || !strcmp(ents[i].classname, "npc_template_maker") ||
		    !strcmp(ents[i].classname, "npc_antlion_template_maker"))
			count++;
	makers = count ? calloc((size_t)count, sizeof(Maker)) : NULL;
	numMakers = 0;
	for (u32 i = 1; i < n && makers; i++) {
		const char *cn = ents[i].classname;
		int kind = !strcmp(cn, "npc_maker") ? MK_NPC : !strcmp(cn, "npc_template_maker") ? MK_TEMPLATE
		         : !strcmp(cn, "npc_antlion_template_maker") ? MK_ANTLION : 0;
		if (!kind)
			continue;
		Maker *m = &makers[numMakers++];
		m->ent = (s32)i;
		m->kind = (u8)kind;
		m->maxMade = kvi((s32)i, "maxnpccount", 1);
		m->maxLive = kvi((s32)i, "maxlivechildren", 0);
		m->freq = kvf((s32)i, "spawnfrequency", 0);
		m->on = kvi((s32)i, "startdisabled", 0) == 0;
		m->next = m->freq > 0 ? m->freq : 0;
		m->template_ = -1;
		if (kind != MK_NPC) {
			s32 t = find_first(kv((s32)i, "templatename"), -1);
			if (t > 0) {
				m->template_ = t;
				ents[t].flags |= EF_DISABLED | EF_TEMPLATE; /* the template itself never plays, nor answers to names */
				m->tname = ents[t].name;
				ents[t].name = NULL; /* (else "alyx" finds the hidden template, not the Alyx it made) */
			}
		}
		if (kind == MK_ANTLION) {
			m->pool = kvi((s32)i, "poolstart", 0);
			m->poolMax = kvi((s32)i, "poolmax", 0);
			m->poolRegen = kvi((s32)i, "poolregenamount", 0);
			m->poolRegenTime = kvf((s32)i, "poolregentime", 0);
		}
	}
}

static void makers_tick(f32 dt)
{
	f32 now = Ent_Now();
	for (int i = 0; i < numMakers; i++) {
		Maker *m = &makers[i];
		if (E(m->ent)->flags & EF_KILLED)
			continue;
		/* children died: the outputs */
		if (m->numKids && !m->allDeadFired && live_kids(m) == 0) {
			m->allDeadFired = 1;
			Ent_FireOutput(m->ent, "onalllivechildrendead", m->ent);
			if (depleted(m))
				Ent_FireOutput(m->ent, "onallspawneddead", m->ent);
		}
		if (m->kind == MK_ANTLION && m->poolRegenTime > 0 && now >= m->poolNext) {
			m->poolNext = now + m->poolRegenTime;
			m->pool += m->poolRegen;
			if (m->poolMax > 0 && m->pool > m->poolMax)
				m->pool = m->poolMax;
		}
		/* spawning on its own (SpawnFrequency > 0; -1 only on the Spawn input) */
		if (!m->on || m->freq <= 0 || depleted(m))
			continue;
		m->next -= dt;
		if (m->next <= 0) {
			m->next = m->freq;
			maker_spawn(m, m->ent, 0);
		}
	}
}

static int maker_input(s32 ent, const char *in, const char *param, s32 activator)
{
	Maker *m = maker_of(ent);
	if (!m)
		return 0;
	if (!strcasecmp(in, "spawn") || !strcasecmp(in, "spawnnpc") || !strcasecmp(in, "forcespawn"))
		maker_spawn(m, activator, 0);
	else if (!strcasecmp(in, "spawnnpcinradius"))
		maker_spawn(m, activator, 1);
	else if (!strcasecmp(in, "spawnnpcinline"))
		maker_spawn(m, activator, 0);
	else if (!strcasecmp(in, "spawnmultiple")) {
		int k = atoi(param);
		for (int i = 0; i < k; i++)
			maker_spawn(m, activator, 0);
	} else if (!strcasecmp(in, "enable"))
		m->on = 1;
	else if (!strcasecmp(in, "disable"))
		m->on = 0;
	else if (!strcasecmp(in, "toggle"))
		m->on = !m->on;
	else if (!strcasecmp(in, "setmaxchildren"))
		m->maxMade = atoi(param), m->made = 0;
	else if (!strcasecmp(in, "addmaxchildren"))
		m->maxMade += atoi(param);
	else if (!strcasecmp(in, "setmaxlivechildren"))
		m->maxLive = atoi(param);
	else if (!strcasecmp(in, "setspawnfrequency"))
		m->freq = (f32)atof(param);
	else if (!strcasecmp(in, "addtopool"))
		m->pool += atoi(param);
	else if (!strcasecmp(in, "setmaxpool"))
		m->poolMax = atoi(param);
	else if (!strcasecmp(in, "setpoolregenamount"))
		m->poolRegen = atoi(param);
	else if (!strcasecmp(in, "setpoolregentime"))
		m->poolRegenTime = (f32)atof(param);
	else
		return !strcasecmp(in, "changedestinationgroup") || !strcasecmp(in, "setminimumspawndistance") ||
		       !strcasecmp(in, "setspawnradius") || !strcasecmp(in, "setfighttarget") ||
		       !strcasecmp(in, "clearfighttarget") || !strcasecmp(in, "setfollowtarget") ||
		       !strcasecmp(in, "clearfollowtarget");
	return 1;
}

int AiEnts_MakerChildren(s32 maker, int *live)
{
	Maker *m = maker_of(maker);
	if (!m)
		return -1;
	if (live)
		*live = live_kids(m);
	return m->made;
}

/* ================================================================== point_template / env_entity_maker */
static u8 *templateSpawned; /* per point_template: its originals have come out once */

static void template_copy(s32 pt, const f32 at[3], const f32 ang[3], int relative, s32 activator, s32 owner)
{
	Entity *t = E(pt);
	f32 R[3][3];
	Ent_AngleMatrix(ang, R);
	int any = 0;
	for (int k = 1; k <= 16; k++) {
		char key[16];
		snprintf(key, sizeof(key), "template%02d", k);
		const char *nm = kv(pt, key);
		if (!*nm)
			continue;
		u32 n;
		Entity *ents = Ent_List(&n);
		for (u32 j = 1; j < n; j++) {
			if (!ents[j].name || strcasecmp(ents[j].name, nm))
				continue;
			f32 o[3], a[3];
			if (relative) {
				/* placed as it sits relative to the template, around the maker */
				f32 d[3] = {ents[j].origin[0] - t->origin[0], ents[j].origin[1] - t->origin[1], ents[j].origin[2] - t->origin[2]};
				for (int r = 0; r < 3; r++)
					o[r] = at[r] + R[r][0] * d[0] + R[r][1] * d[1] + R[r][2] * d[2];
				a[0] = ents[j].angles[0];
				a[1] = ents[j].angles[1] + ang[1] - t->angles[1];
				a[2] = ents[j].angles[2];
			} else {
				memcpy(o, ents[j].origin, 12);
				memcpy(a, ents[j].angles, 12);
			}
			s32 c = Ent_Clone((s32)j, NULL, o, a, ents[j].name, ents[j].npcModel);
			if (c > 0) {
				any = 1;
				Ent_FireOutput(owner, "onentityspawned", c);
			}
			break;
		}
	}
	if (!any)
		Ent_FireOutput(owner, "onentityfailedspawn", activator);
	Ent_FireOutput(pt, "onentityspawned", activator);
}

static int template_input(s32 ent, const char *in, const char *param, s32 activator)
{
	Entity *e = E(ent);
	if (!strcmp(e->classname, "point_template")) {
		if (strcasecmp(in, "forcespawn"))
			return 0;
		if (!templateSpawned[ent]) {
			templateSpawned[ent] = 1;
			return 0; /* the first time: the originals come out where they are (ent.c) */
		}
		template_copy(ent, e->origin, e->angles, 0, activator, ent);
		return 1;
	}
	if (!strcmp(e->classname, "env_entity_maker")) {
		s32 pt = find_first(kv(ent, "entitytemplate"), activator);
		if (pt <= 0)
			return 0;
		if (!strcasecmp(in, "forcespawn")) {
			template_copy(pt, e->origin, e->angles, 1, activator, ent);
		} else if (!strcasecmp(in, "forcespawnatentityorigin")) {
			s32 at = find_first(param, activator);
			if (at < 0)
				return 1;
			template_copy(pt, E(at)->origin, E(at)->angles, 1, activator, ent);
		} else
			return 0;
		return 1;
	}
	return 0;
}

/* ================================================================== scripted schedules, assaults, lead */
#define MAX_ORDERS 64
enum { OR_MOVE = 1, OR_PATH, OR_RALLY, OR_RALLY_WAIT, OR_ASSAULT, OR_ASSAULT_WAIT, OR_LEAD };
typedef struct {
	s32 npc, goal, owner, enemy;
	u8 kind, run, done;
	f32 until;
	s32 point; /* assault: the rally / assault point now */
} Order;
static Order orders[MAX_ORDERS];
static int numOrders;
static u8 *schedFired; /* per aiscripted_schedule: has run once */

static Order *order_of(s32 npc)
{
	for (int i = 0; i < numOrders; i++)
		if (orders[i].npc == npc && !orders[i].done)
			return &orders[i];
	return NULL;
}

static Order *new_order(s32 npc, int kind, s32 owner)
{
	Order *o = order_of(npc);
	if (!o) {
		for (int i = 0; i < numOrders && !o; i++)
			if (orders[i].done)
				o = &orders[i];
		if (!o && numOrders < MAX_ORDERS)
			o = &orders[numOrders++];
	}
	if (!o)
		return NULL;
	memset(o, 0, sizeof(*o));
	o->npc = npc;
	o->kind = (u8)kind;
	o->owner = owner;
	o->enemy = -1;
	return o;
}

static void order_go(Order *o, s32 goal)
{
	o->goal = goal;
	Npc *n = Npc_OfEntity(o->npc);
	if (n && goal >= 0)
		Npc_MoveTo(n, E(goal)->origin, o->run, 16);
}

static int arrived(Order *o)
{
	Npc *n = Npc_OfEntity(o->npc);
	if (!n || o->goal < 0)
		return 0;
	return !n->moving && dist2(E(o->npc)->origin, E(o->goal)->origin) < 64 * 64;
}

s32 AiEnts_ForcedEnemy(s32 npcEnt)
{
	Order *o = order_of(npcEnt);
	return o && o->enemy >= 0 && !(E(o->enemy)->flags & EF_KILLED) ? o->enemy : -1;
}

static void schedule_start(s32 ss, s32 activator)
{
	int sched = kvi(ss, "schedule", 0);
	const char *who = kv(ss, "m_iszentity");
	f32 radius = kvf(ss, "m_flradius", 0);
	int all = kvi(ss, "graball", 0);
	s32 goal = find_first(kv(ss, "goalent"), activator);
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		if (!matches((s32)i, who, activator) || !alive_npc((s32)i))
			continue;
		if (radius > 0 && dist2(ents[i].origin, ents[ss].origin) > radius * radius)
			continue;
		Npc *np = Npc_OfEntity((s32)i);
		if (np && (np->script >= 0 || np->locked))
			continue; /* (not interruptable: in a scripted_sequence) */
		/* 1 walk / 2 run to goal, 3 enemy is goal, 4 walk / 5 run a path, 6 enemy is goal and run to it */
		if (sched == 3 || sched == 6) {
			Order *o = new_order((s32)i, OR_MOVE, ss);
			if (o) {
				o->enemy = goal;
				o->run = 1;
				if (sched == 6 && goal >= 0)
					order_go(o, goal);
				else
					o->goal = -1;
			}
		} else if (sched == 1 || sched == 2) {
			Order *o = new_order((s32)i, OR_MOVE, ss);
			if (o) {
				o->run = sched == 2;
				order_go(o, goal);
			}
		} else if (sched == 4 || sched == 5) {
			Order *o = new_order((s32)i, OR_PATH, ss);
			if (o) {
				o->run = sched == 5;
				order_go(o, goal);
			}
		}
		if (!all)
			break;
	}
	schedFired[ss] = 1;
}

static void schedule_stop(s32 ss)
{
	for (int i = 0; i < numOrders; i++)
		if (orders[i].owner == ss && !orders[i].done) {
			Npc *n = Npc_OfEntity(orders[i].npc);
			if (n)
				Npc_Stop(n);
			orders[i].done = 1;
		}
}

/* ---- assaults: to the rally point, wait for the cue, then from assault point to assault point */
static void assault_activate(s32 goal, s32 activator)
{
	const char *who = kv(goal, "actor");
	s32 rally = find_first(kv(goal, "rallypoint"), activator);
	if (rally < 0)
		return;
	u32 n;
	Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		if (!matches((s32)i, who, activator) || !alive_npc((s32)i))
			continue;
		Order *o = new_order((s32)i, OR_RALLY, goal);
		if (!o)
			continue;
		o->run = 1;
		o->point = rally;
		order_go(o, rally);
	}
}

static void assault_begin(s32 goal)
{
	for (int i = 0; i < numOrders; i++) {
		Order *o = &orders[i];
		if (o->done || o->owner != goal || (o->kind != OR_RALLY_WAIT && o->kind != OR_RALLY))
			continue;
		o->until = Ent_Now() + kvf(o->point, "assaultdelay", 0);
		o->kind = OR_RALLY_WAIT;
		o->goal = -2; /* cue given: off when the delay is up */
	}
}

static void to_assault_point(Order *o, s32 ap)
{
	o->kind = OR_ASSAULT;
	o->point = ap;
	o->run = 1;
	order_go(o, ap);
}

static void orders_tick(void)
{
	f32 now = Ent_Now();
	for (int i = 0; i < numOrders; i++) {
		Order *o = &orders[i];
		if (o->done)
			continue;
		if (!alive_npc(o->npc)) {
			o->done = 1;
			continue;
		}
		Npc *n = Npc_OfEntity(o->npc);
		switch (o->kind) {
		case OR_MOVE:
			if (o->goal >= 0 && arrived(o)) {
				o->goal = -1;
				if (o->enemy < 0)
					o->done = 1;
			}
			if (o->enemy >= 0 && (E(o->enemy)->flags & EF_KILLED))
				o->done = 1;
			break;
		case OR_PATH:
			if (arrived(o)) {
				Ent_FireOutput(o->goal, "onpass", o->npc);
				s32 next = find_first(kv(o->goal, "target"), o->npc);
				if (next > 0 && next != o->goal)
					order_go(o, next);
				else
					o->done = 1;
			}
			break;
		case OR_RALLY:
			if (arrived(o)) {
				Ent_FireOutput(o->point, "onarrival", o->npc);
				o->kind = OR_RALLY_WAIT;
				/* AssaultCue 3: don't wait */
				if (kvi(o->owner, "assaultcue", 1) == 3) {
					o->until = now + kvf(o->point, "assaultdelay", 0);
					o->goal = -2;
				} else
					o->goal = -1;
				if (n)
					n->idealYaw = E(o->point)->angles[1];
			}
			break;
		case OR_RALLY_WAIT:
			if (o->goal == -2 && now >= o->until) {
				s32 ap = find_first(kv(o->point, "assaultpoint"), o->npc);
				if (ap > 0)
					to_assault_point(o, ap);
				else
					o->done = 1;
			}
			break;
		case OR_ASSAULT:
			if (arrived(o)) {
				Ent_FireOutput(o->point, "onarrival", o->npc);
				o->kind = OR_ASSAULT_WAIT;
				f32 to = kvf(o->point, "assaulttimeout", 3);
				o->until = kvi(o->point, "nevertimeout", 0) ? 1e30f : now + (to > 0 ? to : 3);
			}
			break;
		case OR_ASSAULT_WAIT: {
			/* clear: its time is up, or (clear on contact) it has an enemy in sight */
			int contact = kvi(o->point, "clearoncontact", 0) && n && n->enemy >= 0;
			int noEnemy = n && n->enemy < 0;
			if (contact || (now >= o->until && (noEnemy || !kvi(o->point, "nevertimeout", 0)))) {
				Ent_FireOutput(o->point, "onassaultclear", o->npc);
				s32 next = find_first(kv(o->point, "nextassaultpoint"), o->npc);
				if (next > 0)
					to_assault_point(o, next);
				else
					o->done = 1;
			}
		} break;
		case OR_LEAD: {
			/* ai_goal_lead: go on while the player keeps up, wait for him otherwise */
			f32 wait = kvf(o->owner, "waitdistance", 256);
			int near = dist2(E(0)->origin, E(o->npc)->origin) < (wait + 64) * (wait + 64);
			if (n && !near && n->moving)
				Npc_Stop(n);
			else if (n && near && !n->moving && !arrived(o))
				order_go(o, o->goal);
			if (arrived(o)) {
				Ent_FireOutput(o->owner, "onarrival", o->npc);
				if (dist2(E(0)->origin, E(o->goal)->origin) < (wait + 64) * (wait + 64)) {
					Ent_FireOutput(o->owner, "onsuccess", o->npc);
					o->done = 1;
				}
			}
		} break;
		}
	}
}

/* ================================================================== logic and the rest */
typedef struct {
	s32 ent;
	u8 kind, on;
	f32 next;
	u8 seen;
	f32 dmgNext;
	s32 last; /* logic_case PickRandomShuffle: (bit set of used cases) */
} Misc;
enum {
	MI_CASE = 1, MI_REMAP, MI_MEASURE, MI_ENEMYFINDER, MI_REMOVE, MI_PHYSTRAP, MI_VMOTION, MI_WATERDEATH,
	MI_WIND, MI_SPEEDMOD, MI_SCHEDULE, MI_ASSAULT, MI_LEAD, MI_NOOP
};
static Misc *misc;
static int numMisc;
static f32 speedScale = 1;

static int misc_kind(const char *cn)
{
	static const struct {
		const char *n;
		u8 k;
	} map[] = {
		{"logic_case", MI_CASE},
		{"math_remap", MI_REMAP},
		{"logic_measure_movement", MI_MEASURE},
		{"npc_enemyfinder", MI_ENEMYFINDER},
		{"trigger_remove", MI_REMOVE},
		{"trigger_physics_trap", MI_PHYSTRAP},
		{"trigger_vphysics_motion", MI_VMOTION},
		{"trigger_waterydeath", MI_WATERDEATH},
		{"trigger_wind", MI_WIND},
		{"player_speedmod", MI_SPEEDMOD},
		{"aiscripted_schedule", MI_SCHEDULE},
		{"ai_goal_assault", MI_ASSAULT},
		{"ai_goal_lead", MI_LEAD},
		{"ai_goal_lead_weapon", MI_LEAD},
		/* nothing to do on the GameCube, but their inputs are understood (no "unknown input") */
		{"logic_autosave", MI_NOOP},
		{"trigger_autosave", MI_NOOP},
		{"logic_achievement", MI_NOOP},
		{"ai_changehintgroup", MI_NOOP},
		{"ai_speechfilter", MI_NOOP},
		{"game_weapon_manager", MI_NOOP},
		{"game_ragdoll_manager", MI_NOOP},
		{"game_gib_manager", MI_NOOP},
		{"point_clientcommand", MI_NOOP},
		{"point_servercommand", MI_NOOP},
		{"logic_collision_pair", MI_NOOP},
		{"ai_goal_standoff", MI_NOOP},
		{"ai_goal_police", MI_NOOP},
		{"ai_battle_line", MI_NOOP},
		{"ai_ally_manager", MI_NOOP},
		{"ai_citizen_response_system", MI_NOOP},
		{"ai_sound", MI_NOOP},
		{"info_node_link_controller", MI_NOOP},
		{"phys_ragdollmagnet", MI_NOOP},
		{"info_target_gunshipcrash", MI_NOOP},
		{"filter_damage_type", MI_NOOP},
		{"trigger_transition", MI_NOOP},
		{"material_modify_control", MI_NOOP},
		{"info_lighting_relative", MI_NOOP},
		{"env_player_surface_trigger", MI_NOOP},
		{"point_enable_motion_fixup", MI_NOOP},
		{"math_colorblend", MI_NOOP},
		{"info_teleporter_countdown", MI_NOOP},
	};
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(cn, map[i].n))
			return map[i].k;
	return 0;
}

static Misc *misc_of(s32 ent)
{
	int lo = 0, hi = numMisc - 1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (misc[m].ent == ent)
			return &misc[m];
		if (misc[m].ent < ent)
			lo = m + 1;
		else
			hi = m - 1;
	}
	return NULL;
}

/* is p inside this brush entity's box */
static int inside(s32 ent, const f32 p[3])
{
	f32 mn[3], mx[3];
	Ent_WorldBounds(ent, mn, mx);
	return p[0] >= mn[0] && p[0] <= mx[0] && p[1] >= mn[1] && p[1] <= mx[1] && p[2] >= mn[2] && p[2] <= mx[2];
}

static void case_value(s32 ent, const char *v, s32 activator)
{
	for (int k = 1; k <= 16; k++) {
		char key[16], out[16];
		snprintf(key, sizeof(key), "case%02d", k);
		const char *c = Ent_KV(ent, key);
		if (c && *c && !strcasecmp(c, v)) {
			snprintf(out, sizeof(out), "oncase%02d", k);
			Ent_FireOutput(ent, out, activator);
			return;
		}
	}
	Ent_FireOutputValue(ent, "ondefault", activator, v);
}

static void case_random(Misc *m, s32 activator, int shuffle)
{
	int have[16], n = 0;
	for (int k = 1; k <= 16; k++) {
		char key[16];
		snprintf(key, sizeof(key), "case%02d", k);
		const char *c = Ent_KV(m->ent, key);
		/* (PickRandom picks among the cases that have outputs: here, the ones with a value or an output) */
		char out[16];
		snprintf(out, sizeof(out), "oncase%02d", k);
		if ((c && *c) || Ent_KV(m->ent, out))
			if (!shuffle || !(m->last & (1 << k)))
				have[n++] = k;
	}
	if (!n && shuffle) {
		m->last = 0;
		case_random(m, activator, 1);
		return;
	}
	if (!n)
		return;
	int k = have[rand() % n];
	if (shuffle)
		m->last |= 1 << k;
	char out[16];
	snprintf(out, sizeof(out), "oncase%02d", k);
	Ent_FireOutput(m->ent, out, activator);
}

static void remap(s32 ent, f32 v, s32 activator)
{
	f32 in1 = kvf(ent, "in1", 0), in2 = kvf(ent, "in2", 1), o1 = kvf(ent, "out1", 0), o2 = kvf(ent, "out2", 1);
	u32 sf = E(ent)->spawnflags;
	if ((sf & 1) && (v < fminf(in1, in2) || v > fmaxf(in1, in2)))
		return; /* SF_MATH_REMAP_IGNORE_OUT_OF_RANGE */
	if (sf & 2) /* SF_MATH_REMAP_CLAMP_OUTPUT_TO_RANGE */
		v = v < fminf(in1, in2) ? fminf(in1, in2) : v > fmaxf(in1, in2) ? fmaxf(in1, in2) : v;
	f32 r = in2 != in1 ? o1 + (v - in1) * (o2 - o1) / (in2 - in1) : o1;
	char s[24];
	snprintf(s, sizeof(s), "%g", r);
	Ent_FireOutputValue(ent, "outvalue", activator, s);
}

f32 AiEnts_PlayerSpeed(void) { return speedScale; }

static void misc_tick(f32 dt)
{
	f32 now = Ent_Now();
	u32 n;
	Entity *ents = Ent_List(&n);
	for (int i = 0; i < numMisc; i++) {
		Misc *m = &misc[i];
		Entity *e = &ents[m->ent];
		if (e->flags & EF_KILLED)
			continue;
		switch (m->kind) {
		case MI_MEASURE: {
			/* logic_measure_movement: the target moves by how the measured entity moved from the reference,
			 * scaled, relative to the target reference */
			if (!m->on)
				break;
			s32 me = find_first(kv(m->ent, "measureentity"), -1), mr = find_first(kv(m->ent, "measurereference"), -1);
			s32 te = find_first(kv(m->ent, "target"), -1), tr = find_first(kv(m->ent, "targetreference"), -1);
			if (me < 0 || mr < 0 || te <= 0 || tr < 0)
				break;
			f32 sc = kvf(m->ent, "targetscale", 1);
			for (int k = 0; k < 3; k++)
				ents[te].origin[k] = ents[tr].origin[k] + (ents[me].origin[k] - ents[mr].origin[k]) * sc;
			ents[te].angles[1] = ents[tr].angles[1] + ents[me].angles[1] - ents[mr].angles[1];
			Ent_AngleMatrix(ents[te].angles, ents[te].R);
		} break;
		case MI_ENEMYFINDER: {
			if (now < m->next || !m->on)
				break;
			m->next = now + 0.25f;
			f32 d;
			f32 maxd = kvf(m->ent, "maxsearchdist", 2048);
			int sees = Rel_Disposition(m->ent, 0) == REL_HATE && dist2(e->origin, ents[0].origin) < maxd * maxd &&
			           Ent_PlayerVisible(e->origin, &d);
			if (sees && !m->seen) {
				Ent_FireOutput(m->ent, "onfoundenemy", 0);
				Ent_FireOutput(m->ent, "onfoundplayer", 0);
			} else if (!sees && m->seen) {
				Ent_FireOutput(m->ent, "onlostenemylos", 0);
				Ent_FireOutput(m->ent, "onlostplayerlos", 0);
			}
			m->seen = (u8)sees;
		} break;
		case MI_REMOVE:
			/* trigger_remove: what comes into it is removed (not the player) */
			if (!m->on)
				break;
			for (u32 k = 1; k < n; k++) {
				if ((s32)k == m->ent || (ents[k].flags & EF_KILLED) || !(ents[k].npcModel >= 0 || ents[k].mdl >= 0))
					continue;
				if (inside(m->ent, ents[k].origin)) {
					Ent_FireOutput(m->ent, "onremove", (s32)k);
					ents[k].flags |= EF_KILLED;
				}
			}
			break;
		case MI_PHYSTRAP:
			/* trigger_physics_trap: physics objects that come in are dissolved */
			if (!m->on)
				break;
			for (u32 k = 1; k < n; k++)
				if (!(ents[k].flags & EF_KILLED) && Phys_IsBody((s32)k) && Phys_HeldEnt() != (s32)k &&
				    inside(m->ent, ents[k].origin)) {
					f32 up[3] = {0, 0, 1};
					Weap_Sparks(ents[k].origin, up, 2, 1);
					ents[k].flags |= EF_KILLED;
				}
			break;
		case MI_VMOTION: {
			/* trigger_vphysics_motion: gravity scaled and a push on what's inside */
			if (!m->on)
				break;
			f32 g = kvf(m->ent, "setgravityscale", 1), lim = kvf(m->ent, "setvelocitylimit", 0);
			f32 lin[3] = {0, 0, 0};
			sscanf(kv(m->ent, "setlinearforce") , "%f", &lin[2]);
			(void)lim;
			for (u32 k = 1; k < n; k++)
				if (!(ents[k].flags & EF_KILLED) && Phys_IsBody((s32)k) && inside(m->ent, ents[k].origin)) {
					f32 mass = Phys_Mass((s32)k);
					f32 imp[3] = {0, 0, (600.0f * (1 - g) + lin[2]) * mass * dt};
					Phys_Impulse((s32)k, ents[k].origin, imp);
				}
		} break;
		case MI_WATERDEATH:
			/* the leeches (CTriggerWateryDeath): in the deep water the player is bitten, harder and harder */
			if (inside(m->ent, ents[0].origin)) {
				if (now >= m->dmgNext) {
					m->dmgNext = now + 1.0f;
					m->last++;
					Ent_HurtPlayer(2.0f * (f32)(m->last > 10 ? 10 : m->last), DMG_SLASH, ents[0].origin);
				}
			} else
				m->last = 0;
			break;
		case MI_WIND:
			if (!m->on)
				break;
			for (u32 k = 1; k < n; k++)
				if (!(ents[k].flags & EF_KILLED) && Phys_IsBody((s32)k) && inside(m->ent, ents[k].origin)) {
					f32 sp = kvf(m->ent, "speed", 200), a = e->angles[1] * (f32)M_PI / 180;
					f32 imp[3] = {cosf(a) * sp * Phys_Mass((s32)k) * dt, sinf(a) * sp * Phys_Mass((s32)k) * dt, 0};
					Phys_Impulse((s32)k, ents[k].origin, imp);
				}
			break;
		default:
			break;
		}
	}
	(void)dt;
}

static int misc_input(s32 ent, const char *in, const char *param, s32 activator)
{
	Misc *m = misc_of(ent);
	if (!m)
		return 0;
	switch (m->kind) {
	case MI_CASE:
		if (!strcasecmp(in, "invalue"))
			case_value(ent, param, activator);
		else if (!strcasecmp(in, "pickrandom"))
			case_random(m, activator, 0);
		else if (!strcasecmp(in, "pickrandomshuffle"))
			case_random(m, activator, 1);
		else
			return 0;
		return 1;
	case MI_REMAP:
		if (!strcasecmp(in, "invalue")) {
			if (m->on)
				remap(ent, (f32)atof(param), activator);
		} else if (!strcasecmp(in, "enable"))
			m->on = 1;
		else if (!strcasecmp(in, "disable"))
			m->on = 0;
		else
			return 0;
		return 1;
	case MI_SPEEDMOD:
		if (!strcasecmp(in, "modifyspeed")) {
			speedScale = (f32)atof(param);
			if (speedScale <= 0)
				speedScale = 1;
			Player_SetSpeedScale(speedScale);
			return 1;
		}
		return 0;
	case MI_SCHEDULE:
		if (!strcasecmp(in, "startschedule")) {
			if (!schedFired[ent] || (E(ent)->spawnflags & 4)) /* SF_SCRIPT_REPEATABLE */
				schedule_start(ent, activator);
		} else if (!strcasecmp(in, "stopschedule"))
			schedule_stop(ent);
		else
			return 0;
		return 1;
	case MI_ASSAULT:
		if (!strcasecmp(in, "activate"))
			assault_activate(ent, activator);
		else if (!strcasecmp(in, "beginassault"))
			assault_begin(ent);
		else if (!strcasecmp(in, "deactivate"))
			schedule_stop(ent);
		else
			return 0;
		return 1;
	case MI_LEAD:
		if (!strcasecmp(in, "activate")) {
			s32 g = find_first(kv(ent, "goal"), activator);
			u32 n;
			Ent_List(&n);
			for (u32 i = 1; i < n && g >= 0; i++)
				if (matches((s32)i, kv(ent, "actor"), activator) && alive_npc((s32)i)) {
					Order *o = new_order((s32)i, OR_LEAD, ent);
					if (o) {
						o->run = kvi(ent, "run", 0);
						order_go(o, g);
					}
					break;
				}
		} else if (!strcasecmp(in, "deactivate"))
			schedule_stop(ent);
		else
			return !strcasecmp(in, "setsuccess");
		return 1;
	default:
		if (!strcasecmp(in, "enable") || !strcasecmp(in, "turnon") || !strcasecmp(in, "startmeasuring"))
			m->on = 1;
		else if (!strcasecmp(in, "disable") || !strcasecmp(in, "turnoff") || !strcasecmp(in, "stopmeasuring"))
			m->on = 0;
		else if (!strcasecmp(in, "toggle"))
			m->on = !m->on;
		else
			return m->kind == MI_NOOP; /* (Save, SetHintGroup, ... understood, nothing to do) */
		return 1;
	}
}

/* ================================================================== */
void AiEnts_Free(void)
{
	free(makers);
	makers = NULL;
	numMakers = 0;
	free(misc);
	misc = NULL;
	numMisc = 0;
	free(templateSpawned);
	templateSpawned = NULL;
	free(schedFired);
	schedFired = NULL;
	numOrders = 0;
	speedScale = 1;
	Player_SetSpeedScale(1);
}

void AiEnts_Spawn(void)
{
	AiEnts_Free();
	u32 n;
	Entity *ents = Ent_List(&n);
	templateSpawned = calloc(n, 1);
	schedFired = calloc(n, 1);
	makers_spawn();
	int count = 0;
	for (u32 i = 1; i < n; i++)
		if (misc_kind(ents[i].classname))
			count++;
	misc = count ? calloc((size_t)count, sizeof(Misc)) : NULL;
	for (u32 i = 1; i < n && misc; i++) {
		int k = misc_kind(ents[i].classname);
		if (!k)
			continue;
		Misc *m = &misc[numMisc++];
		m->ent = (s32)i;
		m->kind = (u8)k;
		m->on = kvi((s32)i, "startdisabled", 0) == 0;
		if (k == MI_MEASURE)
			m->on = 1;
		if (k == MI_ASSAULT && kvi((s32)i, "startactive", 0))
			assault_activate((s32)i, -1);
	}
}

void AiEnts_Tick(f32 dt)
{
	makers_tick(dt);
	orders_tick();
	misc_tick(dt);
}

int AiEnts_Input(s32 ent, const char *in, const char *param, s32 activator)
{
	if (maker_input(ent, in, param, activator))
		return 1;
	if (template_input(ent, in, param, activator))
		return 1;
	return misc_input(ent, in, param, activator);
}
