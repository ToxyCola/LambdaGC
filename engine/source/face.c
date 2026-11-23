#include "face.h"

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include <ogc/cache.h>

#define SLOTS 4
#define MAX_DESC 160
#define FACE_DISTANCE 900.0f
#define LIP_BLEND 0.07f

typedef struct {
	u32 firstVert, numVerts;   /* the model's vertex range in NVTX */
	s8 *phonCtl;               /* phoneme key -> controller index (-1 none) */
	s16 blink;
} ModelFace;

static const Pack *P;
static ModelFace *models;
static PackNpcVert *slot[SLOTS];
static u32 slotVerts;
static int slotsUsed;
static f32 frameNow;

static const char *pstr(u32 off) { return P->entStrings + off; }

static const PackNpcFace *face_of(u32 model)
{
	if (!P->npcFaces || model >= P->numNpcModels)
		return NULL;
	const PackNpcFace *f = &P->npcFaces[model];
	return f->numCtl ? f : NULL;
}

static s16 find_ctl(const PackNpcFace *f, const char *name)
{
	for (u32 i = 0; i < f->numCtl && i < NPC_MAX_FLEXCTL; i++)
		if (!strcasecmp(pstr(P->flexCtls[f->firstCtl + i].name), name))
			return (s16)i;
	return -1;
}

int Face_Init(const Pack *p)
{
	Face_Free();
	P = p;
	if (!p->npcFaces || !p->numNpcModels)
		return 0;
	models = calloc(p->numNpcModels, sizeof(ModelFace));
	if (!models)
		return -1;
	u32 maxVerts = 0;
	for (u32 m = 0; m < p->numNpcModels; m++) {
		const PackNpcModel *nm = &p->npcModels[m];
		ModelFace *mf = &models[m];
		mf->blink = -1;
		if (!nm->numBatches)
			continue;
		const PackNpcBatch *b0 = &p->npcBatches[nm->firstBatch];
		const PackNpcBatch *b1 = &p->npcBatches[nm->firstBatch + nm->numBatches - 1];
		mf->firstVert = b0->firstVert;
		mf->numVerts = b1->firstVert + b1->numVerts - b0->firstVert;
		const PackNpcFace *f = face_of(m);
		if (!f || !f->numFlex)
			continue;
		if (mf->numVerts > maxVerts)
			maxVerts = mf->numVerts;
		mf->blink = find_ctl(f, "blink");
		if (p->numPhonKeys) {
			mf->phonCtl = malloc(p->numPhonKeys);
			for (u32 k = 0; mf->phonCtl && k < p->numPhonKeys; k++)
				mf->phonCtl[k] = (s8)find_ctl(f, pstr(p->phonKeys[k]));
		}
	}
	slotVerts = maxVerts;
	for (int s = 0; s < SLOTS && maxVerts; s++)
		slot[s] = memalign(32, (maxVerts * sizeof(PackNpcVert) + 31) & ~31u);
	return 0;
}

void Face_Free(void)
{
	if (models && P)
		for (u32 m = 0; m < P->numNpcModels; m++)
			free(models[m].phonCtl);
	free(models);
	models = NULL;
	for (int s = 0; s < SLOTS; s++) {
		free(slot[s]);
		slot[s] = NULL;
	}
	slotVerts = 0;
}

void Face_SetController(Npc *n, const char *name, f32 value)
{
	const PackNpcFace *f = P ? face_of(n->model) : NULL;
	if (!f)
		return;
	s16 c = find_ctl(f, name);
	if (c >= 0)
		n->flexCtl[c] = value;
}

void Face_BeginFrame(f32 now)
{
	slotsUsed = 0;
	frameNow = now;
}

static f32 remap_clamped(f32 v, f32 a, f32 b, f32 c, f32 d)
{
	f32 t = (b == a) ? 0 : (v - a) / (b - a);
	if (t < 0)
		t = 0;
	if (t > 1)
		t = 1;
	return c + (d - c) * t;
}

/* controllers -> flex desc weights (Source's flex rule stack machine) */
static void run_rules(const PackNpcFace *f, const f32 *ctl, f32 *desc)
{
	u32 nd = f->numDesc < MAX_DESC ? f->numDesc : MAX_DESC;
	memset(desc, 0, nd * sizeof(f32));
	for (u32 r = 0; r < f->numRules; r++) {
		const PackFlexRule *rule = &P->flexRules[f->firstRule + r];
		f32 st[32];
		int k = 0;
		for (u32 o = 0; o < rule->numOps; o++) {
			const PackFlexOp *op = &P->flexOps[rule->firstOp + o];
			int idx = (int)op->value;
			switch (op->op) {
			case FLOP_CONST:
				if (k < 32)
					st[k++] = op->value;
				break;
			case FLOP_FETCH1:
				if (k < 32)
					st[k++] = (idx >= 0 && idx < NPC_MAX_FLEXCTL) ? ctl[idx] : 0;
				break;
			case FLOP_FETCH2:
				if (k < 32)
					st[k++] = (idx >= 0 && (u32)idx < nd) ? desc[idx] : 0;
				break;
			case FLOP_ADD:
				if (k >= 2) { st[k - 2] += st[k - 1]; k--; }
				break;
			case FLOP_SUB:
				if (k >= 2) { st[k - 2] -= st[k - 1]; k--; }
				break;
			case FLOP_MUL:
				if (k >= 2) { st[k - 2] *= st[k - 1]; k--; }
				break;
			case FLOP_DIV:
				if (k >= 2) { st[k - 2] = st[k - 1] != 0 ? st[k - 2] / st[k - 1] : 0; k--; }
				break;
			case FLOP_NEG:
				if (k >= 1)
					st[k - 1] = -st[k - 1];
				break;
			case FLOP_MAX:
				if (k >= 2) { st[k - 2] = st[k - 2] > st[k - 1] ? st[k - 2] : st[k - 1]; k--; }
				break;
			case FLOP_MIN:
				if (k >= 2) { st[k - 2] = st[k - 2] < st[k - 1] ? st[k - 2] : st[k - 1]; k--; }
				break;
			case FLOP_2WAY_0:
				if (k < 32)
					st[k++] = remap_clamped(idx >= 0 && idx < NPC_MAX_FLEXCTL ? ctl[idx] : 0, -1, 0, 1, 0);
				break;
			case FLOP_2WAY_1:
				if (k < 32)
					st[k++] = remap_clamped(idx >= 0 && idx < NPC_MAX_FLEXCTL ? ctl[idx] : 0, 0, 1, 0, 1);
				break;
			case FLOP_NWAY:
				if (k >= 5) {
					int vi = (int)st[k - 1];
					f32 v = (vi >= 0 && vi < NPC_MAX_FLEXCTL) ? ctl[vi] : 0;
					f32 w0 = st[k - 5], w1 = st[k - 4], w2 = st[k - 3], w3 = st[k - 2];
					if (v <= w0 || v >= w3)
						v = 0;
					else if (v < w1)
						v = (v - w0) / (w1 - w0);
					else if (v > w2)
						v = (w3 - v) / (w3 - w2);
					else
						v = 1;
					st[k - 5] = v * (idx >= 0 && idx < NPC_MAX_FLEXCTL ? ctl[idx] : 0);
					k -= 4;
				}
				break;
			case FLOP_COMBO:
				if (idx > 0 && k >= idx) {
					f32 v = 1;
					for (int j = 0; j < idx; j++)
						v *= st[--k];
					st[k++] = v;
				}
				break;
			case FLOP_DOMINATE:
				if (idx > 0 && k > idx) {
					f32 dm = 1;
					for (int j = 0; j < idx; j++)
						dm *= st[--k];
					st[k - 1] *= 1 - dm;
				}
				break;
			default:
				break;
			}
		}
		if (rule->flexDesc < nd && k > 0)
			desc[rule->flexDesc] = st[0];
	}
}

static f32 ramp(const PackFlex *fx, f32 w)
{
	if (w <= fx->target[0] || w >= fx->target[3])
		return 0;
	if (w < fx->target[1])
		return (w - fx->target[0]) / (fx->target[1] - fx->target[0]);
	if (w > fx->target[2])
		return (fx->target[3] - w) / (fx->target[3] - fx->target[2]);
	return 1;
}

/* the controllers this frame: the scene's, plus lip sync and blinking */
static void controllers(const Npc *n, const ModelFace *mf, const PackNpcFace *f, f32 *ctl)
{
	u32 nc = f->numCtl < NPC_MAX_FLEXCTL ? f->numCtl : NPC_MAX_FLEXCTL;
	memcpy(ctl, n->flexCtl, nc * sizeof(f32));
	/* blink every few seconds, out of step with everyone else */
	if (mf->blink >= 0) {
		f32 period = 3.0f + (n->ent % 7) * 0.4f;
		f32 t = fmodf(frameNow + n->ent * 1.37f, period);
		if (t < 0.18f)
			ctl[mf->blink] = sinf(t / 0.18f * (f32)M_PI);
	}
	/* lip sync from the line's phonemes */
	if (mf->phonCtl && n->speakSound >= 0 && P->soundPhon && (u32)n->speakSound < P->numSounds &&
	    frameNow < n->speakUntil) {
		const PackSoundPhon *sp = &P->soundPhon[n->speakSound];
		f32 t = frameNow - n->speakStart;
		u32 stride = 4 + P->numPhonKeys * 4;
		for (u32 i = 0; i < sp->count; i++) {
			const PackPhoneme *ph = &P->phonemeRecs[sp->first + i];
			if (t < ph->start - LIP_BLEND || t > ph->end + LIP_BLEND || ph->phon >= P->numPhonemes)
				continue;
			f32 w = 1;
			if (t < ph->start)
				w = 1 - (ph->start - t) / LIP_BLEND;
			else if (t > ph->end)
				w = 1 - (t - ph->end) / LIP_BLEND;
			const f32 *weights = (const f32 *)(P->phonemes + ph->phon * stride + 4);
			for (u32 k = 0; k < P->numPhonKeys; k++) {
				s8 c = mf->phonCtl[k];
				if (c >= 0 && weights[k] >= 0)
					ctl[c] += weights[k] * w;
			}
		}
	}
	for (u32 i = 0; i < nc; i++) {
		const PackFlexCtl *fc = &P->flexCtls[f->firstCtl + i];
		if (ctl[i] < fc->min)
			ctl[i] = fc->min;
		if (ctl[i] > fc->max)
			ctl[i] = fc->max;
	}
}

const PackNpcVert *Face_Morph(const Npc *n, f32 distance, u32 *base)
{
	if (!models || distance > FACE_DISTANCE || slotsUsed >= SLOTS)
		return NULL;
	const PackNpcFace *f = face_of(n->model);
	ModelFace *mf = &models[n->model];
	if (!f || !f->numFlex || mf->numVerts > slotVerts || !slot[slotsUsed])
		return NULL;
	static f32 ctl[NPC_MAX_FLEXCTL], desc[MAX_DESC];
	controllers(n, mf, f, ctl);
	run_rules(f, ctl, desc);
	PackNpcVert *out = slot[slotsUsed++];
	memcpy(out, &P->npcVerts[mf->firstVert], mf->numVerts * sizeof(PackNpcVert));
	u32 nd = f->numDesc < MAX_DESC ? f->numDesc : MAX_DESC;
	for (u32 i = 0; i < f->numFlex; i++) {
		const PackFlex *fx = &P->flexes[f->firstFlex + i];
		if (fx->desc < 0 || (u32)fx->desc >= nd)
			continue;
		f32 w1 = ramp(fx, desc[fx->desc]);
		f32 w2 = (fx->pair >= 0 && (u32)fx->pair < nd) ? ramp(fx, desc[fx->pair]) : w1;
		if (fabsf(w1) < 0.001f && fabsf(w2) < 0.001f)
			continue;
		for (u32 v = 0; v < fx->numVA; v++) {
			const PackFlexVA *va = &P->flexVAs[fx->firstVA + v];
			u32 k = va->vert - mf->firstVert;
			if (k >= mf->numVerts)
				continue;
			f32 s = va->side * (1.0f / 255.0f);
			f32 w = (fx->pair >= 0) ? w1 * (1 - s) + w2 * s : w1;
			w *= 1.0f / 4096.0f;
			out[k].pos[0] += va->d[0] * w;
			out[k].pos[1] += va->d[1] * w;
			out[k].pos[2] += va->d[2] * w;
		}
	}
	DCFlushRange(out, mf->numVerts * sizeof(PackNpcVert));
	*base = mf->firstVert;
	return out;
}
