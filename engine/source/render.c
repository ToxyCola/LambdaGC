#include "render.h"
#include "collide.h"
#include "light.h"
#include "fx.h"

#include <malloc.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "ent.h"
#include "npc.h"
#include "face.h"


/* 3x4 matrix product in plain C. Do NOT use libogc's guMtxConcat (ps_guMtxConcat): it loads its
 * {0,1} constant with `psq_l UNIT01,Unit01@sdarel(r13)`, but psq_l only has a 12-bit offset. The
 * linker writes a 16-bit small-data offset there, so in our DOL the top bits land in the W flag and
 * the instruction reads {<some .sbss variable>, 1.0} instead of {0, 1}. The product's third column
 * then picks up that variable times the view translation: every prop, NPC and the train came out as
 * long stretched slabs that changed as the camera moved. Verified by decoding the shipped main.dol. */
static void mtx_concat(const f32 a[3][4], const f32 b[3][4], f32 ab[3][4])
{
	f32 t[3][4];
	for (int r = 0; r < 3; r++) {
		t[r][0] = a[r][0] * b[0][0] + a[r][1] * b[1][0] + a[r][2] * b[2][0];
		t[r][1] = a[r][0] * b[0][1] + a[r][1] * b[1][1] + a[r][2] * b[2][1];
		t[r][2] = a[r][0] * b[0][2] + a[r][1] * b[1][2] + a[r][2] * b[2][2];
		t[r][3] = a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3];
	}
	memcpy(ab, t, sizeof(t));
}

static GXTexObj *texObjs;
static u32 numTexObjs;
static GXTexObj neutralLm; /* mid-grey: x2 in TEV = unlit */
static u8 neutralLmData[32] ATTRIBUTE_ALIGN(32);
static u32 curSegment;

/* the _rt_Camera render target */
#define CAM_W 256
#define CAM_H 256 /* Source's _rt_Camera is 256x256; screens tile it with their own texture coordinates */
static u8 camData[CAM_W * CAM_H * 2] ATTRIBUTE_ALIGN(32);
static GXTexObj camTex;
/* water: the frame so far, copied at half size before the water is drawn, looked up through the
 * surface (refraction) with a scrolling distortion map */
static u16 *refrData;
static GXTexObj refrTex;
static u16 scrW = 640, scrH = 480;
static u8 refrValid;
static f32 waterClock;
static Mtx refrMtx; /* world -> projective screen texture coordinates */
static const Camera *frameCam;
static int camValid;
static int curLm;

/* per-map caches (declared again further down, where they're used); a new map loads at the same
 * address as the last one, so they're dropped here rather than noticed by a pointer change */
static f32 (*spRot)[3][3];
static const Pack *spRotPack;
static const Pack *entCubePack;
static u32 posFrac = 8; /* the model vertex format's position fraction bits now */

void Render_Free(void)
{
	free(spRot);
	spRot = NULL;
	spRotPack = NULL;
	entCubePack = NULL;
	numTexObjs = 0;
	free(texObjs);
	texObjs = NULL;
}

int Render_Init(const Pack *p)
{
	Render_Free();
	texObjs = memalign(32, p->numTex * sizeof(GXTexObj));
	if (!texObjs)
		return -1;
	numTexObjs = p->numTex;
	for (u32 i = 0; i < p->numTex; i++) {
		const PackTexInfo *t = &p->tex[i];
		u8 wrap = t->clamp ? GX_CLAMP : GX_REPEAT;
		u8 mip = t->levels > 1 ? GX_TRUE : GX_FALSE;
		GX_InitTexObj(&texObjs[i], p->texData + t->offset, t->w, t->h, t->fmt, wrap, wrap, mip);
		if (mip)
			GX_InitTexObjLOD(&texObjs[i], GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, (f32)(t->levels - 1), 0.0f,
			                 GX_FALSE, GX_FALSE, GX_ANISO_1);
	}

	/* 4x4 RGB565 tile of 0x8410 (128,128,128) */
	for (int i = 0; i < 16; i++) {
		neutralLmData[i * 2] = 0x84;
		neutralLmData[i * 2 + 1] = 0x10;
	}
	DCFlushRange(neutralLmData, sizeof(neutralLmData));
	GX_InitTexObj(&neutralLm, neutralLmData, 4, 4, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);

	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	/* studio models: 16-bit positions (fraction bits per model), texture coordinates in 1/1024, normals */
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_S16, 8); /* (fraction set per model) */
	posFrac = 8;
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST, GX_S16, 10);
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 6);
	/* NPCs: same, plus a per-vertex matrix index (rigid skinning through GX's matrix palette) */
	GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_TEX0, GX_TEX_ST, GX_S16, 10);
	GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 6);
	return 0;
}

/* ------------------------------------------------------------------ vertex arrays */

static void world_vtxdesc(void)
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX16);
	curSegment = 0xFFFFFFFF;
}

/* 16-bit indices reach 64K vertices, so the arrays slide in 32K windows */
static inline void set_segment(const Pack *p, u32 base)
{
	if (base == curSegment)
		return;
	u8 *v = (u8 *)&p->verts[base];
	GX_SetArray(GX_VA_POS, v, sizeof(PackVert));
	GX_SetArray(GX_VA_TEX0, v + 12, sizeof(PackVert));
	GX_SetArray(GX_VA_TEX1, v + 20, sizeof(PackVert));
	GX_SetArray(GX_VA_CLR0, v + 28, sizeof(PackVert));
	curSegment = base;
}

static void draw_face(const Pack *p, const PackFace *f, RenderStats *st)
{
	u32 base = f->firstvert & ~0x7FFFu;
	set_segment(p, base);
	if (f->lmtex != curLm) {
		GX_LoadTexObj(f->lmtex == NO_LIGHTMAP ? &neutralLm : &texObjs[f->lmtex], GX_TEXMAP1);
		curLm = f->lmtex;
	}
	u16 i0 = (u16)(f->firstvert - base);
	GX_Begin(f->prim == PRIM_FAN ? GX_TRIANGLEFAN : GX_TRIANGLES, GX_VTXFMT0, f->numverts);
	for (u16 i = 0; i < f->numverts; i++) {
		u16 k = i0 + i;
		GX_Position1x16(k);
		GX_Color1x16(k);
		GX_TexCoord1x16(k);
		GX_TexCoord1x16(k);
	}
	GX_End();
	st->faces++;
	st->verts += f->numverts;
	st->worldVerts += f->numverts;
}

/* ------------------------------------------------------------------ TEV setups */

static void tev_stage_tex(u8 stage, u8 coord, u8 map)
{
	GX_SetTevOrder(stage, coord, map, GX_COLOR0A0);
	GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

/* prev * lightmap * 2  (Source's LDR overbright factor) */
static void tev_stage_lightmap(u8 stage)
{
	GX_SetTevOrder(stage, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void set_blend(u8 kind);
static void lit_channel(void);
static void no_indirect(void);

/* a material with effects (fx.c): its frame's texture moved by its texture transforms, times $texture2
 * (UnlitTwoTexture), lit (lightmap / model lighting) unless unlit, plus base x base alpha if self-lit,
 * times $color and $alpha. world: TEXCOORD1 / TEXMAP1 are the lightmap's; models: COLOR0 is their light */
static void fx_material(const PackMat *m, const FxState *fx, int model)
{
	no_indirect();
	if (model && !(fx->flags & FX_UNLIT))
		lit_channel();
	else
		GX_SetNumChans(1);
	int tex = fx->tex >= 0 && (u32)fx->tex < numTexObjs ? fx->tex : -1;
	int two = (fx->flags & FX_TWOTEX) && fx->tex2 >= 0 && (u32)fx->tex2 < numTexObjs;
	GX_SetNumTexGens(3);
	if (fx->flags & FX_XFORM) {
		GX_LoadTexMtxImm(*(Mtx *)fx->xf1, GX_TEXMTX7, GX_MTX2x4);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX7);
	} else
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, model ? GX_TG_TEX0 : GX_TG_TEX1, GX_IDENTITY);
	if (fx->flags & FX_XFORM2) {
		GX_LoadTexMtxImm(*(Mtx *)fx->xf2, GX_TEXMTX8, GX_MTX2x4);
		GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX8);
	} else
		GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	u8 st = 0;
	if (m->kind == MAT_CAMERA) {
		/* a monitor (UnlitTwoTexture on _rt_Camera): the camera's picture, or a dark screen without one;
		 * the scanlines / static of $texture2, the proxies' colour and scrolling go on top as for any
		 * material */
		if (camValid) {
			GX_LoadTexObj(&camTex, GX_TEXMAP0);
			GX_SetTevOrder(st, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
			GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
		} else {
			GX_SetTevColor(GX_TEVREG0, (GXColor){12, 14, 16, 255});
			GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
			GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
		}
		GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		GX_SetTevKAlphaSel(st, GX_TEV_KASEL_1);
		GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		st++;
		tex = -1; /* (no self-illum from it) */
	} else if (tex >= 0) {
		GX_LoadTexObj(&texObjs[tex], GX_TEXMAP0);
		tev_stage_tex(st++, GX_TEXCOORD0, GX_TEXMAP0);
	} else {
		GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ONE);
		GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		GX_SetTevKAlphaSel(st, GX_TEV_KASEL_1);
		GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		st++;
	}
	if (two) {
		GX_LoadTexObj(&texObjs[fx->tex2], GX_TEXMAP2);
		GX_SetTevOrder(st, GX_TEXCOORD2, GX_TEXMAP2, GX_COLORNULL);
		GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
		GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_APREV, GX_CA_TEXA, GX_CA_ZERO);
		GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		st++;
	}
	if (!(fx->flags & FX_UNLIT)) {
		if (model) {
			GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
			GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
			GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
			GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
			GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
			st++;
		} else
			tev_stage_lightmap(st++);
	}
	if ((fx->flags & FX_SELFILLUM) && tex >= 0) {
		/* + base x its alpha (the self-illumination mask) */
		GX_SetTevOrder(st, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
		GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_TEXC, GX_CC_TEXA, GX_CC_CPREV);
		GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
		GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		st++;
	}
	/* x $color (up to 2: half in the constant, doubled), x $alpha */
	f32 c[3];
	for (int k = 0; k < 3; k++)
		c[k] = fx->color[k] * 127.5f > 255 ? 255 : fx->color[k] * 127.5f;
	GX_SetTevKColor(GX_KCOLOR3, (GXColor){(u8)c[0], (u8)c[1], (u8)c[2], (u8)(fx->alpha * 255)});
	GX_SetTevKColorSel(st, GX_TEV_KCSEL_K3);
	GX_SetTevKAlphaSel(st, GX_TEV_KASEL_K3_A);
	GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_APREV, GX_CA_KONST, GX_CA_ZERO);
	GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	st++;
	GX_SetNumTevStages(st);
	set_blend(m->kind);
}

static const PackWater *water_of(const Pack *p, const PackMat *m)
{
	u16 mi = (u16)(m - p->mats);
	for (u32 i = 0; i < p->numWater; i++)
		if (p->water[i].mat == mi)
			return &p->water[i];
	return NULL;
}

/* the water surface: refraction of what's under it, rippled, sinking into the water's fog colour */
static void water_material(const Pack *p, const PackMat *m)
{
	const PackWater *wt = water_of(p, m);
	u8 fog[3] = {m->r, m->g, m->b}, tint[3] = {255, 255, 255};
	f32 mix = 0.55f;
	if (wt) {
		memcpy(fog, wt->fog, 3);
		memcpy(tint, wt->tint, 3);
		/* murky water (short fog) hides more of what's below */
		f32 fe = wt->fogEnd > 1 ? wt->fogEnd : 400;
		mix = 1.0f - fe / 900.0f;
		mix = mix < 0.25f ? 0.25f : mix > 0.85f ? 0.85f : mix;
	}
	GX_SetNumChans(0);
	if (!refrValid || !wt || wt->normalTex < 0 || (u32)wt->normalTex >= p->numTex) {
		GX_SetNumTexGens(0);
		GX_SetNumIndStages(0);
		GX_SetNumTevStages(1);
		GX_SetTevColor(GX_TEVREG0, (GXColor){fog[0], fog[1], fog[2], 200});
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		return;
	}
	/* coordinates: 0 = projective screen position, 1 = scrolling world xy for the ripples */
	GX_SetNumTexGens(2);
	GX_LoadTexMtxImm(refrMtx, GX_TEXMTX1, GX_MTX3x4);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX1);
	Mtx sc;
	memset(sc, 0, sizeof(sc));
	sc[0][0] = 1.0f / 256.0f;
	sc[0][3] = waterClock * 0.04f;
	sc[1][1] = 1.0f / 256.0f;
	sc[1][3] = waterClock * 0.025f;
	sc[2][2] = 1;
	GX_LoadTexMtxImm(sc, GX_TEXMTX2, GX_MTX2x4);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX2);
	GX_LoadTexObj(&refrTex, GX_TEXMAP4);
	GX_LoadTexObj(&texObjs[wt->normalTex], GX_TEXMAP5);
	/* the distortion: the map's two channels (alpha, intensity) offset the screen lookup a few texels */
	GX_SetNumIndStages(1);
	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP5);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
	f32 im[2][3] = {{0.5f, 0.0f, 0.0f}, {0.0f, 0.5f, 0.0f}};
	GX_SetIndTexMatrix(GX_ITM_0, im, -2);
	GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_ST, GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE,
	                  GX_FALSE, GX_ITBA_OFF);
	GX_SetNumTevStages(2);
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){tint[0], tint[1], tint[2], 255});
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevColor(GX_TEVREG0, (GXColor){fog[0], fog[1], fog[2], (u8)(mix * 255)});
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_C0, GX_CC_A0, GX_CC_ZERO); /* lerp to the fog colour */
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

/* everything else samples directly */
static void no_indirect(void)
{
	GX_SetNumIndStages(0);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevDirect(GX_TEVSTAGE2);
}

static int flashlight_textures(void);

void Render_SetScreen(u16 w, u16 h)
{
	scrW = w;
	scrH = h;
	free(refrData);
	refrData = memalign(32, (u32)(w / 2) * (h / 2) * 2);
	if (refrData)
		GX_InitTexObj(&refrTex, refrData, w / 2, h / 2, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	flashlight_textures(); /* up front, like every buffer that lives across levels */
}

static void set_material(const Pack *p, const PackMat *m)
{
	const FxState *fx = m->kind != MAT_WATER ? Fx_Of(p, (u32)(m - p->mats)) : NULL;
	if (fx) {
		fx_material(m, fx, 0);
		curLm = -1;
		return;
	}
	if (m->kind == MAT_WATER) {
		water_material(p, m);
		set_blend(MAT_WATER);
		curLm = -1;
		return;
	}
	no_indirect();
	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

	if (m->kind == MAT_CAMERA) {
		/* the screen glows by itself: no lightmap */
		GX_SetNumTevStages(1);
		if (camValid) {
			GX_LoadTexObj(&camTex, GX_TEXMAP0);
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
			GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
			GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		} else {
			GX_SetTevColor(GX_TEVREG0, (GXColor){12, 14, 16, 255});
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
			GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
			GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
		}
		GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	} else if (m->kind == MAT_WATER || m->tex < 0) {
		GXColor c = {m->r, m->g, m->b, 170};
		GX_SetTevColor(GX_TEVREG0, c);
		GX_SetNumTevStages(1);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	} else if (m->kind == MAT_BLEND && m->tex2 >= 0) {
		GX_LoadTexObj(&texObjs[m->tex], GX_TEXMAP0);
		GX_LoadTexObj(&texObjs[m->tex2], GX_TEXMAP2);
		GX_SetNumTevStages(3);
		tev_stage_tex(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0);
		/* lerp(prev, tex2, vertex alpha) */
		GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP2, GX_COLOR0A0);
		GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_TEXC, GX_CC_RASA, GX_CC_ZERO);
		GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
		GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		tev_stage_lightmap(GX_TEVSTAGE2);
	} else {
		GX_LoadTexObj(&texObjs[m->tex], GX_TEXMAP0);
		GX_SetNumTevStages(2);
		tev_stage_tex(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0);
		tev_stage_lightmap(GX_TEVSTAGE1);
	}

	set_blend(m->kind);
	curLm = -1;
}

static void set_blend(u8 kind)
{
	switch (kind) {
	case MAT_ALPHATEST:
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		GX_SetAlphaCompare(GX_GREATER, 127, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_FALSE);
		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		break;
	case MAT_TRANSLUCENT:
	case MAT_WATER:
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
		break;
	case MAT_ADDITIVE:
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
		break;
	default:
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		break;
	}
}

static int is_translucent(u8 kind)
{
	return kind == MAT_TRANSLUCENT || kind == MAT_ADDITIVE || kind == MAT_WATER;
}

/* ------------------------------------------------------------------ sky */

/* Source skybox faces, in pack order rt lf bk ft up dn. Axes are from the SDK
 * (utils/vbsp/cubemap.cpp copies them 1:1 into cube faces +X -X +Y -Y +Z -Z); the
 * in-face orientation was fitted by matching pixels across face seams. Each row is
 * {forward, right, texture-up}; a face vertex is forward + right*s + up*t. */
static const f32 skyFrame[6][3][3] = {
	{{1, 0, 0}, {0, -1, 0}, {0, 0, 1}},   /* rt */
	{{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   /* lf */
	{{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},    /* bk */
	{{0, -1, 0}, {-1, 0, 0}, {0, 0, 1}},  /* ft */
	{{0, 0, 1}, {0, -1, 0}, {-1, 0, 0}},  /* up */
	{{0, 0, -1}, {0, 1, 0}, {-1, 0, 0}},  /* dn */
};

static void sky_vertex(int side, f32 s, f32 t)
{
	const f32(*fr)[3] = skyFrame[side];
	f32 v[3];
	for (int j = 0; j < 3; j++)
		v[j] = (fr[0][j] + fr[1][j] * s + fr[2][j] * t) * 32.0f;
	GX_Position3f32(v[0], v[1], v[2]);
	GX_TexCoord2f32((s + 1.0f) * 0.5f, (1.0f - t) * 0.5f);
}

static void draw_sky(const Pack *p, Mtx view)
{
	Mtx rot;
	memcpy(rot, view, sizeof(Mtx));
	rot[0][3] = rot[1][3] = rot[2][3] = 0.0f;
	GX_LoadPosMtxImm(rot, GX_PNMTX0);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

	for (int side = 0; side < 6; side++) {
		s16 t = p->sky[side];
		if (t < 0)
			continue;
		GX_LoadTexObj(&texObjs[t], GX_TEXMAP0);
		GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
		sky_vertex(side, -1, -1);
		sky_vertex(side, 1, -1);
		sky_vertex(side, 1, 1);
		sky_vertex(side, -1, 1);
		GX_End();
	}
}

/* ------------------------------------------------------------------ entities and models */

static void model_matrix(Mtx view, const f32 origin[3], const f32 R[3][3], Mtx out)
{
	Mtx m;
	for (int r = 0; r < 3; r++) {
		m[r][0] = R[r][0];
		m[r][1] = R[r][1];
		m[r][2] = R[r][2];
		m[r][3] = origin[r];
	}
	mtx_concat(view, m, out);
}

/* a face's vertices wind clockwise seen from its front (Source's order, kept by the converter) */
static int face_front(const Pack *p, const PackFace *f, const f32 eye[3])
{
	const f32 *a = p->verts[f->firstvert].pos, *b = p->verts[f->firstvert + 1].pos, *c = p->verts[f->firstvert + 2].pos;
	f32 u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	f32 n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
	f32 d = n[0] * (eye[0] - a[0]) + n[1] * (eye[1] - a[1]) + n[2] * (eye[2] - a[2]);
	return d <= 0; /* the cross product points out of the back */
}

static void styles_begin(void);
static void styles_face(const Pack *p, const PackStyleFace *sf, RenderStats *st);
static void styles_end(void);

/* the light style layers of each brush model's faces (Source lights brush entities' faces with their
 * switchable lights like the world's): per bmodel, a run of indices into styleFaces */
static const Pack *bmStylePack;
static u16 *bmStyleStart, *bmStyleIdx;

static void bmodel_styles(const Pack *p)
{
	if (bmStylePack == p)
		return;
	free(bmStyleStart);
	free(bmStyleIdx);
	bmStyleStart = bmStyleIdx = NULL;
	bmStylePack = p;
	if (!p->numStyleFaces || !p->styleFaces || p->numBModels < 2)
		return;
	u16 *owner = calloc(p->numFaces, sizeof(u16));
	bmStyleStart = calloc(p->numBModels + 1, sizeof(u16));
	bmStyleIdx = malloc(p->numStyleFaces * sizeof(u16));
	if (!owner || !bmStyleStart || !bmStyleIdx) {
		free(owner);
		free(bmStyleStart);
		free(bmStyleIdx);
		bmStyleStart = bmStyleIdx = NULL;
		return;
	}
	for (u32 b = 1; b < p->numBModels; b++)
		for (u32 k = 0; k < p->bmodels[b].numFaces; k++) {
			u32 f = p->bmodelFaces[p->bmodels[b].firstFace + k];
			if (f < p->numFaces)
				owner[f] = (u16)b;
		}
	u32 *cnt = calloc(p->numBModels + 1, sizeof(u32));
	if (!cnt) {
		free(owner);
		return;
	}
	for (u32 i = 0; i < p->numStyleFaces; i++)
		if (p->styleFaces[i].face < p->numFaces && owner[p->styleFaces[i].face])
			cnt[owner[p->styleFaces[i].face]]++;
	u32 acc = 0;
	for (u32 b = 0; b < p->numBModels; b++) {
		bmStyleStart[b] = (u16)acc;
		acc += cnt[b];
		cnt[b] = bmStyleStart[b];
	}
	bmStyleStart[p->numBModels] = (u16)acc;
	for (u32 i = 0; i < p->numStyleFaces; i++)
		if (p->styleFaces[i].face < p->numFaces && owner[p->styleFaces[i].face])
			bmStyleIdx[cnt[owner[p->styleFaces[i].face]]++] = (u16)i;
	free(cnt);
	free(owner);
}

static void draw_brush_entities(const Pack *p, Mtx view, int pass, RenderStats *st)
{
	u32 n;
	Entity *ents = Ent_List(&n);
	bmodel_styles(p);
	for (u32 i = 1; i < n; i++) {
		const Entity *e = &ents[i];
		if (!(e->flags & EF_DRAW) || (e->flags & (EF_DISABLED | EF_KILLED)) || e->bmodel <= 0 ||
		    (u32)e->bmodel >= p->numBModels)
			continue;
		const PackBModel *bm = &p->bmodels[e->bmodel];
		Mtx mv;
		model_matrix(view, e->origin, e->R, mv);
		GX_LoadPosMtxImm(mv, GX_PNMTX0);
		/* the eye in the model's space: Source draws a brush model's faces only from their front (the
		 * intro's cameras ride inside small brush boxes, which it never sees) */
		f32 eyeM[3] = {0, 0, 0};
		int cull = frameCam != NULL;
		if (cull) {
			f32 d[3] = {frameCam->origin.x - e->origin[0], frameCam->origin.y - e->origin[1],
			            frameCam->origin.z - e->origin[2]};
			for (int k = 0; k < 3; k++)
				eyeM[k] = e->R[0][k] * d[0] + e->R[1][k] * d[1] + e->R[2][k] * d[2];
		}
		int lastMat = -1;
		for (u32 k = 0; k < bm->numFaces; k++) {
			const PackFace *f = &p->faces[p->bmodelFaces[bm->firstFace + k]];
			if (is_translucent(p->mats[f->mat].kind) != pass)
				continue;
			if (cull && f->prim == PRIM_FAN && f->numverts >= 3 && !face_front(p, f, eyeM))
				continue;
			if (f->mat != lastMat) {
				set_material(p, &p->mats[f->mat]);
				lastMat = f->mat;
			}
			draw_face(p, f, st);
		}
		if (pass == 0 && bmStyleStart && bmStyleStart[e->bmodel] != bmStyleStart[e->bmodel + 1]) {
			u32 a = bmStyleStart[e->bmodel], b = bmStyleStart[e->bmodel + 1], k = a;
			while (k < b && Light_StyleValue(p->styleFaces[bmStyleIdx[k]].style) <= 0.02f)
				k++;
			if (k < b) {
				styles_begin();
				for (; k < b; k++) {
					const PackStyleFace *sf = &p->styleFaces[bmStyleIdx[k]];
					const PackFace *f = &p->faces[sf->face];
					if (cull && f->prim == PRIM_FAN && f->numverts >= 3 && !face_front(p, f, eyeM))
						continue;
					styles_face(p, sf, st);
				}
				styles_end();
			}
		}
	}
}

/* textured model, lit by a per-instance colour (sampled from the map's ambient light) */
static GXTexObj *skinTex;   /* weapons.hgm's textures */
static u32 numSkinTex;

/* Source's eye shader: the sclera with the mesh's texture coordinates, the iris (with alpha) projected
 * from the vertex positions by a texture matrix set per eyeball (Render_EyeMatrix) */
static void eye_material(const PackMat *m, const u8 tint[4])
{
	GX_SetNumChans(0);
	GX_SetNumTexGens(2);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX0);
	GX_SetNumTevStages(3);
	GX_LoadTexObj(&texObjs[m->tex], GX_TEXMAP0);
	GX_LoadTexObj(&texObjs[m->tex2 >= 0 ? m->tex2 : m->tex], GX_TEXMAP1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO); /* lerp by iris alpha */
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){tint[0], tint[1], tint[2], 255});
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	set_blend(MAT_OPAQUE);
}

/* where an eye looks: Source's R_StudioEyeballPosition, in the bind-pose space the vertices are in.
 * skin: the eye bone's skinning matrix (model space); R/origin: the entity; target: world point */
static void eye_matrix(const PackEye *eye, f32 (*skin)[4], const f32 R[3][3], const f32 origin[3],
                       const guVector *target)
{
	/* target into the entity's model space, then back through the bone's skinning to bind pose */
	f32 w[3] = {target->x - origin[0], target->y - origin[1], target->z - origin[2]};
	f32 mdl[3], bind[3];
	for (int k = 0; k < 3; k++)
		mdl[k] = R[0][k] * w[0] + R[1][k] * w[1] + R[2][k] * w[2];
	f32 d[3] = {mdl[0] - skin[0][3], mdl[1] - skin[1][3], mdl[2] - skin[2][3]};
	for (int k = 0; k < 3; k++)
		bind[k] = skin[0][k] * d[0] + skin[1][k] * d[1] + skin[2][k] * d[2];
	f32 def[3] = {-eye->fwd[0], -eye->fwd[1], -eye->fwd[2]};
	f32 f[3] = {bind[0] - eye->org[0], bind[1] - eye->org[1], bind[2] - eye->org[2]};
	f32 l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	if (l < 1e-3f)
		memcpy(f, def, 12);
	else
		for (int k = 0; k < 3; k++)
			f[k] /= l;
	/* only look at it when it's in front of the face; otherwise straight ahead */
	if (f[0] * def[0] + f[1] * def[1] + f[2] * def[2] < 0.5f)
		memcpy(f, def, 12);
	const f32 *up = eye->up;
	f32 r[3];
#define CROSS(a, b, o)                                                                                                  \
	do {                                                                                                                \
		o[0] = a[1] * b[2] - a[2] * b[1];                                                                               \
		o[1] = a[2] * b[0] - a[0] * b[2];                                                                               \
		o[2] = a[0] * b[1] - a[1] * b[0];                                                                               \
	} while (0)
#define NORM(v)                                                                                                         \
	do {                                                                                                                \
		f32 n_ = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);                                                        \
		if (n_ > 1e-6f) {                                                                                               \
			v[0] /= n_;                                                                                                 \
			v[1] /= n_;                                                                                                 \
			v[2] /= n_;                                                                                                 \
		}                                                                                                               \
	} while (0)
	CROSS(f, up, r);
	NORM(r);
	for (int k = 0; k < 3; k++)
		f[k] += eye->zoffset * r[k]; /* each eye turned in a little */
	NORM(f);
	CROSS(f, up, r);
	NORM(r);
	f32 u2[3];
	CROSS(r, f, u2);
	NORM(u2);
	f32 s = eye->irisScale;
	Mtx m;
	for (int k = 0; k < 3; k++) {
		m[0][k] = -s * r[k];
		m[1][k] = -s * u2[k];
		m[2][k] = 0;
	}
	m[0][3] = s * (r[0] * eye->org[0] + r[1] * eye->org[1] + r[2] * eye->org[2]) + 0.5f;
	m[1][3] = s * (u2[0] * eye->org[0] + u2[1] * eye->org[1] + u2[2] * eye->org[2]) + 0.5f;
	m[2][3] = 1;
	GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX2x4);
}

/* ------------------------------------------------------------------ model lighting
 * Source lights models with the ambient cube of where they stand (six colours, one per axis direction,
 * weighted by the squared normal component). GX does exactly that with six lights in specular
 * attenuation mode: attenuation = max(0, N.d)^2 for each axis direction d, no diffuse term. The cube
 * comes from the map's leaf (moving things, every frame, eased) or was baked per static prop. */
static const Pack *mapPack;
static f32 viewRot[3][3];
static u8 lastCube[18];
static int cubeLoaded;
static u8 cubeAvg[4] = {128, 128, 128, 255}; /* (the eyes, which aren't lit per vertex) */

static int cube_ok(const u8 *c)
{
	for (int i = 0; i < 18; i++)
		if (c[i])
			return 1;
	return 0;
}

static void tint_cube(const u8 tint[4], u8 out[18])
{
	for (int f = 0; f < 6; f++)
		for (int k = 0; k < 3; k++)
			out[f * 3 + k] = tint[k];
}

int Render_CubeAt(const Pack *p, const f32 pos[3], u8 out[18])
{
	if (!p || !p->leafCubes || !p->numNodes)
		return 0;
	int leaf = World_PointLeaf(p, pos);
	if (leaf < 0 || (u32)leaf >= p->numLeafCubes || !cube_ok(p->leafCubes[leaf]))
		return 0;
	memcpy(out, p->leafCubes[leaf], 18);
	return 1;
}

/* the flashlight on models: a spotlight in a second colour channel, added on top of the cube in a
 * second TEV stage (GX's attenuation function is per channel, so it can't share the cube's) */
static int mflOn;
static f32 mflPos[3], mflDir[3];

void Render_ModelFlashlight(int on, const Camera *cam)
{
	mflOn = on && cam;
	if (!mflOn)
		return;
	mflPos[0] = cam->origin.x + cam->right.x * 6 - cam->up.x * 4;
	mflPos[1] = cam->origin.y + cam->right.y * 6 - cam->up.y * 4;
	mflPos[2] = cam->origin.z + cam->right.z * 6 - cam->up.z * 4;
	mflDir[0] = cam->forward.x;
	mflDir[1] = cam->forward.y;
	mflDir[2] = cam->forward.z;
}

static void lights_view(Mtx view)
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			viewRot[r][c] = view[r][c];
	cubeLoaded = 0;
	if (mflOn) {
		f32 vp[3], vd[3];
		for (int r = 0; r < 3; r++) {
			vp[r] = view[r][0] * mflPos[0] + view[r][1] * mflPos[1] + view[r][2] * mflPos[2] + view[r][3];
			vd[r] = view[r][0] * mflDir[0] + view[r][1] * mflDir[1] + view[r][2] * mflDir[2];
		}
		GXLightObj lo;
		GX_InitLightPos(&lo, vp[0], vp[1], vp[2]);
		GX_InitLightDir(&lo, vd[0], vd[1], vd[2]);
		GX_InitLightSpot(&lo, 24.0f, GX_SP_COS2);
		GX_InitLightDistAttn(&lo, 500.0f, 0.4f, GX_DA_MEDIUM);
		GX_InitLightColor(&lo, (GXColor){200, 192, 175, 255});
		GX_LoadLightObj(&lo, GX_LIGHT7);
	}
}

static void light_cube(const u8 cube[18])
{
	for (int k = 0; k < 3; k++)
		cubeAvg[k] = (u8)((cube[k] + cube[3 + k] + cube[6 + k] + cube[9 + k] + cube[12 + k] + cube[15 + k]) / 6);
	if (cubeLoaded && !memcmp(cube, lastCube, 18))
		return;
	memcpy(lastCube, cube, 18);
	cubeLoaded = 1;
	static const f32 dir[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (int i = 0; i < 6; i++) {
		f32 d[3];
		for (int r = 0; r < 3; r++)
			d[r] = viewRot[r][0] * dir[i][0] + viewRot[r][1] * dir[i][1] + viewRot[r][2] * dir[i][2];
		GXLightObj lo;
		GX_InitLightPos(&lo, d[0] * 1.0e6f, d[1] * 1.0e6f, d[2] * 1.0e6f); /* (only gates N.L >= 0) */
		GX_InitLightDir(&lo, -d[0], -d[1], -d[2]);                         /* (stored negated: = d) */
		GX_InitLightAttn(&lo, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);          /* (N.d)^2 / 1 */
		GX_InitLightColor(&lo, (GXColor){cube[i * 3], cube[i * 3 + 1], cube[i * 3 + 2], 255});
		GX_LoadLightObj(&lo, (u8)(GX_LIGHT0 << i));
	}
}

/* far away (litFar): the ambient cube's average instead of its six lights. Six attenuated lights a vertex
 * are most of the GPU's vertex time; a distant model is a few pixels where the difference doesn't show */
static int litFar;
#define FAR_LIGHT_DIST 1200.0f
static f32 detail = 1.0f; /* adaptive level of detail, 0.3..1 (Render_AdaptDetail) */

static void lit_channel(void)
{
	GX_SetNumChans(1);
	if (litFar) {
		GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){cubeAvg[0], cubeAvg[1], cubeAvg[2], 255});
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 255, 255, 255});
		GX_SetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		return;
	}
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){0, 0, 0, 255});
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 255, 255, 255});
	GX_SetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
	               GX_LIGHT0 | GX_LIGHT1 | GX_LIGHT2 | GX_LIGHT3 | GX_LIGHT4 | GX_LIGHT5, GX_DF_NONE, GX_AF_SPEC);
	GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	if (mflOn) {
		GX_SetNumChans(2);
		GX_SetChanAmbColor(GX_COLOR1A1, (GXColor){0, 0, 0, 255});
		GX_SetChanMatColor(GX_COLOR1A1, (GXColor){255, 255, 255, 255});
		GX_SetChanCtrl(GX_COLOR1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT7, GX_DF_CLAMP, GX_AF_SPOT);
		GX_SetChanCtrl(GX_ALPHA1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	}
}

static void unlit_channel(void)
{
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
}

/* moving things: the cube at a point, eased towards (half a second) so walking between leaves doesn't pop */
#define MAX_LIT_ENTS 4096
static u8 (*entCube)[18];
static const Pack *entCubePack;

static u8 tmpCube[18];

static const u8 *entity_cube(const Pack *p, s32 ent, const f32 at[3], const u8 tint[4])
{
	if (!entCube)
		entCube = malloc(MAX_LIT_ENTS * 18);
	if (entCubePack != p && entCube) {
		memset(entCube, 0, MAX_LIT_ENTS * 18);
		entCubePack = p;
	}
	static u8 tmp[18];
	u8 want[18];
	if (!Light_EntityCube(p, ent >= 0 ? ent : -1, at, want))
		tint_cube(tint, want);
	if (!entCube || ent < 0 || ent >= MAX_LIT_ENTS) {
		memcpy(tmp, want, 18);
		return tmp;
	}
	u8 *c = entCube[ent];
	if (!cube_ok(c)) {
		memcpy(c, want, 18);
	} else {
		for (int i = 0; i < 18; i++) {
			int d = (int)want[i] - (int)c[i];
			int step = d / 8;
			if (step == 0 && d)
				step = d > 0 ? 1 : -1;
			c[i] = (u8)(c[i] + step);
		}
	}
	return c;
}

static void model_material(const Pack *p, const PackMat *m, const u8 tint[4])
{
	no_indirect();
	if (m->kind == MAT_EYE && m->tex >= 0) {
		eye_material(m, cubeAvg);
		return;
	}
	const FxState *fx = Fx_Of(p, (u32)(m - p->mats));
	if (fx) {
		fx_material(m, fx, 1);
		return;
	}
	(void)tint;
	lit_channel();
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	if (m->tex >= 0) {
		GX_LoadTexObj(&texObjs[m->tex], GX_TEXMAP0);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	} else {
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	}
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	if (mflOn && m->tex >= 0 && !is_translucent(m->kind)) {
		/* + texture x flashlight */
		GX_SetNumTevStages(2);
		GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR1A1);
		GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_CPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
		GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
	set_blend(m->kind);
}

static void draw_model(const Pack *p, u32 mdl, const f32 origin[3], const f32 R[3][3], const u8 cube[18], Mtx view,
                       int pass, RenderStats *st)
{
	const PackModel *m = &p->models[mdl];
	u8 *v = (u8 *)&p->mverts[m->firstVert];
	GX_SetArray(GX_VA_POS, v, sizeof(PackMVert));
	GX_SetArray(GX_VA_TEX0, v + 6, sizeof(PackMVert));
	GX_SetArray(GX_VA_NRM, v + 10, sizeof(PackMVert));
	Mtx mv;
	model_matrix(view, origin, R, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_LoadNrmMtxImm(mv, GX_PNMTX0);
	light_cube(cube);
	/* (the eye's distance is mv's translation) */
	f32 r2 = 0, d2 = mv[0][3] * mv[0][3] + mv[1][3] * mv[1][3] + mv[2][3] * mv[2][3];
	for (int k = 0; k < 3; k++)
		r2 += (m->maxs[k] - m->mins[k]) * (m->maxs[k] - m->mins[k]) * 0.25f;
	litFar = !mflOn && d2 > FAR_LIGHT_DIST * FAR_LIGHT_DIST * detail * detail && d2 > r2 * 30.0f * 30.0f * detail * detail;
	for (u32 k = 0; k < m->numMeshes; k++) {
		const PackMesh *me = &p->meshes[m->firstMesh + k];
		const PackMat *mat = &p->mats[me->mat];
		if (is_translucent(mat->kind) != pass || mat->kind == MAT_NODRAW)
			continue;
		model_material(p, mat, cubeAvg);
		u16 frac = me->frac & 0x7FFF; /* (the top bit: the indices are one triangle strip) */
		if (frac != posFrac) {
			posFrac = frac;
			GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_S16, (u8)posFrac);
		}
		const u16 *idx = &p->mindices[me->firstIndex];
		GX_Begin((me->frac & 0x8000) ? GX_TRIANGLESTRIP : GX_TRIANGLES, GX_VTXFMT2, (u16)me->numIndices);
		for (u32 j = 0; j < me->numIndices; j++) {
			GX_Position1x16(idx[j]);
			GX_Normal1x16(idx[j]);
			GX_TexCoord1x16(idx[j]);
		}
		GX_End();
		st->verts += me->numIndices;
		st->modelVerts += me->numIndices;
#ifdef RENDER_MODEL_STATS
		{
			extern u32 rtModelVerts[4096];
			if (mdl < 4096)
				rtModelVerts[mdl] += me->numIndices;
		}
#endif
	}
	st->faces++;
	litFar = 0;
}

/* a sphere against the camera's view: in front, inside the side planes, and big enough on screen to matter
 * (Source fades small props out with distance; a prop under ~2 pixels isn't worth its vertices) */
/* Adaptive detail, so busy scenes hold the frame rate: when the last frames sent more model vertices
 * than the GPU can transform and light at 30 fps, far LODs take over closer, small far things drop out
 * sooner and far models get the cheap lighting earlier; it recovers slowly once the scene is lighter.
 * (Source does the same job with its LOD and fade settings per machine; ours follows the scene.) */
#define DETAIL_VERT_BUDGET 52000u

void Render_AdaptDetail(const RenderStats *st)
{
	u32 v = st->modelVerts + st->npcVerts + st->worldVerts / 2; /* (world faces are cheaper: no lighting) */
	if (v > DETAIL_VERT_BUDGET + DETAIL_VERT_BUDGET / 6)
		detail *= 0.9f;
	else if (v < DETAIL_VERT_BUDGET * 3 / 4)
		detail *= 1.02f;
	detail = detail < 0.3f ? 0.3f : detail > 1.0f ? 1.0f : detail;
}

f32 Render_Detail(void) { return detail; }

static int sphere_visible(const Camera *cam, const f32 c[3], f32 r, f32 fadeMax)
{
	f32 d[3] = {c[0] - cam->origin.x, c[1] - cam->origin.y, c[2] - cam->origin.z};
	f32 z = d[0] * cam->forward.x + d[1] * cam->forward.y + d[2] * cam->forward.z;
	if (z < -r)
		return 0; /* behind the camera */
	f32 x = d[0] * cam->right.x + d[1] * cam->right.y + d[2] * cam->right.z;
	f32 y = d[0] * cam->up.x + d[1] * cam->up.y + d[2] * cam->up.z;
	f32 tx = tanf(cam->fovx * (f32)M_PI / 360.0f), ty = tanf(cam->fovy * (f32)M_PI / 360.0f);
	/* |x| <= z tan + r / cos  (the sphere touches the side plane) */
	if (fabsf(x) > z * tx + r * sqrtf(1 + tx * tx))
		return 0;
	if (fabsf(y) > z * ty + r * sqrtf(1 + ty * ty))
		return 0;
	f32 dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	if (fadeMax > 0 && dist2 > fadeMax * fadeMax)
		return 0; /* the map's own fade distance */
	if (z > r && r * detail < 0.0047f * z * tx)
		return 0; /* under ~1.5 pixels across a 640-wide screen (more when the scene is heavy) */
	f32 far = 6000.0f * detail;
	if (dist2 > (far + r) * (far + r) && r < 64.0f)
		return 0; /* small and far: Source fades these out too */
	return 1;
}

/* a model's far LOD takes over once its bounding sphere is this many radii away (about 35 pixels tall) */
#define LOD_SWITCH 12.0f

/* visible? and which LOD to draw (*use: the model or its far version) */
static int model_visible(const Pack *p, u32 mdl, const f32 origin[3], const f32 R[3][3], const Camera *cam, f32 fadeMax,
                         u32 *use)
{
	const PackModel *m = &p->models[mdl];
	f32 mid[3], r = 0;
	for (int k = 0; k < 3; k++) {
		f32 h = (m->maxs[k] - m->mins[k]) * 0.5f;
		r += h * h;
		mid[k] = (m->maxs[k] + m->mins[k]) * 0.5f;
	}
	f32 c[3];
	for (int k = 0; k < 3; k++)
		c[k] = origin[k] + (R ? R[k][0] * mid[0] + R[k][1] * mid[1] + R[k][2] * mid[2] : mid[k]);
	r = sqrtf(r);
	if (!sphere_visible(cam, c, r + 4.0f, fadeMax))
		return 0;
	*use = mdl;
	if (p->modelFar && mdl < p->numModelFar && p->modelFar[mdl] < p->numModels) {
		f32 d[3] = {c[0] - cam->origin.x, c[1] - cam->origin.y, c[2] - cam->origin.z};
		f32 sw = LOD_SWITCH * detail * r * (cam->fovy > 0 ? 59.0f / cam->fovy : 1.0f); /* (zoomed in: later) */
		if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > sw * sw)
			*use = p->modelFar[mdl];
	}
	return 1;
}

/* static props' rotations, worked out once per map */
static f32 (*spRot)[3][3];
static const Pack *spRotPack;

static const f32 (*static_rot(const Pack *p, u32 i))[3]
{
	if (spRotPack != p) {
		free(spRot);
		spRot = p->numStaticProps ? malloc(p->numStaticProps * sizeof(*spRot)) : NULL;
		spRotPack = p;
		for (u32 k = 0; spRot && k < p->numStaticProps; k++)
			Ent_AngleMatrix(p->staticProps[k].angles, spRot[k]);
	}
	return spRot ? (const f32(*)[3])spRot[i] : NULL;
}

static void draw_props(const Pack *p, const World *w, const Camera *cam, Mtx view, int pass, RenderStats *st)
{
	if (!p->numModels)
		return;
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);

	for (u32 i = 0; i < p->numStaticProps; i++) {
		const PackStaticProp *sp = &p->staticProps[i];
		if (sp->model >= p->numModels)
			continue;
		if (p->numClusters) {
			if (p->spClusterRange) {
				/* visible when any cluster its leaves touch is (Source's static prop leaf list) */
				u32 f = p->spClusterRange[i][0], n = p->spClusterRange[i][1], k;
				for (k = 0; k < n && f + k < p->numSpClusters; k++)
					if (p->spClusters[f + k] < p->numClusters && w->pvs[p->spClusters[f + k]])
						break;
				if (n && k >= n)
					continue;
			} else if (sp->cluster >= 0 && !w->pvs[sp->cluster])
				continue;
		}
		f32 Rt[3][3];
		const f32(*R)[3] = static_rot(p, i);
		if (!R) {
			Ent_AngleMatrix(sp->angles, Rt);
			R = (const f32(*)[3])Rt;
		}
		u32 use;
		if (!model_visible(p, sp->model, sp->origin, R, cam, sp->pad > 0 ? sp->pad * 4.0f : 0, &use))
			continue;
		u8 cb[18];
		const u8 *sc = Light_StaticCube(p, i);
		if (sc)
			memcpy(cb, sc, 18);
		else
			tint_cube(sp->tint, cb);
		draw_model(p, use, sp->origin, R, cb, view, pass, st);
	}

	u32 n;
	Entity *ents = Ent_List(&n);
	for (u32 i = 1; i < n; i++) {
		const Entity *e = &ents[i];
		if (e->mdl < 0 || (u32)e->mdl >= p->numModels || (e->flags & (EF_DISABLED | EF_KILLED)))
			continue;
		if (e->npcModel >= 0)
			continue; /* a prop with bones: drawn posed, with the characters */
		const PackModel *pm = &p->models[e->mdl];
		f32 mid[3] = {(pm->mins[0] + pm->maxs[0]) * 0.5f, (pm->mins[1] + pm->maxs[1]) * 0.5f, (pm->mins[2] + pm->maxs[2]) * 0.5f};
		f32 at[3];
		for (int k = 0; k < 3; k++)
			at[k] = e->origin[k] + e->R[k][0] * mid[0] + e->R[k][1] * mid[1] + e->R[k][2] * mid[2];
		if (p->numClusters) {
			/* hidden only when neither its origin's cluster nor its middle's is visible (an origin can sit
			 * under the floor or through a wall, in another room's cluster) */
			int c = World_PointCluster(p, e->origin);
			if (c >= 0 && !w->pvs[c]) {
				int c2 = World_PointCluster(p, at);
				if (c2 < 0 || !w->pvs[c2])
					continue;
			}
		}
		u32 use;
		if (!model_visible(p, e->mdl, e->origin, e->R, cam, e->fadeMax, &use))
			continue;
		draw_model(p, use, e->origin, e->R, entity_cube(p, (s32)i, at, e->tint), view, pass, st);
	}
}

/* ------------------------------------------------------------------ NPCs */

static f32 npcPose[NPC_MAX_BONES][3][4] ATTRIBUTE_ALIGN(32);

static void draw_npcs(const Pack *p, const World *w, const Camera *cam, Mtx view, int pass, RenderStats *st)
{
	u32 nn;
	Npc *list = Npc_List(&nn);
	if (!nn || !p->numNpcModels)
		return;
	u32 ne;
	Entity *ents = Ent_List(&ne);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_PTNMTXIDX, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
	for (u32 i = 0; i < nn; i++) {
		const Npc *n = &list[i];
		const Entity *e = &ents[n->ent];
		if (e->flags & (EF_DISABLED | EF_KILLED))
			continue;
		const PackNpcModel *m = &p->npcModels[n->model];
		if (p->numClusters) {
			f32 c[3] = {e->origin[0], e->origin[1], e->origin[2] + m->height * 0.5f};
			int cl = World_PointCluster(p, c);
			if (cl >= 0 && !w->pvs[cl])
				continue;
		}
		f32 d[3] = {e->origin[0] - cam->origin.x, e->origin[1] - cam->origin.y,
		            e->origin[2] + m->height * 0.5f - cam->origin.z};
		f32 r = m->height * 0.6f + m->radius + 16.0f;
		f32 cc[3] = {e->origin[0], e->origin[1], e->origin[2] + m->height * 0.5f};
		if (!sphere_visible(cam, cc, r, e->fadeMax))
			continue;
		Npc_Pose(n, npcPose);
		f32 dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		u32 faceBase = 0;
		const PackNpcVert *face = Face_Morph(n, dist, &faceBase);
		Mtx mv;
		model_matrix(view, e->origin, e->R, mv);
		light_cube(entity_cube(p, n->ent, cc, e->tint));
		litFar = !mflOn && dist > FAR_LIGHT_DIST * detail && dist > m->height * 25.0f * detail;
		for (u32 b = 0; b < m->numBatches; b++) {
			const PackNpcBatch *bt = &p->npcBatches[m->firstBatch + b];
			const PackMat *mat = &p->mats[bt->mat];
			if (is_translucent(mat->kind) != pass || mat->kind == MAT_NODRAW)
				continue;
			for (u32 k = 0; k < bt->numBones && k < 10; k++) {
				Mtx bm;
				u32 bone = bt->bones[k];
				if (bone >= n->skel->numBones)
					bone = 0;
				mtx_concat(mv, npcPose[bone], bm);
				GX_LoadPosMtxImm(bm, GX_PNMTX0 + k * 3);
				GX_LoadNrmMtxImm(bm, GX_PNMTX0 + k * 3);
			}
			const PackNpcVert *v = (face && bt->flexed) ? &face[bt->firstVert - faceBase] : &p->npcVerts[bt->firstVert];
			GX_SetArray(GX_VA_POS, (void *)v, sizeof(PackNpcVert));
			GX_SetArray(GX_VA_TEX0, (u8 *)v + 12, sizeof(PackNpcVert));
			GX_SetArray(GX_VA_NRM, (u8 *)v + 17, sizeof(PackNpcVert));
			if (mat->kind == MAT_EYE && bt->eye != 255 && p->eyeIndex && p->eyes) {
				const PackEyeIndex *ei = &p->eyeIndex[n->model];
				if (bt->eye < ei->count) {
					const PackEye *eye = &p->eyes[ei->first + bt->eye];
					u32 bone = (eye->bone >= 0 && (u32)eye->bone < n->skel->numBones) ? (u32)eye->bone : 0;
					/* eyes follow the character's look-at target (Source's m_viewtarget); otherwise the camera */
					f32 tgt[3] = {cam->origin.x, cam->origin.y, cam->origin.z};
					if (n->lookEnt > 0 && n->lookW > 0.3f) {
						Npc_EyePos(n->lookEnt, tgt);
					}
					eye_matrix(eye, npcPose[bone], e->R, e->origin, (const guVector *)tgt);
				}
			}
			model_material(p, mat, cubeAvg);
			const u16 *idx = &p->npcIndices[bt->firstIndex];
			GX_Begin(bt->strip ? GX_TRIANGLESTRIP : GX_TRIANGLES, GX_VTXFMT5, (u16)bt->numIndices);
			for (u32 j = 0; j < bt->numIndices; j++) {
				u16 k = idx[j];
				GX_MatrixIndex1x8(v[k].mtx);
				GX_Position1x16(k);
				GX_Normal1x16(k);
				GX_TexCoord1x16(k);
			}
			GX_End();
			st->verts += bt->numIndices;
			st->npcVerts += bt->numIndices;
		}
		st->faces++;
	}
	litFar = 0;
	GX_SetCurrentMtx(GX_PNMTX0);
}

/* ------------------------------------------------------------------ camera screens */

void Render_CameraBegin(void)
{
	GX_SetViewport(0, 0, CAM_W, CAM_H, 0, 1);
	GX_SetScissor(0, 0, CAM_W, CAM_H);
}

void Render_CameraEnd(u16 fbWidth, u16 efbHeight)
{
	GX_SetTexCopySrc(0, 0, CAM_W, CAM_H);
	GX_SetTexCopyDst(CAM_W, CAM_H, GX_TF_RGB565, GX_FALSE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	DCInvalidateRange(camData, sizeof(camData));
	GX_CopyTex(camData, GX_TRUE); /* copy and clear the corner of the EFB we drew into */
	GX_PixModeSync();
	GX_InitTexObj(&camTex, camData, CAM_W, CAM_H, GX_TF_RGB565, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GX_InvalidateTexAll();
	camValid = 1;
	GX_SetViewport(0, 0, fbWidth, efbHeight, 0, 1);
	GX_SetScissor(0, 0, fbWidth, efbHeight);
}

void Render_CameraOff(void) { camValid = 0; }

/* ------------------------------------------------------------------ frame */

/* under water: everything fades into the water's fog colour (Source fogs the view with the water
 * material's $fogcolor/$fogstart/$fogend); the nearest water surface decides which water it is */
static int fogOn;
static f32 fogStart, fogEnd;
static GXColor fogCol;

static void underwater_fog(const Pack *p, const Camera *cam)
{
	f32 o[3] = {cam->origin.x, cam->origin.y, cam->origin.z};
	fogOn = 0;
	if (!p->numWater || !(CM_PointContents(o) & MASK_WATER)) {
		GX_SetFog(GX_FOG_NONE, 0, 1, 1, 2, (GXColor){0, 0, 0, 255});
		return;
	}
	const PackWater *best = &p->water[0];
	f32 bestD = 1e30f;
	for (u32 i = 0; i < p->numFaces; i++) {
		const PackFace *f = &p->faces[i];
		if (p->mats[f->mat].kind != MAT_WATER || !f->numverts)
			continue;
		const f32 *v = &p->verts[f->firstvert].pos[0];
		f32 d = (v[0] - o[0]) * (v[0] - o[0]) + (v[1] - o[1]) * (v[1] - o[1]) + (v[2] - o[2]) * (v[2] - o[2]);
		if (d < bestD) {
			const PackWater *wt = water_of(p, &p->mats[f->mat]);
			if (wt) {
				bestD = d;
				best = wt;
			}
		}
	}
	f32 fs = best->fogStart, fe = best->fogEnd > fs + 16 ? best->fogEnd : fs + 400;
	GX_SetFog(GX_FOG_LIN, fs, fe, 4.0f, 16384.0f, (GXColor){best->fog[0], best->fog[1], best->fog[2], 255});
	fogOn = 1;
	fogStart = fs;
	fogEnd = fe;
	fogCol = (GXColor){best->fog[0], best->fog[1], best->fog[2], 255};
}

/* switchable and flickering lights: each lit style's layer added onto the faces it lights
 * (tools/lightstyles.py), texture x layer x the style's brightness, after the opaque faces */
static int stPage, stStyle, stMat;

static void styles_begin(void)
{
	no_indirect();
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(2);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_TEXMTX5);
	GX_SetNumTevStages(2);
	/* texture x brightness/2 ... */
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	/* ... x layer x 4 (the lightmaps' x2, and the halved brightness) */
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_4, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	if (fogOn) /* (fog fades what's added towards nothing, not towards the fog colour) */
		GX_SetFog(GX_FOG_LIN, fogStart, fogEnd, 4.0f, 16384.0f, (GXColor){0, 0, 0, 255});
	stPage = stStyle = stMat = -1;
}

static void styles_face(const Pack *p, const PackStyleFace *sf, RenderStats *st)
{
	if (sf->face >= p->numFaces || sf->page >= p->numStylePages)
		return;
	f32 v = Light_StyleValue(sf->style);
	if (v <= 0.02f)
		return;
	const PackFace *f = &p->faces[sf->face];
	const PackMat *m = &p->mats[f->mat];
	if (m->tex < 0 || is_translucent(m->kind) || m->kind == MAT_WATER || m->kind == MAT_SKY ||
	    m->kind == MAT_NODRAW || m->kind == MAT_CAMERA)
		return;
	if (sf->page != stPage) {
		const PackStylePage *sp = &p->stylePages[sf->page];
		if (sp->tex >= p->numTex)
			return;
		stPage = sf->page;
		GX_LoadTexObj(&texObjs[sp->tex], GX_TEXMAP1);
		Mtx tm = {{1, 0, 0, 0}, {0, sp->vscale, 0, 0}, {0, 0, 1, 0}};
		GX_LoadTexMtxImm(tm, GX_TEXMTX5, GX_MTX2x4);
	}
	if (sf->style != stStyle) {
		stStyle = sf->style;
		f32 k = v * 127.5f;
		u8 c = (u8)(k > 255 ? 255 : k);
		GX_SetTevKColor(GX_KCOLOR0, (GXColor){c, c, c, 255});
	}
	if (f->mat != stMat) {
		stMat = f->mat;
		GX_LoadTexObj(&texObjs[m->tex], GX_TEXMAP0);
		if (m->kind == MAT_ALPHATEST)
			GX_SetAlphaCompare(GX_GREATER, 127, GX_AOP_AND, GX_ALWAYS, 0);
		else
			GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	}
	u32 base = f->firstvert & ~0x7FFFu;
	set_segment(p, base);
	u16 i0 = (u16)(f->firstvert - base);
	GX_Begin(f->prim == PRIM_FAN ? GX_TRIANGLEFAN : GX_TRIANGLES, GX_VTXFMT0, f->numverts);
	for (u16 k = 0; k < f->numverts; k++) {
		u16 x = i0 + k;
		GX_Position1x16(x);
		GX_Color1x16(x);
		GX_TexCoord1x16(x);
		GX_TexCoord1x16(x);
	}
	GX_End();
	st->verts += f->numverts;
	st->worldVerts += f->numverts;
}

static void styles_end(void)
{
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	if (fogOn)
		GX_SetFog(GX_FOG_LIN, fogStart, fogEnd, 4.0f, 16384.0f, fogCol);
	curLm = -1;
}

static void draw_light_styles(const Pack *p, const World *w, RenderStats *st)
{
	if (!p->numStyleFaces || !p->styleFaces || !p->stylePages || !w->faceFrame)
		return;
	int any = 0;
	for (u32 i = 0; i < p->numStyleFaces && !any; i++) {
		const PackStyleFace *sf = &p->styleFaces[i];
		any = sf->face < p->numFaces && w->faceFrame[sf->face] == w->frame && Light_StyleValue(sf->style) > 0.02f;
	}
	if (!any)
		return;
	styles_begin();
	for (u32 i = 0; i < p->numStyleFaces; i++)
		if (p->styleFaces[i].face < p->numFaces && w->faceFrame[p->styleFaces[i].face] == w->frame)
			styles_face(p, &p->styleFaces[i], st);
	styles_end();
}

/* ---- the 3D skybox (sky_camera): the city beyond the walls, built at 1/16 scale somewhere else in the map and
 * seen from a camera that moves 1/16 as far as the player; drawn behind everything after the sky texture */
static const World *skyW;
static Camera skyC;
static Mtx skyV;
static u8 skyFog;
static f32 skyFogS, skyFogE;
static GXColor skyFogC;

void Render_SetSky(const World *w, const Camera *c, Mtx v, int fog, f32 fogStart, f32 fogEnd, const u8 col[3])
{
	skyW = w;
	if (!w)
		return;
	skyC = *c;
	memcpy(skyV, v, sizeof(Mtx));
	skyFog = (u8)(fog != 0);
	skyFogS = fogStart;
	skyFogE = fogEnd > fogStart + 1 ? fogEnd : fogStart + 1;
	skyFogC = (GXColor){col[0], col[1], col[2], 255};
}

static void draw_props(const Pack *p, const World *w, const Camera *cam, Mtx view, int pass, RenderStats *st);

/* the depth buffer back to "far" (after the skybox: the world is always in front of it) */
static void clear_depth(void)
{
	Mtx id;
	guMtxIdentity(id);
	GX_LoadPosMtxImm(id, GX_PNMTX0);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetNumChans(0);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetFog(GX_FOG_NONE, 0, 1, 1, 2, (GXColor){0, 0, 0, 255});
	GX_SetColorUpdate(GX_FALSE);
	GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
	/* a screen-filling quad just inside the far plane (eye space looks down -z) */
	const f32 z = -16000.0f, e = 22000.0f; /* (wider than any field of view at that depth) */
	GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
	GX_Position3f32(-e, e, z);
	GX_Position3f32(e, e, z);
	GX_Position3f32(e, -e, z);
	GX_Position3f32(-e, -e, z);
	GX_End();
	GX_SetColorUpdate(GX_TRUE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

static void draw_skybox3d(const Pack *p, RenderStats *st)
{
	world_vtxdesc();
	GX_LoadPosMtxImm(skyV, GX_PNMTX0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	/* sky_camera's fog: the distant city fades into the sky */
	if (skyFog)
		GX_SetFog(GX_FOG_LIN, skyFogS, skyFogE, 4.0f, 16384.0f, skyFogC);
	else
		GX_SetFog(GX_FOG_NONE, 0, 1, 1, 2, (GXColor){0, 0, 0, 255});
	for (int pass = 0; pass < 2; pass++) {
		for (u32 m = 0; m < p->numMats; m++) {
			u32 a = skyW->matStart[m], b = skyW->matStart[m + 1];
			if (a == b || is_translucent(p->mats[m].kind) != pass || p->mats[m].kind == MAT_WATER)
				continue;
			set_material(p, &p->mats[m]);
			for (u32 i = a; i < b; i++)
				draw_face(p, &p->faces[skyW->sorted[i]], st);
		}
		draw_props(p, skyW, &skyC, skyV, pass, st);
		world_vtxdesc();
		GX_LoadPosMtxImm(skyV, GX_PNMTX0);
		unlit_channel();
	}
	no_indirect();
	clear_depth();
}

void Render_Frame(const Pack *p, const World *w, const Camera *cam, Mtx view, RenderStats *st)
{
	memset(st, 0, sizeof(*st));
	mapPack = p;
	Light_Frame();
	lights_view(view);
	draw_sky(p, view);
	if (skyW)
		draw_skybox3d(p, st);
	underwater_fog(p, cam);

	world_vtxdesc();
	GX_LoadPosMtxImm(view, GX_PNMTX0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	/* the screen-space refraction for water: projection * view, scaled into 0..1 texture space */
	frameCam = cam;
	waterClock += 1.0f / 60.0f;
	{
		Mtx44 pr;
		guPerspective(pr, cam->fovy, 4.0f / 3.0f, 4.0f, 16384.0f);
		f32 pv[4][4];
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++) {
				f32 s = pr[r][3] * (c == 3 ? 1.0f : 0.0f);
				for (int k = 0; k < 3; k++)
					s += pr[r][k] * view[k][c];
				pv[r][c] = s;
			}
		for (int c = 0; c < 4; c++) {
			refrMtx[0][c] = 0.5f * pv[0][c] + 0.5f * pv[3][c];
			refrMtx[1][c] = -0.5f * pv[1][c] + 0.5f * pv[3][c];
			refrMtx[2][c] = pv[3][c];
		}
	}
	refrValid = 0;

	/* pass 0: opaque + alpha-tested + blended world, pass 1: translucent */
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1 && refrData) {
			/* any water in view: grab the picture so far for its refraction */
			int water = 0;
			for (u32 m = 0; m < p->numMats && !water; m++)
				water = p->mats[m].kind == MAT_WATER && w->matStart[m] != w->matStart[m + 1];
			if (water) {
				GX_SetTexCopySrc(0, 0, scrW, scrH);
				GX_SetTexCopyDst(scrW / 2, scrH / 2, GX_TF_RGB565, GX_TRUE);
				GX_CopyTex(refrData, GX_FALSE);
				GX_PixModeSync();
				GX_InvalidateTexAll();
				refrValid = 1;
			}
		}
		for (u32 m = 0; m < p->numMats; m++) {
			u32 a = w->matStart[m], b = w->matStart[m + 1];
			if (a == b || is_translucent(p->mats[m].kind) != pass)
				continue;
			set_material(p, &p->mats[m]);
			st->materials++;
			for (u32 i = a; i < b; i++)
				draw_face(p, &p->faces[w->sorted[i]], st);
		}
		if (pass == 0)
			draw_light_styles(p, w, st);

		draw_brush_entities(p, view, pass, st);
		world_vtxdesc();
		GX_LoadPosMtxImm(view, GX_PNMTX0);
		draw_props(p, w, cam, view, pass, st);
		draw_npcs(p, w, cam, view, pass, st);
		world_vtxdesc();
		GX_LoadPosMtxImm(view, GX_PNMTX0);
		unlit_channel();
	}
	no_indirect();
	GX_SetFog(GX_FOG_NONE, 0, 1, 1, 2, (GXColor){0, 0, 0, 255});
}

/* ------------------------------------------------------------------ weapons / items (their own pack) */

int Render_InitSkinPack(const Pack *wp)
{
	free(skinTex);
	skinTex = NULL;
	numSkinTex = 0;
	if (!wp || !wp->numTex)
		return 0;
	skinTex = memalign(32, wp->numTex * sizeof(GXTexObj));
	if (!skinTex)
		return -1;
	for (u32 i = 0; i < wp->numTex; i++) {
		const PackTexInfo *t = &wp->tex[i];
		u8 wrap = t->clamp ? GX_CLAMP : GX_REPEAT;
		u8 mip = t->levels > 1 ? GX_TRUE : GX_FALSE;
		GX_InitTexObj(&skinTex[i], wp->texData + t->offset, t->w, t->h, t->fmt, wrap, wrap, mip);
		if (mip)
			GX_InitTexObjLOD(&skinTex[i], GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, (f32)(t->levels - 1), 0.0f, GX_FALSE,
			                 GX_FALSE, GX_ANISO_1);
	}
	numSkinTex = wp->numTex;
	return 0;
}

/* a texture of the weapons pack (effects: muzzle flashes, bullet holes...) on TEXMAP0; 0 if there's none */
int Render_SkinTexture(s32 idx)
{
	if (!skinTex || idx < 0 || (u32)idx >= numSkinTex)
		return 0;
	GX_LoadTexObj(&skinTex[idx], GX_TEXMAP0);
	return 1;
}

void Render_SkinnedModel(const Pack *wp, u32 model, f32 (*pose)[3][4], u32 numBones, const f32 origin[3],
                         const f32 R[3][3], const u8 tint[4], Mtx view)
{
	if (!wp || model >= wp->numNpcModels || !skinTex)
		return;
	GXTexObj *keep = texObjs;
	texObjs = skinTex;
	const PackNpcModel *m = &wp->npcModels[model];
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_PTNMTXIDX, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
	Mtx mv;
	model_matrix(view, origin, R, mv);
	lights_view(view);
	{
		/* (slot 0: the viewmodel; entity 0 is the world, never drawn as a model) */
		light_cube(mapPack ? entity_cube(mapPack, 0, origin, tint) : (tint_cube(tint, tmpCube), tmpCube));
	}
	for (int pass = 0; pass < 2; pass++)
		for (u32 b = 0; b < m->numBatches; b++) {
			const PackNpcBatch *bt = &wp->npcBatches[m->firstBatch + b];
			const PackMat *mat = &wp->mats[bt->mat];
			if (is_translucent(mat->kind) != pass || mat->kind == MAT_NODRAW)
				continue;
			if (mat->tex >= (s32)numSkinTex)
				continue;
			for (u32 k = 0; k < bt->numBones && k < 10; k++) {
				Mtx bm;
				u32 bone = bt->bones[k];
				if (bone >= numBones)
					bone = 0;
				mtx_concat(mv, pose[bone], bm);
				GX_LoadPosMtxImm(bm, GX_PNMTX0 + k * 3);
				GX_LoadNrmMtxImm(bm, GX_PNMTX0 + k * 3);
			}
			const PackNpcVert *v = &wp->npcVerts[bt->firstVert];
			GX_SetArray(GX_VA_POS, (void *)v, sizeof(PackNpcVert));
			GX_SetArray(GX_VA_TEX0, (u8 *)v + 12, sizeof(PackNpcVert));
			GX_SetArray(GX_VA_NRM, (u8 *)v + 17, sizeof(PackNpcVert));
			model_material(wp, mat, cubeAvg);
			const u16 *idx = &wp->npcIndices[bt->firstIndex];
			GX_Begin(bt->strip ? GX_TRIANGLESTRIP : GX_TRIANGLES, GX_VTXFMT5, (u16)bt->numIndices);
			for (u32 j = 0; j < bt->numIndices; j++) {
				u16 k = idx[j];
				GX_MatrixIndex1x8(v[k].mtx);
				GX_Position1x16(k);
				GX_Normal1x16(k);
				GX_TexCoord1x16(k);
			}
			GX_End();
		}
	GX_SetCurrentMtx(GX_PNMTX0);
	unlit_channel();
	texObjs = keep;
	set_blend(0);
}

/* ------------------------------------------------------------------ the HEV flashlight */

/* Source projects a soft disc from the eye (45 degrees, 750 units, r_flashlightfov/far) and lights
 * what it hits by its texture. Here: a second pass over the visible world, the disc and a distance
 * ramp projected from the vertex positions, times the surface's texture, added on. */
static u8 *flCookie, *flRamp;
static GXTexObj flCookieTex, flRampTex;

static int flashlight_textures(void)
{
	if (flCookie)
		return 0;
	flCookie = memalign(32, 64 * 64);
	flRamp = memalign(32, 64 * 4);
	if (!flCookie || !flRamp)
		return -1;
	/* I8 is tiled 8x4 */
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			f32 dx = (x - 31.5f) / 31.5f, dy = (y - 31.5f) / 31.5f;
			f32 r = sqrtf(dx * dx + dy * dy);
			f32 v = r >= 1 ? 0 : (1 - r * r) * (0.75f + 0.25f * cosf(r * 9.0f) * (1 - r));
			int tile = (y / 4) * 8 + x / 8, in = (y % 4) * 8 + x % 8;
			flCookie[tile * 32 + in] = (u8)(v < 0 ? 0 : v > 1 ? 255 : v * 255);
		}
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 64; x++) {
			f32 s = x / 63.0f;
			f32 v = x == 0 ? 0 : (1 - s) * (1 - s);
			int tile = x / 8, in = y * 8 + x % 8;
			flRamp[tile * 32 + in] = (u8)(v * 255);
		}
	DCFlushRange(flCookie, 64 * 64);
	DCFlushRange(flRamp, 64 * 4);
	GX_InitTexObj(&flCookieTex, flCookie, 64, 64, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObj(&flRampTex, flRamp, 64, 4, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	return 0;
}

void Render_Flashlight(const Pack *p, const World *w, const Camera *cam, Mtx view)
{
	if (flashlight_textures() != 0)
		return;
	const f32 far = 750.0f, tanh = tanf(22.5f * (f32)M_PI / 180.0f);
	/* the light sits a little right of and below the eye, like the player's */
	guVector eye = {cam->origin.x + cam->right.x * 6 - cam->up.x * 4, cam->origin.y + cam->right.y * 6 - cam->up.y * 4,
	                cam->origin.z + cam->right.z * 6 - cam->up.z * 4};
	guVector at = {eye.x + cam->forward.x, eye.y + cam->forward.y, eye.z + cam->forward.z};
	Mtx L, pm, dm;
	guLookAt(L, &eye, (guVector *)&cam->up, &at);
	for (int c = 0; c < 4; c++) {
		pm[0][c] = (0.5f / tanh) * L[0][c] - 0.5f * L[2][c];
		pm[1][c] = (-0.5f / tanh) * L[1][c] - 0.5f * L[2][c];
		pm[2][c] = -L[2][c];
		dm[0][c] = -L[2][c] / far;
		dm[1][c] = 0;
		dm[2][c] = 0;
	}
	dm[1][3] = 0.5f;
	GX_LoadTexMtxImm(pm, GX_TEXMTX3, GX_MTX3x4);
	GX_LoadTexMtxImm(dm, GX_TEXMTX4, GX_MTX2x4);
	world_vtxdesc();
	GX_LoadPosMtxImm(view, GX_PNMTX0);
	no_indirect();
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(3);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX3);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX4);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&flCookieTex, GX_TEXMAP5);
	GX_LoadTexObj(&flRampTex, GX_TEXMAP6);
	GX_SetNumTevStages(3);
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){235, 225, 205, 255});
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP6, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD2, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	f32 o[3] = {eye.x, eye.y, eye.z};
	f32 fw[3] = {cam->forward.x, cam->forward.y, cam->forward.z};
	RenderStats st;
	for (u32 m = 0; m < p->numMats; m++) {
		u32 a = w->matStart[m], b = w->matStart[m + 1];
		const PackMat *mat = &p->mats[m];
		if (a == b || mat->tex < 0 || mat->kind == MAT_SKY || mat->kind == MAT_NODRAW || mat->kind == MAT_WATER ||
		    is_translucent(mat->kind))
			continue;
		GX_LoadTexObj(&texObjs[mat->tex], GX_TEXMAP0);
		for (u32 i = a; i < b; i++) {
			const PackFace *f = &p->faces[w->sorted[i]];
			/* only faces reaching into the beam's range in front of us */
			int near = 0;
			for (u32 k = 0; k < f->numverts && !near; k++) {
				const f32 *v = p->verts[f->firstvert + k].pos;
				f32 d[3] = {v[0] - o[0], v[1] - o[1], v[2] - o[2]};
				f32 along = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2];
				near = along > -64 && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < (far + 256) * (far + 256);
			}
			if (near)
				draw_face(p, f, &st);
		}
	}
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

/* a static model anywhere (break pieces): both passes, its own vertex setup */
void Render_Model(const Pack *p, u32 mdl, const f32 origin[3], const f32 R[3][3], const u8 tint[4], Mtx view)
{
	if (mdl >= p->numModels)
		return;
	RenderStats st;
	/* model vertices: indexed position, normal and texture coordinate (as in draw_props) */
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
	GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
	lights_view(view);
	u8 cb[18];
	if (!Render_CubeAt(mapPack, origin, cb))
		tint_cube(tint, cb);
	for (int pass = 0; pass < 2; pass++)
		draw_model(p, mdl, origin, R, cb, view, pass, &st);
	unlit_channel();
	world_vtxdesc();
	GX_LoadPosMtxImm(view, GX_PNMTX0);
}

/* ------------------------------------------------------------------ sprites and glows */

/* env_sprite / env_sprite_oriented / env_glow / env_lightglow, drawn after the world like Source.
 * Glow modes (3 glow, 9 world glow) ignore the depth buffer and fade out when the world hides the
 * sprite's centre (Source uses an occlusion query for this; a trace does the same job here). Glow
 * keeps its size on screen; the other modes are sized in the world (texture size x scale). */
void Render_Sprites(Mtx view, const f32 eye[3], f32 dt, int (*occluded)(const f32 a[3], const f32 b[3]))
{
	u32 n;
	Entity *ents = Ent_List(&n);
	/* camera right / up in world space: the rows of the view rotation */
	f32 rt[3] = {view[0][0], view[0][1], view[0][2]}, up[3] = {view[1][0], view[1][1], view[1][2]};
	int set = 0;
	for (u32 i = 1; i < n; i++) {
		Entity *e = &ents[i];
		if ((e->cls != EC_SPRITE && e->cls != EC_LIGHTGLOW) || e->tex <= 0 || (u32)e->tex >= numTexObjs)
			continue;
		f32 d[3] = {e->origin[0] - eye[0], e->origin[1] - eye[1], e->origin[2] - eye[2]};
		f32 dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		/* behind the camera */
		f32 fz = -(view[2][0] * e->origin[0] + view[2][1] * e->origin[1] + view[2][2] * e->origin[2] + view[2][3]);
		int glow = e->rendermode == 3 || e->rendermode == 9 || e->cls == EC_LIGHTGLOW;
		f32 target = e->enabled && fz > 1 ? 1 : 0;
		if (target > 0 && glow && occluded && occluded(eye, e->origin))
			target = 0;
		if (!glow)
			e->seen = target;
		else {
			/* Source fades glows in and out over about 0.1 s */
			f32 step = dt * 10;
			e->seen += target > e->seen ? step : -step;
			if (e->seen < 0)
				e->seen = 0;
			if (e->seen > 1)
				e->seen = 1;
		}
		if (e->seen <= 0 || fz <= 1)
			continue;
		f32 alpha = e->color[3] * e->seen;
		f32 hw, hh;
		const char *sz = Ent_KV((s32)i, "_size");
		f32 tw = 64, th = 64;
		if (sz)
			sscanf(sz, "%f %f", &tw, &th);
		if (e->cls == EC_LIGHTGLOW) {
			hw = e->glowSize[0];
			hh = e->glowSize[1];
			/* fade in between min and max distance, out past the outer distance */
			f32 a = 1;
			if (e->glowDist[1] > e->glowDist[0])
				a = (dist - e->glowDist[0]) / (e->glowDist[1] - e->glowDist[0]);
			if (a > 1)
				a = 1;
			if (a < 0)
				a = 0;
			if (e->glowDist[2] > e->glowDist[1] && dist > e->glowDist[1])
				a *= 1 - (dist - e->glowDist[1]) / (e->glowDist[2] - e->glowDist[1]);
			if (a <= 0)
				continue;
			alpha *= a;
			/* glow size is in screen terms at 1 unit: keep its screen size */
			hw *= dist / 100.0f * 0.5f;
			hh *= dist / 100.0f * 0.5f;
		} else {
			hw = tw * 0.5f * e->scale;
			hh = th * 0.5f * e->scale;
			if (e->rendermode == 3) {
				/* kRenderGlow: constant screen size (Source scales by distance / 200 at most) */
				f32 k = dist / 200.0f;
				if (k < 1)
					k = 1;
				hw *= k;
				hh *= k;
			}
		}
		const f32 *ax = rt, *ay = up;
		f32 orx[3], ory[3];
		if (e->cls == EC_SPRITE && !strcasecmp(e->classname, "env_sprite_oriented")) {
			for (int k = 0; k < 3; k++) {
				orx[k] = -e->R[k][1]; /* left -> right */
				ory[k] = e->R[k][2];
			}
			ax = orx;
			ay = ory;
		}
		if (!set) {
			GX_LoadPosMtxImm(view, GX_PNMTX0);
			GX_SetCurrentMtx(GX_PNMTX0);
			GX_ClearVtxDesc();
			GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
			GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
			GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
			GX_SetNumChans(1);
			GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
			GX_SetNumTexGens(1);
			GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
			GX_SetNumTevStages(1);
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
			GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
			GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
			GX_SetCullMode(GX_CULL_NONE);
			GX_SetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, (GXColor){0, 0, 0, 0});
			set = 1;
		}
		if (e->tex2 > 0 && (u32)e->tex2 < numTexObjs && e->enabled && e->beamLen > 0) {
			/* point_spotlight's beam: a strip along the light facing the camera, fading to its end */
			f32 dir[3] = {e->R[0][0], e->R[1][0], e->R[2][0]};
			f32 tc[3] = {eye[0] - e->origin[0], eye[1] - e->origin[1], eye[2] - e->origin[2]};
			f32 sd[3] = {dir[1] * tc[2] - dir[2] * tc[1], dir[2] * tc[0] - dir[0] * tc[2], dir[0] * tc[1] - dir[1] * tc[0]};
			f32 sl = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
			if (sl > 0.001f) {
				for (int k = 0; k < 3; k++)
					sd[k] /= sl;
				/* CPointSpotlight: the beam is as wide at both ends (its width is the cone's base, and the
				 * cone is in the texture, sprites/glow_test02), brightness 64 shading out to its end */
				f32 w0 = e->beamWidth * 0.5f, w1 = w0;
				f32 end[3] = {e->origin[0] + dir[0] * e->beamLen, e->origin[1] + dir[1] * e->beamLen,
				              e->origin[2] + dir[2] * e->beamLen};
				GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
				GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
				GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
				GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
				GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
				GX_LoadTexObj(&texObjs[e->tex2], GX_TEXMAP0);
				GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
				GX_Position3f32(e->origin[0] - sd[0] * w0, e->origin[1] - sd[1] * w0, e->origin[2] - sd[2] * w0);
				GX_Color4u8(e->color[0], e->color[1], e->color[2], 64);
				GX_TexCoord2f32(0, 0);
				GX_Position3f32(e->origin[0] + sd[0] * w0, e->origin[1] + sd[1] * w0, e->origin[2] + sd[2] * w0);
				GX_Color4u8(e->color[0], e->color[1], e->color[2], 64);
				GX_TexCoord2f32(1, 0);
				GX_Position3f32(end[0] + sd[0] * w1, end[1] + sd[1] * w1, end[2] + sd[2] * w1);
				GX_Color4u8(e->color[0], e->color[1], e->color[2], 0);
				GX_TexCoord2f32(1, 1);
				GX_Position3f32(end[0] - sd[0] * w1, end[1] - sd[1] * w1, end[2] - sd[2] * w1);
				GX_Color4u8(e->color[0], e->color[1], e->color[2], 0);
				GX_TexCoord2f32(0, 1);
				GX_End();
			}
		}
		/* additive for glow/add modes (3, 5, 9) as in Source; texture/color modes blend */
		int add = e->rendermode == 3 || e->rendermode == 5 || e->rendermode == 9 || e->cls == EC_LIGHTGLOW;
		u8 r = e->color[0], g = e->color[1], b = e->color[2];
		u8 a8 = (u8)(alpha > 255 ? 255 : alpha);
		if (add) {
			/* additive: texture x color, scaled by renderamt only (Source ignores the texture's alpha) */
			GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
			GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
			GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
		} else {
			GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
		}
		GX_SetZMode(glow ? GX_FALSE : GX_TRUE, GX_LEQUAL, GX_FALSE);
		GX_LoadTexObj(&texObjs[e->tex], GX_TEXMAP0);
		const f32 *o = e->origin;
		GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		for (int c = 0; c < 4; c++) {
			f32 sx = (c == 0 || c == 3) ? -1.0f : 1.0f, sy = (c < 2) ? 1.0f : -1.0f;
			GX_Position3f32(o[0] + ax[0] * sx * hw + ay[0] * sy * hh, o[1] + ax[1] * sx * hw + ay[1] * sy * hh,
			                o[2] + ax[2] * sx * hw + ay[2] * sy * hh);
			GX_Color4u8(r, g, b, a8);
			GX_TexCoord2f32(sx > 0 ? 1.0f : 0.0f, sy > 0 ? 0.0f : 1.0f);
		}
		GX_End();
	}
	if (set) {
		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		GX_SetCullMode(GX_CULL_NONE);
	}
}
