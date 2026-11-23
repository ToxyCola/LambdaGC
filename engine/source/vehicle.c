#include "vehicle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "collide.h"
#include "npc.h"
#include "physics.h"
#include "weapons.h"

/* HL2's vehicles face -Y in their models (front wheels / bow at -y, +x is the left side) */

enum { VT_NONE, VT_AIRBOAT, VT_JEEP, VT_POD };

#define MPH 17.6f /* units per second */
#define JEEP_MAX (35 * MPH)
#define JEEP_BOOST_MAX (50 * MPH)
#define JEEP_REVERSE (8 * MPH)
#define JEEP_BOOST_TIME 1.5f
#define JEEP_BOOST_DELAY 15.0f
#define AIRBOAT_MAX (40 * MPH)
#define AIRBOAT_REVERSE (10 * MPH)
#define GRAVITY 600.0f

typedef struct {
	f32 x, y; /* model space */
} Probe;

static const Probe jeepWheels[4] = {{34.7f, -43.4f}, {-34.7f, -43.4f}, {42.3f, 92.4f}, {-42.3f, 92.4f}};
static const Probe boatPoints[4] = {{0, -60}, {0, 80}, {38, 10}, {-38, 10}};

static const Pack *P;
static Player *pl;

static struct {
	s32 ent;           /* driven/ridden entity, -1 none */
	int type;
	f32 heading;       /* degrees: the way the front points */
	f32 speed;         /* along the heading */
	f32 vz;
	f32 slope[3];      /* ground up vector */
	f32 throttle, steer;
	u32 buttons, prevButtons;
	f32 lookYaw, lookPitch;
	f32 boostUntil, boostReady, clock;
	u32 idleSnd, throttleSnd;
	u8 onGround;
	u8 gun;            /* EnableGun: the jeep's tau cannon / the airboat's gun */
	f32 nextShot, gunHeat;
	f32 fwd[2];        /* the model's front, in model space (from its driver's eyes) */
} v = {.ent = -1, .fwd = {0, -1}};

/* a vehicle travelling between levels */
static struct {
	int active, type;
	f32 rel[3], headingRel;
	f32 speed;
} carry;

static s32 ent_index(const Entity *e)
{
	u32 n;
	return (s32)(e - Ent_List(&n));
}

static int type_of(const Entity *e)
{
	if (!strcmp(e->classname, "prop_vehicle_airboat"))
		return VT_AIRBOAT;
	if (!strcmp(e->classname, "prop_vehicle_prisoner_pod"))
		return VT_POD;
	if (!strcmp(e->classname, "prop_vehicle_jeep")) {
		const char *m = Ent_KV(ent_index(e), "model");
		return (m && strstr(m, "buggy")) ? VT_JEEP : VT_NONE; /* the APCs are NPC-driven */
	}
	return VT_NONE;
}

static Entity *ent_at(s32 i)
{
	u32 n;
	Entity *ents = Ent_List(&n);
	return (i > 0 && (u32)i < n) ? &ents[i] : NULL;
}

static void angles_from_R(const f32 R[3][3], f32 a[3])
{
	f32 xy = sqrtf(R[0][0] * R[0][0] + R[1][0] * R[1][0]);
	const f32 k = 180.0f / (f32)M_PI;
	if (xy > 0.001f) {
		a[1] = atan2f(R[1][0], R[0][0]) * k;
		a[0] = atan2f(-R[2][0], xy) * k;
		a[2] = atan2f(R[2][1], R[2][2]) * k;
	} else {
		a[1] = atan2f(-R[0][1], R[1][1]) * k;
		a[0] = atan2f(-R[2][0], xy) * k;
		a[2] = 0;
	}
}

static void norm(f32 a[3])
{
	f32 l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
	if (l > 1e-6f)
		for (int k = 0; k < 3; k++)
			a[k] /= l;
}

/* entity rotation from the heading and the ground's up vector. The model's front is v.fwd (model space,
 * read from its vehicle_driver_eyes attachment: Source's view and the vehicle's front). The models are
 * converted in their idle pose, as the PC draws them, which turns some round from their bind pose (the
 * buggy and airboat by 180 degrees, the pod by 90): their front is wherever the eyes look. */
static void place(Entity *e, f32 heading, const f32 up[3])
{
	f32 h = heading * (f32)M_PI / 180.0f;
	f32 f[3] = {cosf(h), sinf(h), 0};
	f32 u[3] = {up[0], up[1], up[2]};
	norm(u);
	/* forward along the slope */
	f32 d = f[0] * u[0] + f[1] * u[1] + f[2] * u[2];
	for (int k = 0; k < 3; k++)
		f[k] -= u[k] * d;
	norm(f);
	f32 l[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
	/* in the model: front fx,fy; its left is (-fy, fx) */
	f32 fx = v.fwd[0], fy = v.fwd[1];
	for (int k = 0; k < 3; k++) {
		/* R * (fx,fy,0) = f, R * (-fy,fx,0) = l */
		e->R[k][0] = f[k] * fx - l[k] * fy;
		e->R[k][1] = f[k] * fy + l[k] * fx;
		e->R[k][2] = u[k];
	}
	angles_from_R(e->R, e->angles);
}

static void front_of(const Entity *e, f32 out[3])
{
	for (int k = 0; k < 3; k++)
		out[k] = e->R[k][0] * v.fwd[0] + e->R[k][1] * v.fwd[1];
}

static f32 heading_of(const Entity *e)
{
	f32 f[3];
	front_of(e, f);
	return atan2f(f[1], f[0]) * 180.0f / (f32)M_PI;
}

/* the model's front from its driver's eyes (attachment x axis), in model space */
static void learn_front(Entity *e)
{
	v.fwd[0] = 0;
	v.fwd[1] = -1;
	Npc *n = Npc_OfEntity(ent_index(e));
	f32 eye[3], R[3][3];
	if (!n || !Npc_Attachment(n, "vehicle_driver_eyes", eye, R))
		return;
	/* world -> model: R^T */
	f32 mx = e->R[0][0] * R[0][0] + e->R[1][0] * R[1][0] + e->R[2][0] * R[2][0];
	f32 my = e->R[0][1] * R[0][0] + e->R[1][1] * R[1][0] + e->R[2][1] * R[2][0];
	f32 l = sqrtf(mx * mx + my * my);
	if (l > 0.5f) {
		v.fwd[0] = mx / l;
		v.fwd[1] = my / l;
	}
}

void Veh_LevelStart(const Pack *p, Player *player)
{
	P = p;
	pl = player;
	if (v.idleSnd)
		Audio_Stop(v.idleSnd);
	if (v.throttleSnd)
		Audio_Stop(v.throttleSnd);
	memset(&v, 0, sizeof(v));
	v.ent = -1;
	if (pl)
		pl->frozen = 0;
	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++)
		if (ents[i].cls == EC_VEHICLE) {
			ents[i].flags |= EF_SOLID;
			ents[i].locked = Ent_KV((s32)i, "vehiclelocked") && atoi(Ent_KV((s32)i, "vehiclelocked")) != 0;
			const char *c = Ent_KV((s32)i, "_carried");
			if (c && c[0] == '1')
				ents[i].flags |= EF_DISABLED; /* waiting for a vehicle from the last level */
		}
}

s32 Veh_Driving(void) { return v.ent; }

static void engine_sounds(int on)
{
	if (v.idleSnd)
		Audio_Stop(v.idleSnd);
	if (v.throttleSnd)
		Audio_Stop(v.throttleSnd);
	v.idleSnd = v.throttleSnd = 0;
	if (!on || v.type == VT_POD)
		return;
	int boat = v.type == VT_AIRBOAT;
	Weap_PlaySound(boat ? X_AIRBOAT_START : X_JEEP_START, NULL, 1);
	v.idleSnd = Weap_PlaySound(boat ? X_AIRBOAT_IDLE : X_JEEP_IDLE, NULL, 0.7f);
	v.throttleSnd = Weap_PlaySound(boat ? X_AIRBOAT_THROTTLE : X_JEEP_THROTTLE, NULL, 0.0f);
}

static void enter(Entity *e)
{
	v.ent = ent_index(e);
	v.type = type_of(e);
	learn_front(e);
	v.heading = heading_of(e);
	v.speed = v.vz = 0;
	v.slope[0] = v.slope[1] = 0;
	v.slope[2] = 1;
	v.lookYaw = v.lookPitch = 0;
	v.prevButtons = 0xFFFFFFFF; /* the use press that got us in mustn't get us out */
	{
		/* EnableGun: the input's last word, else the vehicle's "EnableGun" keyvalue */
		const char *g = Ent_KV(ent_index(e), "enablegun");
		v.gun = e->value >= 2.0f ? 1 : e->value >= 1.0f ? 0 : (u8)(g && atoi(g) != 0);
		v.gunHeat = 0;
	}
	pl->frozen = 1;
	memset(pl->velocity, 0, sizeof(pl->velocity));
	e->flags &= ~EF_DISABLED;
	Ent_FireOutput(v.ent, "playeron", 0);
	engine_sounds(1);
}

int Veh_TryEnter(s32 ent)
{
	Entity *e = ent_at(ent);
	if (!e || v.ent >= 0 || !pl)
		return 0;
	int t = type_of(e);
	if (t == VT_NONE || t == VT_POD || e->locked)
		return 0; /* pods are entered by the map's scripts */
	enter(e);
	return 1;
}

static int exit_vehicle(int force)
{
	Entity *e = ent_at(v.ent);
	if (!e) {
		v.ent = -1;
		pl->frozen = 0;
		return 1;
	}
	if (!force && (fabsf(v.speed) > 100 || v.type == VT_POD))
		return 0; /* Source won't let you out while moving fast; pods only by script */
	Npc *n = Npc_OfEntity(v.ent);
	/* the model's exit points, in order, then around it */
	for (int k = 1; k <= 12; k++) {
		f32 pos[3], R[3][3];
		char nm[16];
		snprintf(nm, sizeof(nm), "exit%d", k);
		int ok = 0;
		if (k <= 10 && n)
			ok = Npc_Attachment(n, nm, pos, R);
		if (!ok) {
			if (k <= 10)
				continue;
			f32 side = k == 11 ? 1.0f : -1.0f;
			for (int c = 0; c < 3; c++)
				pos[c] = e->origin[c] + e->R[c][0] * 90 * side + e->R[c][2] * 16;
		}
		Player t;
		if (Player_Spawn(&t, pos, pl->pitch, pl->yaw) && fabsf(t.origin[2] - pos[2]) < 48) {
			*pl = t;
			pl->frozen = 0;
			Ent_FireOutput(v.ent, "playeroff", 0);
			engine_sounds(0);
			Weap_PlaySound(v.type == VT_AIRBOAT ? X_AIRBOAT_STOP : X_JEEP_STOP, NULL, 1);
			v.ent = -1;
			return 1;
		}
	}
	return 0;
}

void Veh_Input(Entity *e, const char *in, const char *param)
{
	(void)param;
	if (!strcasecmp(in, "lock"))
		e->locked = 1;
	else if (!strcasecmp(in, "unlock"))
		e->locked = 0;
	else if ((!strcasecmp(in, "entervehicle") || !strcasecmp(in, "entervehicleimmediate")) && v.ent < 0 && pl)
		enter(e);
	else if ((!strcasecmp(in, "exitvehicle") || !strcasecmp(in, "exitvehicleimmediate")) &&
	         v.ent == ent_index(e))
		exit_vehicle(1);
	else if (!strcasecmp(in, "enablegun")) {
		e->value = atoi(param) ? 2.0f : 1.0f; /* remembered on the entity: 2 on, 1 off */
		if (v.ent == ent_index(e))
			v.gun = (u8)(atoi(param) != 0);
	}
}

int Veh_GunState(f32 *heat) { if (heat) *heat = v.gunHeat; return v.ent >= 0 && v.gun; }

/* the mounted gun, aimed where the driver looks: the jeep's tau cannon (sk_jeep_gauss_damage 15, a shot per
 * press) and the airboat's gun (held down: bursts of 3-damage rounds that heat it up until it has to cool) */
static void gun_tick(f32 dt)
{
	if (!v.gun || v.ent < 0)
		return;
	v.gunHeat -= dt * 0.35f;
	if (v.gunHeat < 0)
		v.gunHeat = 0;
	if (!(v.buttons & VB_FIRE) || v.clock < v.nextShot)
		return;
	f32 eye[3], pitch, yaw, roll;
	if (!Veh_View(eye, &pitch, &yaw, &roll))
		return;
	f32 cp = cosf(pitch * (f32)M_PI / 180.0f);
	f32 dir[3] = {cp * cosf(yaw * (f32)M_PI / 180.0f), cp * sinf(yaw * (f32)M_PI / 180.0f), -sinf(pitch * (f32)M_PI / 180.0f)};
	if (v.type == VT_JEEP) {
		if (v.prevButtons & VB_FIRE)
			return; /* one shot per press */
		Weap_PlaySound(X_JEEP_CANNON, eye, 1.0f);
		Weap_VehicleShot(eye, dir, 15.0f);
		v.nextShot = v.clock + 0.2f;
	} else {
		if (v.gunHeat >= 1.0f)
			return; /* overheated: wait */
		Weap_PlaySound(X_AIRBOAT_GUN, eye, 0.8f);
		Weap_VehicleShot(eye, dir, 3.0f);
		v.gunHeat += 0.03f;
		v.nextShot = v.clock + 0.06f;
	}
}

void Veh_Controls(f32 throttle, f32 steer, f32 lookYaw, f32 lookPitch, u32 buttons)
{
	v.throttle = throttle;
	v.steer = steer;
	v.lookYaw = lookYaw;
	v.lookPitch = lookPitch;
	v.buttons = buttons;
}

/* the height of the ground (or the water's surface for the airboat) under a point */
static int surface_under(f32 x, f32 y, f32 z, int water, f32 *out, f32 n[3])
{
	f32 a[3] = {x, y, z + 48}, b[3] = {x, y, z - 96};
	f32 mn[3] = {-2, -2, 0}, mx[3] = {2, 2, 2};
	Trace tr;
	CM_SetIgnoreEnt(v.ent);
	CM_BoxTrace(&tr, a, b, mn, mx, MASK_PLAYERSOLID);
	CM_SetIgnoreEnt(-1);
	int hit = tr.fraction < 1 && !tr.startsolid;
	f32 h = hit ? tr.endpos[2] : -1e9f;
	if (hit)
		memcpy(n, tr.normal, 12);
	if (water) {
		/* water above the ground here: find its top */
		f32 lo = hit ? h : z - 96, hi = z + 48;
		f32 p[3] = {x, y, lo + 1};
		if (CM_PointContents(p) & MASK_WATER) {
			for (int it = 0; it < 12; it++) {
				f32 mid = (lo + hi) * 0.5f;
				p[2] = mid;
				if (CM_PointContents(p) & MASK_WATER)
					lo = mid;
				else
					hi = mid;
			}
			if (lo > h) {
				h = lo;
				n[0] = n[1] = 0;
				n[2] = 1;
				hit = 2;
			}
		}
	}
	*out = h;
	return hit;
}

static void run_over(Entity *e)
{
	/* NPCs in the way get hit like on PC; physics objects get shoved */
	u32 nn;
	Npc *npcs = Npc_List(&nn);
	f32 sp = fabsf(v.speed);
	if (sp < 150)
		return;
	f32 h = v.heading * (f32)M_PI / 180.0f;
	f32 f[3] = {cosf(h), sinf(h), 0};
	for (u32 i = 0; i < nn; i++) {
		Npc *n = &npcs[i];
		Entity *ne = ent_at(n->ent);
		if (!ne || n->dead || n->prop || (ne->flags & (EF_KILLED | EF_DISABLED)))
			continue;
		f32 d[3] = {ne->origin[0] - e->origin[0], ne->origin[1] - e->origin[1], ne->origin[2] - e->origin[2]};
		if (fabsf(d[2]) > 80 || d[0] * d[0] + d[1] * d[1] > 90 * 90)
			continue;
		if ((d[0] * f[0] + d[1] * f[1]) * (v.speed > 0 ? 1 : -1) < 0)
			continue;
		Ent_Damage(n->ent, sp * 0.5f, f, ne->origin);
	}
}

/* the body: three boxes along the front direction f (offsets from the origin, forward positive) */
static const f32 jeepSegs[3] = {50, -25, -100}, boatSegs[3] = {45, -15, -75};
#define SEG_HALF 40.0f

static void body_trace(Entity *e, const f32 from[3], const f32 to[3], const f32 f[3], int boat, Trace *best)
{
	(void)e;
	const f32 *segs = boat ? boatSegs : jeepSegs;
	f32 mn[3] = {-SEG_HALF, -SEG_HALF, 12}, mx[3] = {SEG_HALF, SEG_HALF, 56};
	best->fraction = 2;
	CM_SetIgnoreEnt(v.ent);
	CM_SetSkipBoxes(1); /* (NPCs are run over, not driven around) */
	for (int i = 0; i < 3; i++) {
		f32 a[3] = {from[0] + f[0] * segs[i], from[1] + f[1] * segs[i], from[2]};
		f32 b[3] = {to[0] + f[0] * segs[i], to[1] + f[1] * segs[i], to[2]};
		Trace t;
		CM_BoxTrace(&t, a, b, mn, mx, MASK_PLAYERSOLID);
		if (t.startsolid || t.fraction < best->fraction) {
			*best = t;
			if (t.startsolid)
				break;
		}
	}
	CM_SetSkipBoxes(0);
	CM_SetIgnoreEnt(-1);
	/* the end for the origin */
	for (int k = 0; k < 3; k++)
		best->endpos[k] = from[k] + (to[k] - from[k]) * (best->fraction > 1 ? 1 : best->fraction);
	if (best->fraction > 1)
		best->fraction = 1;
}

static int body_trace_start(Entity *e, const f32 f[3], int boat)
{
	Trace t;
	body_trace(e, e->origin, e->origin, f, boat, &t);
	return t.startsolid;
}

/* inside something (spawned overlapping, pushed in, turned into a wall): step out to the nearest free
 * spot; if there's none, carry on through rather than stay stuck */
static void unstick(Entity *e, const f32 f[3], int boat)
{
	static const f32 dirs[10][3] = {{0, 0, 1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0},
	                                {0.7f, 0.7f, 0}, {-0.7f, 0.7f, 0}, {0.7f, -0.7f, 0}, {-0.7f, -0.7f, 0}, {0, 0, -1}};
	f32 start[3];
	memcpy(start, e->origin, 12);
	for (f32 d = 4; d <= 48; d += 4)
		for (int i = 0; i < 10; i++) {
			for (int k = 0; k < 3; k++)
				e->origin[k] = start[k] + dirs[i][k] * d;
			if (!body_trace_start(e, f, boat))
				return;
		}
	memcpy(e->origin, start, 12);
}

static void drive(Entity *e, f32 dt)
{
	int boat = v.type == VT_AIRBOAT;
	v.clock += dt;
	/* throttle, brakes, turbo */
	f32 maxF = boat ? AIRBOAT_MAX : JEEP_MAX, maxR = boat ? AIRBOAT_REVERSE : JEEP_REVERSE;
	if (!boat && (v.buttons & VB_TURBO) && !(v.prevButtons & VB_TURBO) && v.clock >= v.boostReady && v.throttle > 0) {
		v.boostUntil = v.clock + JEEP_BOOST_TIME;
		v.boostReady = v.clock + JEEP_BOOST_DELAY;
		Weap_PlaySound(X_JEEP_TURBO, NULL, 1);
	}
	int boosting = v.clock < v.boostUntil;
	if (boosting)
		maxF = JEEP_BOOST_MAX;
	f32 accel = boat ? 380.0f : 420.0f;
	if (boosting)
		accel *= 2.2f;
	f32 grip = v.onGround ? 1.0f : (boat ? 0.0f : 0.0f);
	if (boat && v.onGround == 1)
		grip = 0.35f; /* the airboat on land: its fan still pushes, slowly */
	if (boat)
		grip = v.onGround == 2 ? 1.0f : grip; /* on water */
	if (v.throttle > 0.05f) {
		if (v.speed < 0)
			v.speed += 900.0f * dt * grip; /* braking out of reverse */
		else if (v.speed < maxF * v.throttle)
			v.speed += accel * v.throttle * dt * grip;
	} else if (v.throttle < -0.05f) {
		if (v.speed > 0)
			v.speed -= 900.0f * dt * grip;
		else if (v.speed > -maxR)
			v.speed += accel * 0.6f * v.throttle * dt * grip;
	} else {
		/* rolling resistance / water drag */
		f32 drag = boat ? (v.onGround == 2 ? 0.5f : 2.0f) : 0.6f;
		v.speed -= v.speed * drag * dt;
	}
	if (v.buttons & VB_HANDBRAKE)
		v.speed -= v.speed * 2.5f * dt;
	if (v.speed > maxF * 1.05f)
		v.speed -= (v.speed - maxF) * 2 * dt;
	/* steering: wheels turn the buggy in proportion to speed; the airboat's fan turns it at any speed */
	f32 turn;
	if (boat)
		turn = -v.steer * (60.0f + fabsf(v.speed) * 0.04f);
	else {
		f32 sp = fabsf(v.speed);
		f32 maxDeg = sp > 400 ? 18.0f : 30.0f; /* degreesFast / degreesSlow */
		turn = -v.steer * sp / 180.0f * maxDeg * (v.speed < 0 ? -1.0f : 1.0f) * ((v.buttons & VB_HANDBRAKE) ? 1.8f : 1.0f);
	}
	if (v.onGround || boat)
		v.heading += turn * dt;

	/* move, sliding along what we hit: the body is three boxes along its length (one box the size of the
	 * whole buggy can't turn with it; one the size of its middle let the front and back into walls) */
	f32 h = v.heading * (f32)M_PI / 180.0f;
	f32 f[3] = {cosf(h), sinf(h), 0};
	f32 move[3] = {f[0] * v.speed * dt, f[1] * v.speed * dt, 0};
	if (body_trace_start(e, f, boat))
		unstick(e, f, boat);
	for (int it = 0; it < 3; it++) {
		f32 to[3] = {e->origin[0] + move[0], e->origin[1] + move[1], e->origin[2]};
		Trace tr;
		body_trace(e, e->origin, to, f, boat, &tr);
		if (tr.startsolid) {
			memcpy(e->origin, to, 12); /* nowhere free near: drive out of it rather than stay stuck */
			break;
		}
		memcpy(e->origin, tr.endpos, 12);
		if (tr.fraction >= 1)
			break;
#ifdef HOST_TEST
		if (getenv("DBG"))
			printf("   vehicle hit ent %d normal %.2f %.2f %.2f speed %.0f\n", tr.ent, tr.normal[0], tr.normal[1], tr.normal[2], v.speed);
#endif
		if (tr.ent > 0 && Phys_IsBody(tr.ent)) {
			f32 imp[3] = {f[0] * v.speed * 120.0f, f[1] * v.speed * 120.0f, 20000.0f};
			Phys_Impulse(tr.ent, tr.endpos, imp);
		}
		/* hit: lose speed by how square it was, slide the rest */
		f32 into = -(f[0] * tr.normal[0] + f[1] * tr.normal[1]);
		if (into > 0.5f && fabsf(v.speed) > 250)
			Weap_PlaySound(boat ? X_AIRBOAT_IMPACT : X_JEEP_IMPACT, e->origin, 1);
		v.speed *= 1.0f - (into > 0 ? into : 0) * 0.8f;
		f32 rem = 1 - tr.fraction;
		f32 dn = move[0] * tr.normal[0] + move[1] * tr.normal[1];
		move[0] = (move[0] - tr.normal[0] * dn) * rem;
		move[1] = (move[1] - tr.normal[1] * dn) * rem;
	}

	/* the ground (or water) under the wheels / hull: height, and tilt from front/back, left/right */
	const Probe *pr = boat ? boatPoints : jeepWheels;
	f32 hz[4], n[3] = {0, 0, 1};
	int hits = 0, water = 0;
	f32 l[3] = {-f[1], f[0], 0};
	for (int k = 0; k < 4; k++) {
		/* model x = left, model -y = forward */
		f32 wx = e->origin[0] + l[0] * pr[k].x - f[0] * pr[k].y;
		f32 wy = e->origin[1] + l[1] * pr[k].x - f[1] * pr[k].y;
		f32 nn[3];
		int r = surface_under(wx, wy, e->origin[2], boat, &hz[k], nn);
		if (r) {
			hits++;
			water += r == 2;
		} else
			hz[k] = e->origin[2] - 200;
	}
	f32 ground = (hz[0] + hz[1] + hz[2] + hz[3]) * 0.25f;
	f32 ride = boat && water ? -4.0f : 0.0f; /* the airboat sits a little into the water */
	if (hits >= 2 && e->origin[2] <= ground + ride + 6 + (v.vz > 0 ? 0 : 12)) {
		e->origin[2] += (ground + ride - e->origin[2]) * (dt * 12 > 1 ? 1 : dt * 12);
		v.vz = 0;
		v.onGround = water >= 2 ? 2 : 1;
		/* tilt: front minus back along the wheelbase, left minus right */
		f32 front = (hz[0] + hz[1]) * 0.5f, back = (hz[2] + hz[3]) * 0.5f;
		f32 left = (hz[0] + hz[2]) * 0.5f, right = (hz[1] + hz[3]) * 0.5f;
		f32 wb = boat ? 140.0f : 136.0f, tw = boat ? 76.0f : 74.0f;
		f32 pitchSlope = (front - back) / wb, rollSlope = (left - right) / tw;
		pitchSlope = pitchSlope > 1 ? 1 : pitchSlope < -1 ? -1 : pitchSlope;
		rollSlope = rollSlope > 1 ? 1 : rollSlope < -1 ? -1 : rollSlope;
		for (int k = 0; k < 3; k++)
			n[k] = -f[k] * pitchSlope - l[k] * rollSlope;
		n[2] = 1;
		norm(n);
	} else {
		v.onGround = 0;
		v.vz -= GRAVITY * dt;
		f32 to[3] = {e->origin[0], e->origin[1], e->origin[2] + v.vz * dt};
		Trace tr;
		body_trace(e, e->origin, to, f, boat, &tr);
		if (!tr.startsolid)
			memcpy(e->origin, tr.endpos, 12);
		if (tr.fraction < 1)
			v.vz = 0;
		memcpy(n, v.slope, 12);
	}
	for (int k = 0; k < 3; k++)
		v.slope[k] += (n[k] - v.slope[k]) * (dt * 8 > 1 ? 1 : dt * 8);
	place(e, v.heading, v.slope);
	run_over(e);

	/* engine pitch: crossfade idle into full throttle */
	f32 load = fabsf(v.throttle) * 0.6f + fabsf(v.speed) / (boat ? AIRBOAT_MAX : JEEP_MAX) * 0.4f;
	if (v.idleSnd)
		Audio_SetChannelVolume(v.idleSnd, 0.7f * (1 - load * 0.7f));
	if (v.throttleSnd)
		Audio_SetChannelVolume(v.throttleSnd, 0.8f * load);
}

void Veh_Tick(f32 dt)
{
	Entity *e = ent_at(v.ent);
	if (!e || !pl) {
		v.ent = -1;
		return;
	}
	if (e->flags & EF_KILLED) {
		exit_vehicle(1);
		return;
	}
	if ((v.buttons & VB_EXIT) && !(v.prevButtons & VB_EXIT))
		exit_vehicle(0);
	if (v.ent < 0)
		return;
	if (v.type != VT_POD) {
		drive(e, dt);
		gun_tick(dt);
	}
	v.prevButtons = v.buttons;
	/* the player rides inside: triggers and everything else see them where the vehicle is */
	f32 eye[3];
	f32 p, y, r;
	if (Veh_View(eye, &p, &y, &r)) {
		pl->origin[0] = eye[0];
		pl->origin[1] = eye[1];
		pl->origin[2] = eye[2] - 64;
	}
	memset(pl->velocity, 0, sizeof(pl->velocity));
	pl->velocity[0] = cosf(v.heading * (f32)M_PI / 180) * v.speed;
	pl->velocity[1] = sinf(v.heading * (f32)M_PI / 180) * v.speed;
}

int Veh_View(f32 eye[3], f32 *pitch, f32 *yaw, f32 *roll)
{
	Entity *e = ent_at(v.ent);
	if (!e)
		return 0;
	Npc *n = Npc_OfEntity(v.ent);
	f32 R[3][3];
	if (!n || !Npc_Attachment(n, "vehicle_driver_eyes", eye, R)) {
		for (int k = 0; k < 3; k++)
			eye[k] = e->origin[k] + e->R[k][2] * 64;
	}
	f32 a[3];
	angles_from_R(e->R, a);
	/* the view looks along the front, plus where the player turns their head */
	f32 fr[3];
	front_of(e, fr);
	f32 fx = fr[0], fy = fr[1], fz = fr[2];
	*yaw = atan2f(fy, fx) * 180.0f / (f32)M_PI + v.lookYaw;
	*pitch = -asinf(fz > 1 ? 1 : fz < -1 ? -1 : fz) * 180.0f / (f32)M_PI + v.lookPitch;
	*roll = 0;
	return 1;
}

void Veh_PrepareCarry(const f32 landmarkFrom[3])
{
	carry.active = 0;
	Entity *e = ent_at(v.ent);
	if (!e || v.type == VT_POD)
		return;
	carry.active = 1;
	carry.type = v.type;
	for (int k = 0; k < 3; k++)
		carry.rel[k] = e->origin[k] - landmarkFrom[k];
	carry.headingRel = v.heading;
	carry.speed = v.speed;
	engine_sounds(0);
}

void Veh_FinishCarry(const f32 landmarkTo[3])
{
	if (!carry.active || !pl)
		return;
	carry.active = 0;
	u32 n;
	Entity *ents = Ent_List(&n);
	/* the map's own vehicle of that kind if it has one standing (not waiting in a template),
	 * else the placeholder every vehicle level has */
	Entity *use = NULL;
	for (u32 i = 1; i < n && !use; i++) {
		Entity *e = &ents[i];
		const char *c = Ent_KV((s32)i, "_carried");
		if (e->cls == EC_VEHICLE && type_of(e) == carry.type && !(e->flags & (EF_DISABLED | EF_KILLED)) &&
		    !(c && c[0] == '1'))
			use = e;
	}
	for (u32 i = 1; i < n && !use; i++) {
		Entity *e = &ents[i];
		const char *c = Ent_KV((s32)i, "_carried");
		if (e->cls == EC_VEHICLE && type_of(e) == carry.type && c && c[0] == '1')
			use = e;
	}
	if (!use)
		return;
	use->flags &= ~(EF_DISABLED | EF_KILLED);
	for (int k = 0; k < 3; k++)
		use->origin[k] = landmarkTo[k] + carry.rel[k];
	f32 up[3] = {0, 0, 1};
	place(use, carry.headingRel, up);
	enter(use);
	v.heading = carry.headingRel;
	v.speed = carry.speed;
}
