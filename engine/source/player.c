#include "player.h"

#include <math.h>
#include <string.h>

#include "collide.h"

/* Half-Life 2 defaults (movevars_shared.cpp, hl2_player.cpp, gamerules.cpp in the SDK) */
#define SV_GRAVITY 600.0f
#define SV_FRICTION 4.0f
#define SV_STOPSPEED 100.0f
#define SV_ACCELERATE 10.0f
#define SV_AIRACCELERATE 10.0f
#define SV_MAXVELOCITY 3500.0f
#define STEP_SIZE 18.0f
#define WALK_SPEED 150.0f
#define NORM_SPEED 190.0f
#define SPRINT_SPEED 320.0f
#define DUCK_SPEED_MULT 0.33333333f
#define JUMP_SPEED 160.0f   /* sqrt(2 * 600 * 21) */
#define NON_JUMP_VELOCITY 140.0f
#define AIR_SPEED_CAP 30.0f

static f32 speedScale = 1.0f;
void Player_SetSpeedScale(f32 s) { speedScale = s > 0 ? s : 1.0f; }
#define MAX_CLIP_PLANES 5
#define VIEW_STAND 64.0f
#define VIEW_DUCK 28.0f
#define DUCK_TIME 0.4f

static const f32 HULL_MIN[3] = {-16, -16, 0};
static const f32 HULL_MAX[3] = {16, 16, 72};
static const f32 DUCK_MAX[3] = {16, 16, 36};

static inline f32 dot3(const f32 *a, const f32 *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline f32 len3(const f32 *a) { return sqrtf(dot3(a, a)); }
static inline f32 norm3(f32 *a)
{
	f32 l = len3(a);
	if (l > 0) {
		a[0] /= l;
		a[1] /= l;
		a[2] /= l;
	}
	return l;
}

void Player_Bounds(const Player *pl, f32 mins[3], f32 maxs[3])
{
	memcpy(mins, HULL_MIN, sizeof(HULL_MIN));
	memcpy(maxs, pl->ducked ? DUCK_MAX : HULL_MAX, sizeof(HULL_MAX));
}

f32 Player_EyeHeight(const Player *pl)
{
	if (pl->noclip)
		return VIEW_STAND;
	/* while crouched the hull is shorter, so the eye sits at the duck view */
	if (pl->ducked)
		return VIEW_DUCK;
	return VIEW_STAND + (VIEW_DUCK - VIEW_STAND) * pl->duckAmount;
}

static void trace_hull(const Player *pl, const f32 *start, const f32 *end, Trace *tr)
{
	f32 mn[3], mx[3];
	Player_Bounds(pl, mn, mx);
	CM_BoxTrace(tr, start, end, mn, mx, MASK_PLAYERSOLID);
}

static void angle_vectors(f32 pitch, f32 yaw, f32 *fwd, f32 *right)
{
	f32 sp = sinf(pitch * (f32)M_PI / 180), cp = cosf(pitch * (f32)M_PI / 180);
	f32 sy = sinf(yaw * (f32)M_PI / 180), cy = cosf(yaw * (f32)M_PI / 180);
	fwd[0] = cp * cy;
	fwd[1] = cp * sy;
	fwd[2] = -sp;
	right[0] = sy;
	right[1] = -cy;
	right[2] = 0;
}

/* ------------------------------------------------------------------ helpers */

static void clip_velocity(const f32 *in, const f32 *n, f32 *out, f32 overbounce)
{
	f32 backoff = dot3(in, n) * overbounce;
	for (int i = 0; i < 3; i++)
		out[i] = in[i] - n[i] * backoff;
	f32 adjust = dot3(out, n);
	if (adjust < 0)
		for (int i = 0; i < 3; i++)
			out[i] -= n[i] * adjust;
}

static void check_velocity(Player *pl)
{
	for (int i = 0; i < 3; i++) {
		if (pl->velocity[i] > SV_MAXVELOCITY)
			pl->velocity[i] = SV_MAXVELOCITY;
		if (pl->velocity[i] < -SV_MAXVELOCITY)
			pl->velocity[i] = -SV_MAXVELOCITY;
	}
}

static void start_gravity(Player *pl)
{
	pl->velocity[2] -= SV_GRAVITY * 0.5f * TICK_INTERVAL;
	check_velocity(pl);
}

static void finish_gravity(Player *pl)
{
	pl->velocity[2] -= SV_GRAVITY * 0.5f * TICK_INTERVAL;
	check_velocity(pl);
}

static void categorize_position(Player *pl)
{
	pl->surfaceFriction = 1.0f;
	if (pl->velocity[2] > NON_JUMP_VELOCITY) {
		pl->onGround = 0;
		pl->groundEnt = -1;
		return;
	}
	f32 point[3] = {pl->origin[0], pl->origin[1], pl->origin[2] - 2.0f};
	Trace tr;
	trace_hull(pl, pl->origin, point, &tr);
	if (tr.fraction < 1.0f && tr.normal[2] >= 0.7f) {
		pl->onGround = 1;
		pl->groundEnt = tr.ent;
		memcpy(pl->groundNormal, tr.normal, sizeof(tr.normal));
	} else {
		pl->onGround = 0;
		pl->groundEnt = -1;
		if (pl->velocity[2] > 0)
			pl->surfaceFriction = 0.25f;
	}
}

/* Slide along everything we touch, up to four times per tick. */
static int try_player_move(Player *pl)
{
	f32 planes[MAX_CLIP_PLANES][3];
	int numplanes = 0, blocked = 0;
	f32 primal[3], original[3], newvel[3];
	memcpy(primal, pl->velocity, sizeof(primal));
	memcpy(original, pl->velocity, sizeof(original));
	f32 timeLeft = TICK_INTERVAL, allFraction = 0;

	for (int bump = 0; bump < 4; bump++) {
		if (len3(pl->velocity) == 0)
			break;
		f32 end[3];
		for (int i = 0; i < 3; i++)
			end[i] = pl->origin[i] + pl->velocity[i] * timeLeft;
		Trace tr;
		trace_hull(pl, pl->origin, end, &tr);
		allFraction += tr.fraction;
		if (tr.allsolid) {
			memset(pl->velocity, 0, sizeof(pl->velocity));
			return 4;
		}
		if (tr.fraction > 0) {
			memcpy(pl->origin, tr.endpos, sizeof(pl->origin));
			memcpy(original, pl->velocity, sizeof(original));
			numplanes = 0;
		}
		if (tr.fraction == 1)
			break;
		if (tr.normal[2] > 0.7f)
			blocked |= 1; /* floor */
		if (tr.normal[2] == 0)
			blocked |= 2; /* step / wall */
		timeLeft -= timeLeft * tr.fraction;
		if (numplanes >= MAX_CLIP_PLANES) {
			memset(pl->velocity, 0, sizeof(pl->velocity));
			break;
		}
		memcpy(planes[numplanes++], tr.normal, sizeof(tr.normal));

		if (numplanes == 1 && !pl->onGround) {
			/* airborne against one surface: slide (no bounce in HL2, sv_bounce = 0) */
			clip_velocity(original, planes[0], newvel, 1.0f);
			memcpy(pl->velocity, newvel, sizeof(newvel));
			memcpy(original, newvel, sizeof(newvel));
		} else {
			int i, j;
			for (i = 0; i < numplanes; i++) {
				clip_velocity(original, planes[i], pl->velocity, 1.0f);
				for (j = 0; j < numplanes; j++)
					if (j != i && dot3(pl->velocity, planes[j]) < 0)
						break;
				if (j == numplanes)
					break;
			}
			if (i == numplanes) {
				/* wedged between two planes: slide along the crease */
				if (numplanes != 2) {
					memset(pl->velocity, 0, sizeof(pl->velocity));
					break;
				}
				f32 dir[3] = {planes[0][1] * planes[1][2] - planes[0][2] * planes[1][1],
				              planes[0][2] * planes[1][0] - planes[0][0] * planes[1][2],
				              planes[0][0] * planes[1][1] - planes[0][1] * planes[1][0]};
				norm3(dir);
				f32 d = dot3(dir, pl->velocity);
				for (int k = 0; k < 3; k++)
					pl->velocity[k] = dir[k] * d;
			}
			if (dot3(pl->velocity, primal) <= 0) {
				memset(pl->velocity, 0, sizeof(pl->velocity));
				break;
			}
		}
	}
	if (allFraction == 0)
		memset(pl->velocity, 0, sizeof(pl->velocity));
	return blocked;
}

static void stay_on_ground(Player *pl)
{
	f32 start[3] = {pl->origin[0], pl->origin[1], pl->origin[2] + 2};
	f32 end[3] = {pl->origin[0], pl->origin[1], pl->origin[2] - STEP_SIZE};
	Trace tr;
	trace_hull(pl, pl->origin, start, &tr);
	memcpy(start, tr.endpos, sizeof(start));
	trace_hull(pl, start, end, &tr);
	if (tr.fraction > 0 && tr.fraction < 1 && !tr.startsolid && tr.normal[2] >= 0.7f) {
		if (fabsf(pl->origin[2] - tr.endpos[2]) > 0.5f / 8.0f)
			memcpy(pl->origin, tr.endpos, sizeof(pl->origin));
	}
}

/* Try the move flat and stepped up by STEP_SIZE; keep whichever gets further. */
static void step_move(Player *pl)
{
	f32 pos[3], vel[3], downPos[3], downVel[3], end[3];
	memcpy(pos, pl->origin, sizeof(pos));
	memcpy(vel, pl->velocity, sizeof(vel));
	Trace tr;

	try_player_move(pl);
	memcpy(downPos, pl->origin, sizeof(downPos));
	memcpy(downVel, pl->velocity, sizeof(downVel));

	memcpy(pl->origin, pos, sizeof(pos));
	memcpy(pl->velocity, vel, sizeof(vel));
	memcpy(end, pl->origin, sizeof(end));
	end[2] += STEP_SIZE + DIST_EPSILON;
	trace_hull(pl, pl->origin, end, &tr);
	if (!tr.startsolid && !tr.allsolid)
		memcpy(pl->origin, tr.endpos, sizeof(pl->origin));

	try_player_move(pl);

	memcpy(end, pl->origin, sizeof(end));
	end[2] -= STEP_SIZE + DIST_EPSILON;
	trace_hull(pl, pl->origin, end, &tr);
	if (tr.normal[2] < 0.7f) {
		/* stepping up put us somewhere we can't stand: use the flat result */
		memcpy(pl->origin, downPos, sizeof(downPos));
		memcpy(pl->velocity, downVel, sizeof(downVel));
		return;
	}
	if (!tr.startsolid && !tr.allsolid)
		memcpy(pl->origin, tr.endpos, sizeof(pl->origin));

	f32 downDist = (downPos[0] - pos[0]) * (downPos[0] - pos[0]) + (downPos[1] - pos[1]) * (downPos[1] - pos[1]);
	f32 upDist = (pl->origin[0] - pos[0]) * (pl->origin[0] - pos[0]) + (pl->origin[1] - pos[1]) * (pl->origin[1] - pos[1]);
	if (downDist > upDist) {
		memcpy(pl->origin, downPos, sizeof(downPos));
		memcpy(pl->velocity, downVel, sizeof(downVel));
	} else {
		pl->velocity[2] = downVel[2];
	}
}

static void friction(Player *pl)
{
	f32 speed = len3(pl->velocity);
	if (speed < 0.1f)
		return;
	f32 drop = 0;
	if (pl->onGround) {
		f32 control = speed < SV_STOPSPEED ? SV_STOPSPEED : speed;
		drop = control * SV_FRICTION * pl->surfaceFriction * TICK_INTERVAL;
	}
	f32 newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;
	for (int i = 0; i < 3; i++)
		pl->velocity[i] *= newspeed;
}

static void accelerate(Player *pl, const f32 *wishdir, f32 wishspeed, f32 accel)
{
	f32 add = wishspeed - dot3(pl->velocity, wishdir);
	if (add <= 0)
		return;
	f32 a = accel * TICK_INTERVAL * wishspeed * pl->surfaceFriction;
	if (a > add)
		a = add;
	for (int i = 0; i < 3; i++)
		pl->velocity[i] += a * wishdir[i];
}

static void air_accelerate(Player *pl, const f32 *wishdir, f32 wishspeed, f32 accel)
{
	f32 capped = wishspeed > AIR_SPEED_CAP ? AIR_SPEED_CAP : wishspeed;
	f32 add = capped - dot3(pl->velocity, wishdir);
	if (add <= 0)
		return;
	f32 a = accel * wishspeed * TICK_INTERVAL * pl->surfaceFriction;
	if (a > add)
		a = add;
	for (int i = 0; i < 3; i++)
		pl->velocity[i] += a * wishdir[i];
}

static f32 wish(const Player *pl, const UserCmd *cmd, f32 maxspeed, f32 *wishdir)
{
	f32 fwd[3], right[3];
	angle_vectors(pl->pitch, pl->yaw, fwd, right);
	fwd[2] = right[2] = 0;
	norm3(fwd);
	norm3(right);
	f32 fm = cmd->forwardmove * maxspeed, sm = cmd->sidemove * maxspeed;
	if (pl->ducked && pl->onGround) {
		fm *= DUCK_SPEED_MULT;
		sm *= DUCK_SPEED_MULT;
	}
	for (int i = 0; i < 2; i++)
		wishdir[i] = fwd[i] * fm + right[i] * sm;
	wishdir[2] = 0;
	f32 speed = norm3(wishdir);
	return speed > maxspeed ? maxspeed : speed;
}

static void walk_move(Player *pl, const UserCmd *cmd, f32 maxspeed)
{
	f32 wishdir[3];
	f32 wishspeed = wish(pl, cmd, maxspeed, wishdir);
	pl->velocity[2] = 0;
	accelerate(pl, wishdir, wishspeed, SV_ACCELERATE);
	pl->velocity[2] = 0;
	if (len3(pl->velocity) < 1.0f) {
		memset(pl->velocity, 0, sizeof(pl->velocity));
		return;
	}
	f32 dest[3] = {pl->origin[0] + pl->velocity[0] * TICK_INTERVAL, pl->origin[1] + pl->velocity[1] * TICK_INTERVAL,
	               pl->origin[2]};
	Trace tr;
	trace_hull(pl, pl->origin, dest, &tr);
	if (tr.fraction == 1) {
		memcpy(pl->origin, tr.endpos, sizeof(pl->origin));
		stay_on_ground(pl);
		return;
	}
	step_move(pl);
	stay_on_ground(pl);
}

static void air_move(Player *pl, const UserCmd *cmd, f32 maxspeed)
{
	f32 wishdir[3];
	f32 wishspeed = wish(pl, cmd, maxspeed, wishdir);
	air_accelerate(pl, wishdir, wishspeed, SV_AIRACCELERATE);
	try_player_move(pl);
}

/* Source's CGameMovement::CheckWater: contents at the feet, the waist and the eyes */
static void check_water(Player *pl)
{
	f32 mn[3], mx[3];
	Player_Bounds(pl, mn, mx);
	f32 p[3] = {pl->origin[0], pl->origin[1], pl->origin[2] + mn[2] + 1};
	int c = CM_PointContents(p);
	pl->waterLevel = 0;
	pl->waterType = 0;
	if (!(c & MASK_WATER))
		return;
	pl->waterType = c & MASK_WATER;
	pl->waterLevel = 1;
	p[2] = pl->origin[2] + (mn[2] + mx[2]) * 0.5f;
	if (CM_PointContents(p) & MASK_WATER) {
		pl->waterLevel = 2;
		p[2] = pl->origin[2] + Player_EyeHeight(pl);
		if (CM_PointContents(p) & MASK_WATER)
			pl->waterLevel = 3;
	}
}

/* CGameMovement::WaterMove: swim where you look, drift down when idle, jump to rise */
static void water_move(Player *pl, const UserCmd *cmd, f32 maxspeed)
{
	f32 fwd[3], right[3];
	f32 sp = sinf(pl->pitch * (f32)M_PI / 180), cp = cosf(pl->pitch * (f32)M_PI / 180);
	f32 sy = sinf(pl->yaw * (f32)M_PI / 180), cy = cosf(pl->yaw * (f32)M_PI / 180);
	fwd[0] = cp * cy;
	fwd[1] = cp * sy;
	fwd[2] = -sp;
	right[0] = sy;
	right[1] = -cy;
	right[2] = 0;
	f32 fm = cmd->forwardmove * maxspeed, sm = cmd->sidemove * maxspeed;
	f32 wv[3];
	for (int i = 0; i < 3; i++)
		wv[i] = fwd[i] * fm + right[i] * sm;
	if (cmd->buttons & IN_JUMP)
		wv[2] += maxspeed;
	else if (fm == 0 && sm == 0)
		wv[2] -= 60; /* drift towards bottom */
	else {
		f32 up = fm * fwd[2] * 2;
		wv[2] += up < 0 ? 0 : up > maxspeed ? maxspeed : up;
	}
	f32 wd[3] = {wv[0], wv[1], wv[2]};
	f32 wishspeed = norm3(wd);
	if (wishspeed > maxspeed) {
		for (int i = 0; i < 3; i++)
			wv[i] *= maxspeed / wishspeed;
		wishspeed = maxspeed;
	}
	wishspeed *= 0.8f;
	f32 speed = len3(pl->velocity), newspeed = 0;
	if (speed > 0) {
		newspeed = speed - TICK_INTERVAL * speed * SV_FRICTION * pl->surfaceFriction;
		if (newspeed < 0.1f)
			newspeed = 0;
		for (int i = 0; i < 3; i++)
			pl->velocity[i] *= newspeed / speed;
	}
	if (wishspeed >= 0.1f) {
		f32 add = wishspeed - newspeed;
		if (add > 0) {
			norm3(wv);
			f32 acc = SV_ACCELERATE * wishspeed * TICK_INTERVAL * pl->surfaceFriction;
			if (acc > add)
				acc = add;
			for (int i = 0; i < 3; i++)
				pl->velocity[i] += acc * wv[i];
		}
	}
	try_player_move(pl);
}

/* CGameMovement::CheckWaterJump: at the surface against a ledge, pushing forward hops you out */
static void check_water_jump(Player *pl, const UserCmd *cmd)
{
	if (pl->waterJumpTime > 0 || pl->velocity[2] < -180 || cmd->forwardmove <= 0)
		return;
	f32 fwd[3] = {cosf(pl->yaw * (f32)M_PI / 180), sinf(pl->yaw * (f32)M_PI / 180), 0};
	f32 mn[3], mx[3];
	Player_Bounds(pl, mn, mx);
	f32 a[3] = {pl->origin[0], pl->origin[1], pl->origin[2] + (mn[2] + mx[2]) * 0.5f};
	f32 b[3] = {a[0] + fwd[0] * 24, a[1] + fwd[1] * 24, a[2]};
	Trace tr;
	f32 z[3] = {0, 0, 0};
	CM_BoxTrace(&tr, a, b, z, z, MASK_PLAYERSOLID);
	if (tr.fraction >= 1)
		return;
	a[2] = pl->origin[2] + Player_EyeHeight(pl) + 8;
	b[2] = a[2];
	CM_BoxTrace(&tr, a, b, z, z, MASK_PLAYERSOLID);
	if (tr.fraction < 1)
		return;
	pl->waterJumpVel[0] = fwd[0] * 50;
	pl->waterJumpVel[1] = fwd[1] * 50;
	pl->velocity[2] = 256;
	pl->waterJumpTime = 2.0f;
}

static void check_jump(Player *pl, const UserCmd *cmd, f32 maxspeed)
{
	if (!pl->onGround) {
		pl->oldButtons |= IN_JUMP;
		return;
	}
	if (pl->oldButtons & IN_JUMP)
		return; /* must release between jumps */
	pl->onGround = 0;
	pl->groundEnt = -1;
	if (pl->ducked)
		pl->velocity[2] = JUMP_SPEED;
	else
		pl->velocity[2] += JUMP_SPEED;

	/* HL2 single player: a small forward push when jumping */
	f32 fwd[3], right[3];
	angle_vectors(pl->pitch, pl->yaw, fwd, right);
	fwd[2] = 0;
	norm3(fwd);
	int sprinting = (cmd->buttons & IN_SPEED) != 0;
	f32 boost = (!sprinting && !pl->ducked) ? 0.5f : 0.1f;
	f32 fm = cmd->forwardmove * maxspeed;
	f32 add = fabsf(fm * boost);
	f32 maxs = maxspeed + maxspeed * boost;
	f32 speed2d = sqrtf(pl->velocity[0] * pl->velocity[0] + pl->velocity[1] * pl->velocity[1]);
	if (add + speed2d > maxs)
		add -= add + speed2d - maxs;
	if (fm < 0)
		add = -add;
	pl->velocity[0] += fwd[0] * add;
	pl->velocity[1] += fwd[1] * add;

	finish_gravity(pl);
	pl->oldButtons |= IN_JUMP;
}

static int fits(Player *pl, const f32 *origin, int ducked)
{
	int was = pl->ducked;
	pl->ducked = ducked;
	Trace tr;
	trace_hull(pl, origin, origin, &tr);
	pl->ducked = was;
	return !tr.startsolid;
}

static void duck(Player *pl, const UserCmd *cmd)
{
	const f32 hullDiff = HULL_MAX[2] - DUCK_MAX[2];
	if ((cmd->buttons & IN_DUCK) || pl->forceDuck) {
		if (!pl->ducked) {
			if (!pl->onGround) {
				/* crouch-jump: pull the legs up */
				f32 o[3] = {pl->origin[0], pl->origin[1], pl->origin[2] + hullDiff};
				if (fits(pl, o, 1)) {
					memcpy(pl->origin, o, sizeof(o));
					pl->ducked = 1;
					pl->duckAmount = 1;
				}
			} else {
				pl->duckAmount += TICK_INTERVAL / DUCK_TIME;
				if (pl->duckAmount >= 1) {
					pl->duckAmount = 1;
					pl->ducked = 1;
				}
			}
		}
	} else if (pl->ducked || pl->duckAmount > 0) {
		if (pl->ducked) {
			f32 o[3] = {pl->origin[0], pl->origin[1], pl->origin[2]};
			if (!pl->onGround)
				o[2] -= hullDiff;
			if (!fits(pl, o, 0))
				return; /* no headroom: stay crouched */
			memcpy(pl->origin, o, sizeof(o));
			pl->ducked = 0;
		}
		pl->duckAmount -= TICK_INTERVAL / (DUCK_TIME * 0.5f);
		if (pl->duckAmount < 0)
			pl->duckAmount = 0;
	}
}

static void noclip_move(Player *pl, const UserCmd *cmd)
{
	f32 fwd[3], right[3];
	angle_vectors(pl->pitch, pl->yaw, fwd, right);
	f32 speed = (cmd->buttons & IN_SPEED) ? 1600.0f : 600.0f;
	for (int i = 0; i < 3; i++)
		pl->velocity[i] = (fwd[i] * cmd->forwardmove + right[i] * cmd->sidemove) * speed;
	if (cmd->buttons & IN_JUMP)
		pl->velocity[2] += speed;
	if (cmd->buttons & IN_DUCK)
		pl->velocity[2] -= speed;
	for (int i = 0; i < 3; i++)
		pl->origin[i] += pl->velocity[i] * TICK_INTERVAL;
}

/* ------------------------------------------------------------------ public */

int Player_Spawn(Player *pl, const f32 feet[3], f32 pitch, f32 yaw)
{
	memset(pl, 0, sizeof(*pl));
	pl->groundEnt = -1;
	pl->ladder = -1;
	memcpy(pl->origin, feet, sizeof(pl->origin));
	pl->pitch = pitch;
	pl->yaw = yaw;
	pl->surfaceFriction = 1.0f;
	/* if the spawn/arrival point is embedded, search outward for the nearest free spot:
	 * straight up first (most common: feet in the floor), then rings around it */
	int found = fits(pl, pl->origin, 0);
	if (!found) {
		f32 base[3];
		memcpy(base, pl->origin, sizeof(base));
		for (int dz = 1; dz <= 64 && !found; dz++) {
			f32 o[3] = {base[0], base[1], base[2] + dz};
			if (fits(pl, o, 0)) {
				memcpy(pl->origin, o, sizeof(o));
				found = 1;
			}
		}
		for (int r = 16; r <= 256 && !found; r += 16)
			for (int a = 0; a < 16 && !found; a++)
				for (int dz = 0; dz <= 64 && !found; dz += 16) {
					f32 o[3] = {base[0] + r * cosf(a * (f32)M_PI / 8), base[1] + r * sinf(a * (f32)M_PI / 8),
					            base[2] + dz};
					if (fits(pl, o, 0)) {
						memcpy(pl->origin, o, sizeof(o));
						found = 1;
					}
				}
	}
	categorize_position(pl);
	return found;
}


/* ---- ladders (CHL2GameMovement's useable ladders): walking into one while facing it climbs on; forward
 * climbs up when looking ahead or up and down when looking down (200 units/s, like MAX_CLIMB_SPEED); jump
 * lets go; at either end the player steps off to the nearest info_ladder_dismount that's free */
#define MAX_LADDERS 64
#define MAX_DISMOUNTS 256
static f32 ladEnds[MAX_LADDERS][2][3];
static int numLadders;
static f32 ladDismount[MAX_DISMOUNTS][3];
static int numDismount;

void Player_SetLadders(const f32 (*ends)[2][3], int count, const f32 (*dismounts)[3], int numD)
{
	numLadders = count < MAX_LADDERS ? count : MAX_LADDERS;
	memcpy(ladEnds, ends, sizeof(ladEnds[0]) * (size_t)numLadders);
	numDismount = numD < MAX_DISMOUNTS ? numD : MAX_DISMOUNTS;
	memcpy(ladDismount, dismounts, sizeof(ladDismount[0]) * (size_t)numDismount);
}

static int ladder_fits(const Player *pl, const f32 p[3])
{
	f32 mn[3], mx[3];
	Player_Bounds(pl, mn, mx);
	Trace tr;
	CM_BoxTrace(&tr, p, p, mn, mx, MASK_PLAYERSOLID);
	return !tr.startsolid;
}

static void ladder_dismount(Player *pl, int top)
{
	int L = pl->ladder;
	const f32 *end = ladEnds[L][top];
	f32 best = 1e30f;
	int bi = -1;
	for (int i = 0; i < numDismount; i++) {
		f32 d[3] = {ladDismount[i][0] - end[0], ladDismount[i][1] - end[1], ladDismount[i][2] - end[2]};
		f32 dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd < best && dd < 160.0f * 160.0f && ladder_fits(pl, ladDismount[i])) {
			best = dd;
			bi = i;
		}
	}
	if (bi >= 0) {
		memcpy(pl->origin, ladDismount[bi], 12);
	} else {
		/* no spot: step off backwards (bottom) or forwards over the top */
		f32 fx = cosf(pl->yaw * (f32)M_PI / 180.0f), fy = sinf(pl->yaw * (f32)M_PI / 180.0f);
		f32 s = top ? 24.0f : -24.0f;
		f32 p[3] = {end[0] + fx * s, end[1] + fy * s, end[2] + (top ? 8.0f : 0.0f)};
		if (ladder_fits(pl, p))
			memcpy(pl->origin, p, 12);
	}
	pl->ladder = -1;
	pl->ladderCooldown = 0.5f;
	memset(pl->velocity, 0, sizeof(pl->velocity));
}

/* returns 1 if the player is on a ladder this tick (movement handled) */
static int ladder_move(Player *pl, const UserCmd *cmd)
{
	if (pl->ladderCooldown > 0)
		pl->ladderCooldown -= TICK_INTERVAL;
	if (pl->ladder < 0) {
		if (pl->ladderCooldown > 0 || cmd->forwardmove <= 0.2f || pl->waterLevel >= 2)
			return 0;
		f32 fx = cosf(pl->yaw * (f32)M_PI / 180.0f), fy = sinf(pl->yaw * (f32)M_PI / 180.0f);
		for (int i = 0; i < numLadders; i++) {
			const f32 *a = ladEnds[i][0], *b = ladEnds[i][1];
			if (pl->origin[2] < a[2] - 24 || pl->origin[2] > b[2] + 8)
				continue;
			f32 len = b[2] - a[2];
			f32 t = len > 1 ? (pl->origin[2] - a[2]) / len : 0;
			t = t < 0 ? 0 : (t > 1 ? 1 : t);
			f32 lx = a[0] + (b[0] - a[0]) * t, ly = a[1] + (b[1] - a[1]) * t;
			f32 dx = lx - pl->origin[0], dy = ly - pl->origin[1];
			f32 d = sqrtf(dx * dx + dy * dy);
			/* close (Source mounts within about a hull's reach) and walking at it */
			if (d > 40.0f || (d > 1.0f && (dx * fx + dy * fy) / d < 0.5f))
				continue;
			if (t > 0.97f && cmd->pitch < 30.0f)
				continue; /* at the top looking ahead: walking away over it, not onto it */
			pl->ladder = i;
			pl->ladderT = t;
			break;
		}
		if (pl->ladder < 0)
			return 0;
	}
	if ((cmd->buttons & IN_JUMP) && !(pl->oldButtons & IN_JUMP)) {
		/* let go, pushed back off the ladder */
		pl->ladder = -1;
		pl->ladderCooldown = 0.5f;
		f32 fx = cosf(pl->yaw * (f32)M_PI / 180.0f), fy = sinf(pl->yaw * (f32)M_PI / 180.0f);
		pl->velocity[0] = -fx * 200;
		pl->velocity[1] = -fy * 200;
		pl->velocity[2] = 0;
		return 0;
	}
	int L = pl->ladder;
	const f32 *a = ladEnds[L][0], *b = ladEnds[L][1];
	f32 len = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
	f32 climb = cmd->forwardmove * (cmd->pitch > 30.0f ? -1.0f : 1.0f) - (cmd->forwardmove < 0 ? 0 : 0);
	f32 nt = pl->ladderT + (len > 1 ? climb * 200.0f * TICK_INTERVAL / len : 0);
	if (nt >= 1.0f) {
		ladder_dismount(pl, 1);
		return 1;
	}
	if (nt <= 0.0f) {
		ladder_dismount(pl, 0);
		return 1;
	}
	f32 p[3];
	for (int k = 0; k < 3; k++)
		p[k] = a[k] + (b[k] - a[k]) * nt;
	if (ladder_fits(pl, p)) {
		memcpy(pl->origin, p, 12);
		pl->ladderT = nt;
	}
	pl->velocity[0] = pl->velocity[1] = 0;
	pl->velocity[2] = climb * 200.0f;
	pl->onGround = 0;
	pl->groundEnt = -1;
	return 1;
}

void Player_Tick(Player *pl, const UserCmd *cmd)
{
	pl->pitch = cmd->pitch;
	pl->yaw = cmd->yaw;
	if (pl->frozen) {
		memset(pl->velocity, 0, sizeof(pl->velocity));
		return;
	}
	if (pl->noclip) {
		noclip_move(pl, cmd);
		pl->oldButtons = cmd->buttons;
		return;
	}

	f32 maxspeed = (cmd->buttons & IN_SPEED) ? SPRINT_SPEED : (cmd->buttons & IN_WALK) ? WALK_SPEED : NORM_SPEED;
	maxspeed *= speedScale; /* player_speedmod */

	if (ladder_move(pl, cmd)) {
		pl->oldButtons = cmd->buttons;
		return;
	}
	duck(pl, cmd);
	check_water(pl);
	if (pl->waterJumpTime > 0) {
		/* hopping out of the water: keep the push until we land or time runs out */
		pl->waterJumpTime -= TICK_INTERVAL;
		pl->velocity[0] = pl->waterJumpVel[0];
		pl->velocity[1] = pl->waterJumpVel[1];
		start_gravity(pl);
		try_player_move(pl);
		finish_gravity(pl);
		categorize_position(pl);
		if (pl->onGround || pl->waterLevel == 0)
			pl->waterJumpTime = 0;
		pl->oldButtons = cmd->buttons;
		return;
	}
	if (pl->waterLevel >= 2) {
		if (pl->waterLevel == 2)
			check_water_jump(pl, cmd);
		if (pl->waterJumpTime <= 0) {
			pl->onGround = 0;
			pl->groundEnt = -1;
			water_move(pl, cmd, maxspeed > NORM_SPEED ? NORM_SPEED : maxspeed);
			categorize_position(pl);
			check_velocity(pl);
		}
		pl->oldButtons = cmd->buttons;
		return;
	}
	start_gravity(pl);
	if (cmd->buttons & IN_JUMP)
		check_jump(pl, cmd, maxspeed);
	else
		pl->oldButtons &= ~IN_JUMP;

	if (pl->onGround) {
		pl->velocity[2] = 0;
		friction(pl);
	}
	check_velocity(pl);
	if (pl->onGround)
		walk_move(pl, cmd, maxspeed);
	else
		air_move(pl, cmd, maxspeed);
	categorize_position(pl);
	check_velocity(pl);
	finish_gravity(pl);
	if (pl->onGround)
		pl->velocity[2] = 0;
}
