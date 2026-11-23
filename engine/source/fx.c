#include "fx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { OP_SINE, OP_SCROLL, OP_ANIM, OP_RAMP, OP_NOISE, OP_ABS, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_COPY,
       OP_CLAMP, OP_LESSEQ, OP_XFORM, OP_RAND };

#define S_COLOR 0
#define S_ALPHA 3
#define S_FRAME 4
#define S_XF1 5
#define S_XF2 10

static const Pack *fxPack;
static f32 *vars;       /* every program's variables, laid out as in MFXV */
static s16 *fxOfMat;    /* material -> effect index, -1 none */
static u32 numMats;
static FxState *state;
static u32 rng = 0x1234567u;

static f32 frand(void)
{
	rng = rng * 1664525u + 1013904223u;
	return (f32)((rng >> 8) & 0xFFFF) / 65535.0f;
}

/* (a Gaussian from the sum of three uniforms is close enough for TV static) */
static f32 gauss(f32 mean, f32 halfwidth)
{
	f32 s = frand() + frand() + frand() - 1.5f;
	return mean + s * halfwidth * 1.15f;
}

void Fx_Shutdown(void)
{
	free(vars);
	free(fxOfMat);
	free(state);
	vars = NULL;
	fxOfMat = NULL;
	state = NULL;
	fxPack = NULL;
	numMats = 0;
}

void Fx_Init(const Pack *p)
{
	Fx_Shutdown();
	if (!p || !p->numMatFx || !p->matFx || !p->numMats)
		return;
	vars = malloc(p->numFxVars * sizeof(f32) + 4);
	fxOfMat = malloc(p->numMats * sizeof(s16));
	state = calloc(p->numMatFx, sizeof(FxState));
	if (!vars || !fxOfMat || !state) {
		Fx_Shutdown();
		return;
	}
	memcpy(vars, p->fxVars, p->numFxVars * sizeof(f32));
	numMats = p->numMats;
	for (u32 i = 0; i < numMats; i++)
		fxOfMat[i] = -1;
	for (u32 i = 0; i < p->numMatFx; i++) {
		const PackMatFx *f = &p->matFx[i];
		if (f->mat < numMats && f->firstVar + f->numVars <= p->numFxVars && f->firstOp + f->numOps <= p->numFxOps &&
		    f->numVars >= 15)
			fxOfMat[f->mat] = (s16)i;
	}
	fxPack = p;
	Fx_Tick(0);
}

static void xform(f32 out[2][4], const f32 *v)
{
	/* scale (s, t) and rotation about the centre (0.5, 0.5), then translation, like Source's
	 * TextureTransform: M = T(translate) * T(c) * R * S * T(-c) */
	f32 sx = v[0], sy = v[1], r = v[2] * (f32)M_PI / 180.0f, tx = v[3], ty = v[4];
	f32 c = cosf(r), s = sinf(r);
	f32 a = c * sx, b = -s * sy, d = s * sx, e = c * sy;
	out[0][0] = a;
	out[0][1] = b;
	out[0][2] = 0;
	out[0][3] = 0.5f - (a * 0.5f + b * 0.5f) + tx;
	out[1][0] = d;
	out[1][1] = e;
	out[1][2] = 0;
	out[1][3] = 0.5f - (d * 0.5f + e * 0.5f) + ty;
}

static f32 wrap1(f32 x)
{
	x = fmodf(x, 1.0f);
	return x;
}

void Fx_Tick(f32 t)
{
	const Pack *p = fxPack;
	if (!p || !state)
		return;
	for (u32 i = 0; i < p->numMatFx; i++) {
		const PackMatFx *f = &p->matFx[i];
		f32 *v = &vars[f->firstVar];
		u32 nv = f->numVars;
#define V(k) ((k) < nv ? v[(k)] : 0.0f)
		for (u32 o = 0; o < f->numOps; o++) {
			const PackFxOp *op = &p->fxOps[f->firstOp + o];
			if (op->dst >= nv)
				continue;
			f32 *dst = &v[op->dst];
			switch (op->op) {
			case OP_SINE: {
				f32 mn = V(op->a), mx = V(op->b), per = V(op->c), off = V(op->d);
				if (per == 0)
					per = 1;
				*dst = mn + (mx - mn) * (sinf(2.0f * (f32)M_PI * (t - off) / per) * 0.5f + 0.5f);
				break;
			}
			case OP_SCROLL:
				if (op->dst + 4 < nv) {
					v[op->dst + 3] = wrap1(op->k0 * t);
					v[op->dst + 4] = wrap1(op->k1 * t);
				}
				break;
			case OP_ANIM: {
				u32 n = op->k1 > 1 ? (u32)op->k1 : 1;
				*dst = (f32)((u32)(t * op->k0) % n);
				break;
			}
			case OP_RAMP:
				*dst = V(op->b) + V(op->a) * t;
				break;
			case OP_NOISE: {
				f32 x = gauss(V(op->a), V(op->b));
				f32 lo = V(op->c), hi = V(op->d);
				*dst = x < lo ? lo : x > hi ? hi : x;
				break;
			}
			case OP_RAND:
				*dst = V(op->a) + (V(op->b) - V(op->a)) * frand();
				break;
			case OP_ABS:
				*dst = fabsf(V(op->a));
				break;
			case OP_ADD:
				*dst = V(op->a) + V(op->b);
				break;
			case OP_SUB:
				*dst = V(op->a) - V(op->b);
				break;
			case OP_MUL:
				*dst = V(op->a) * V(op->b);
				break;
			case OP_DIV:
				*dst = V(op->b) != 0 ? V(op->a) / V(op->b) : 0;
				break;
			case OP_COPY:
				*dst = V(op->a);
				break;
			case OP_CLAMP: {
				f32 x = V(op->a), lo = V(op->b), hi = V(op->c);
				*dst = x < lo ? lo : x > hi ? hi : x;
				break;
			}
			case OP_LESSEQ:
				*dst = V(op->a) <= V(op->b) ? V(op->c) : V(op->d);
				break;
			case OP_XFORM:
				if (op->dst + 4 < nv) {
					v[op->dst + 0] = V(op->c);
					v[op->dst + 1] = V(op->d);
					v[op->dst + 2] = V((u32)op->k0);
					v[op->dst + 3] = V(op->a);
					v[op->dst + 4] = V(op->b);
				}
				break;
			default:
				break;
			}
		}
		FxState *s = &state[i];
		s->flags = f->flags;
		u32 frame = 0;
		if (f->frames > 1) {
			f32 fr = V(S_FRAME);
			frame = fr > 0 ? (u32)fr % f->frames : 0;
		}
		const PackMat *m = &p->mats[f->mat];
		s->tex = (f->frame0 > 0 && f->frames > 1) ? (s16)(f->frame0 + (s16)frame) : m->tex;
		s->tex2 = f->tex2;
		for (int k = 0; k < 3; k++) {
			f32 c = V(S_COLOR + k);
			s->color[k] = c < 0 ? 0 : c > 4 ? 4 : c;
		}
		f32 a = V(S_ALPHA);
		s->alpha = a < 0 ? 0 : a > 1 ? 1 : a;
		xform(s->xf1, &v[S_XF1]);
		xform(s->xf2, &v[S_XF2]);
#undef V
	}
}

const FxState *Fx_Of(const Pack *p, u32 mat)
{
	if (!fxOfMat || p != fxPack || mat >= numMats || fxOfMat[mat] < 0)
		return NULL;
	return &state[fxOfMat[mat]];
}
