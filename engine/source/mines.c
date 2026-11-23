/*
 * hl2gc: hopper mines (combine_mine, combine_mine.cpp). Within BOUNCEBOMB_WARN_RADIUS of an enemy it opens its
 * hooks (warns); within BOUNCEBOMB_DETONATE_RADIUS it hops toward them and blows up ExplosionDelay later
 * (150 damage, radius 125). A citizen-modified mine (Modification 1) is friendly to the player and hunts Combine.
 */
#include "mines.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "collide.h"
#include "ent.h"
#include "npc.h"
#include "relations.h"
#include "weapons.h"

#define WARN_RADIUS 245.0f
#define DETONATE_RADIUS 100.0f
#define EXPLODE_RADIUS 125.0f
#define EXPLODE_DAMAGE 150.0f

enum { MS_DORMANT, MS_ARMED, MS_WARN, MS_HOP, MS_GONE };
typedef struct {
	s32 ent;
	u8 state, citizen, disarmed;
	f32 vel[3], boomAt, nextThink;
} Mine;
#define MAX_MINES 96
static Mine mn[MAX_MINES];
static int numMn;

static Entity *E(s32 i) { return Ent_List(NULL) + i; }
static void snd(Mine *m, int slot)
{
	const Pack *P = Ent_Pack();
	Entity *e = E(m->ent);
	if (P && e->esnd[slot] >= 0 && (u32)e->esnd[slot] < P->numSounds)
		Audio_Play(&P->sounds[e->esnd[slot]], e->origin, 1.0f, SNDLVL_DEFAULT);
}

void Mines_Spawn(void)
{
	numMn = 0;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n && numMn < MAX_MINES; i++) {
		if (strcmp(ents[i].classname, "combine_mine"))
			continue;
		Mine *m = &mn[numMn++];
		memset(m, 0, sizeof(*m));
		m->ent = (s32)i;
		const char *mod = Ent_KV((s32)i, "modification");
		m->citizen = mod && atoi(mod) == 1;
		const char *st = Ent_KV((s32)i, "startdisarmed");
		m->disarmed = st && atoi(st);
		m->state = m->disarmed ? MS_DORMANT : MS_ARMED;
	}
}

/* does this mine go off for that? (IsFriend: Combine mines spare Combine, citizen mines spare the rebels) */
static int enemy_of(Mine *m, s32 t)
{
	if (t == 0)
		return !m->citizen && !Ent_NoTarget();
	Npc *n = Npc_OfEntity(t);
	if (!n || n->dead || (E(t)->flags & (EF_KILLED | EF_DISABLED)))
		return 0;
	int combine = Rel_Disposition(t, 0) == REL_HATE && Rel_Class(t) != 0; /* (the player's enemies) */
	const char *cn = E(t)->classname;
	if (!strncmp(cn, "npc_manhack", 11) || !strncmp(cn, "npc_cscanner", 12) || !strcmp(cn, "combine_mine"))
		return 0; /* (flying, or another mine) */
	return m->citizen ? combine : !combine && Rel_Disposition(t, 0) == REL_LIKE;
}

static s32 nearest(Mine *m, f32 *dist)
{
	u32 n;
	Entity *ents = Ent_List(&n);
	const f32 *o = E(m->ent)->origin;
	s32 best = -1;
	f32 bd = WARN_RADIUS * WARN_RADIUS + 1;
	for (u32 i = 0; i < n; i++) {
		if (i && ents[i].npcModel < 0)
			continue;
		const f32 *p = i == 0 ? Ent_Origin(0) : ents[i].origin;
		f32 d[3] = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd >= bd || !enemy_of(m, (s32)i))
			continue;
		bd = dd;
		best = (s32)i;
	}
	*dist = sqrtf(bd);
	return best;
}

static void explode(Mine *m)
{
	Entity *e = E(m->ent);
	Weap_Explosion(e->origin, 1.0f, 0);
	Ent_RadiusDamage(e->origin, EXPLODE_DAMAGE, EXPLODE_RADIUS);
	e->flags |= EF_KILLED;
	m->state = MS_GONE;
}

void Mines_Tick(f32 dt)
{
	f32 now = Ent_Now();
	for (int i = 0; i < numMn; i++) {
		Mine *m = &mn[i];
		Entity *e = E(m->ent);
		if (m->state == MS_GONE || (e->flags & EF_KILLED))
			continue;
		Npc *np = Npc_OfEntity(m->ent);
		if (np && np->dead) { /* shot: it goes off */
			explode(m);
			continue;
		}
		if (m->state == MS_HOP) {
			/* in the air: gravity, and it goes off at the top or when the delay runs out */
			m->vel[2] -= 600 * dt;
			f32 to[3] = {e->origin[0] + m->vel[0] * dt, e->origin[1] + m->vel[1] * dt, e->origin[2] + m->vel[2] * dt};
			Trace tr;
			CM_SetIgnoreEnt(m->ent);
			CM_TraceWorld(&tr, e->origin, to, MASK_PLAYERSOLID);
			CM_SetIgnoreEnt(-1);
			memcpy(e->origin, tr.endpos, 12);
			if (now >= m->boomAt || (m->citizen && m->vel[2] <= 0) || tr.fraction < 1)
				explode(m);
			continue;
		}
		if (m->state == MS_DORMANT || (e->flags & EF_DISABLED) || now < m->nextThink)
			continue;
		m->nextThink = now + 0.1f;
		f32 d;
		s32 t = nearest(m, &d);
		if (t < 0) {
			if (m->state == MS_WARN) { /* CloseHooks */
				m->state = MS_ARMED;
				snd(m, 2);
			}
			continue;
		}
		if (m->state == MS_ARMED) { /* OpenHooks: the warning */
			m->state = MS_WARN;
			snd(m, 1);
		}
		if (d <= DETONATE_RADIUS) {
			/* Hop: up over its target's head height and toward it */
			const f32 *p = t == 0 ? Ent_Origin(0) : E(t)->origin;
			f32 h = (p[2] + 64) - e->origin[2];
			if (h < 48)
				h = 48;
			f32 up = sqrtf(2 * 600 * h), tt = up / 600;
			m->vel[0] = (p[0] - e->origin[0]) / (2 * tt);
			m->vel[1] = (p[1] - e->origin[1]) / (2 * tt);
			m->vel[2] = up;
			const char *dl = Ent_KV(m->ent, "explosiondelay");
			m->boomAt = now + (dl && *dl ? (f32)atof(dl) : 0.5f);
			m->state = MS_HOP;
			snd(m, 0);
			Ent_FireOutput(m->ent, "ontriggered", t);
		}
	}
}

int Mines_Input(s32 ent, const char *in)
{
	for (int i = 0; i < numMn; i++) {
		Mine *m = &mn[i];
		if (m->ent != ent)
			continue;
		if (!strcasecmp(in, "disarm")) {
			if (m->state != MS_HOP && m->state != MS_GONE)
				m->state = MS_DORMANT;
		} else if (!strcasecmp(in, "arm") || !strcasecmp(in, "enable")) {
			if (m->state == MS_DORMANT)
				m->state = MS_ARMED;
		} else
			return 0;
		return 1;
	}
	return 0;
}

int Mines_Probe(s32 ent) { for (int i = 0; i < numMn; i++) if (mn[i].ent == ent) return mn[i].state; return -1; }
